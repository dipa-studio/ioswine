#!/usr/bin/env python3
"""
gen/tls_sites.py: Offline TLS patch-site scanner for dyld shared cache.

Scans __TEXT,__text sections of all images in the dyld shared cache for:
  - kind 1: mrs Xn, tpidrro_el0 ((w & 0xffffffe0) == 0xd53bd060)
  - kind 2: mrs Xn, tpidr_el0   ((w & 0xffffffe0) == 0xd53bd040)

Outputs binary format:
  - 8-byte magic: b"IOSWTLS1"
  - 16-byte cache UUID
  - u64 count (little-endian)
  - count u64 entries sorted ascending: address | (kind << 56) (little-endian)
"""

import sys
import os
import struct
import bisect
import numpy as np

CHUNK_SIZE = 4 * 1024 * 1024

def check_file(bin_path):
    if not os.path.isfile(bin_path):
        print(f"Error: {bin_path} not found", file=sys.stderr)
        sys.exit(1)

    file_size = os.path.getsize(bin_path)
    if file_size < 32:
        print(f"Error: file size {file_size} is too small for header", file=sys.stderr)
        sys.exit(1)

    with open(bin_path, "rb") as f:
        magic = f.read(8)
        uuid = f.read(16)
        count_bytes = f.read(8)
        count, = struct.unpack("<Q", count_bytes)

        expected_size = 32 + count * 8
        if file_size != expected_size:
            print(f"Error: file size {file_size} != expected {expected_size} (count={count})", file=sys.stderr)
            sys.exit(1)

        raw_entries = f.read(count * 8)

    entries = np.frombuffer(raw_entries, dtype=np.uint64)
    # Check sorted ascending
    if len(entries) > 1:
        is_sorted = bool(np.all(entries[:-1] <= entries[1:]))
    else:
        is_sorted = True

    kinds = entries >> 56
    kind1_count = int(np.count_nonzero(kinds == 1))
    kind2_count = int(np.count_nonzero(kinds == 2))
    other_count = int(np.count_nonzero((kinds != 1) & (kinds != 2)))

    print("=== TLS Sites Header Check ===")
    print(f"Magic:       {magic.decode('latin1', errors='replace')}")
    print(f"Cache UUID:  {uuid.hex()}")
    print(f"Total count: {count} (kind 1: {kind1_count}, kind 2: {kind2_count}, other: {other_count})")
    print(f"Sorted:      {is_sorted}")

    print("\nFirst 5 entries:")
    for idx in range(min(5, len(entries))):
        val = int(entries[idx])
        va = val & 0x00ffffffffffffff
        k = val >> 56
        print(f"  [{idx}] 0x{va:09x} (kind {k}) -> raw: 0x{val:016x}")


def scan_cache(main_cache_path, out_bin_path):
    if not os.path.isfile(main_cache_path):
        print(f"Error: Shared cache main file not found: {main_cache_path}", file=sys.stderr)
        sys.exit(1)

    with open(main_cache_path, "rb") as f:
        hdr = f.read(0x200)

    magic = hdr[:16].rstrip(b"\x00")
    if not magic.startswith(b"dyld_v1"):
        print(f"Error: Not a valid dyld shared cache: {magic}", file=sys.stderr)
        sys.exit(1)

    uuid = hdr[0x58:0x68]
    subcache_off, subcache_count = struct.unpack_from("<II", hdr, 0x188)
    images_off, images_count = struct.unpack_from("<II", hdr, 0x1c0)

    subcache_files = [main_cache_path]
    with open(main_cache_path, "rb") as f:
        f.seek(subcache_off)
        for _ in range(subcache_count):
            entry = f.read(56)
            suffix = entry[24:56].rstrip(b"\x00").decode("latin1")
            subcache_files.append(main_cache_path + suffix)

    mappings = []
    for file_path in subcache_files:
        with open(file_path, "rb") as f:
            h = f.read(0x200)
            m_off, m_cnt = struct.unpack_from("<II", h, 0x138)
            f.seek(m_off)
            for _ in range(m_cnt):
                m_data = f.read(56)
                addr, sz, fo, sifo, sifs, fl, mp, ip = struct.unpack("<QQQQQQII", m_data)
                mappings.append((addr, sz, file_path, fo, ip))

    mappings.sort(key=lambda x: x[0])
    map_addrs = [m[0] for m in mappings]

    open_files = {}
    def get_file_handle(path):
        if path not in open_files:
            open_files[path] = open(path, "rb")
        return open_files[path]

    def read_vaddr(va, length):
        idx = bisect.bisect_right(map_addrs, va) - 1
        if idx < 0:
            return None
        m_addr, m_sz, m_file, m_fo, m_prot = mappings[idx]
        if va + length <= m_addr + m_sz:
            fh = get_file_handle(m_file)
            fh.seek(m_fo + (va - m_addr))
            return fh.read(length)
        return None

    # Read image header addresses
    image_addrs = []
    with open(main_cache_path, "rb") as f:
        f.seek(images_off)
        for _ in range(images_count):
            entry = f.read(32)
            load_addr, = struct.unpack("<Q", entry[:8])
            image_addrs.append(load_addr)

    text_sections = []
    for img_addr in image_addrs:
        hdr_data = read_vaddr(img_addr, 32)
        if not hdr_data:
            continue
        magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved = struct.unpack("<IIIIIIII", hdr_data)
        cmds_data = read_vaddr(img_addr + 32, sizeofcmds)
        if not cmds_data:
            continue
        off = 0
        for _ in range(ncmds):
            cmd, cmdsize = struct.unpack_from("<II", cmds_data, off)
            if cmd == 0x19: # LC_SEGMENT_64
                nsects, = struct.unpack_from("<I", cmds_data, off + 64)
                sect_off = off + 72
                for _ in range(nsects):
                    sname = cmds_data[sect_off:sect_off+16].rstrip(b"\x00")
                    smeg = cmds_data[sect_off+16:sect_off+32].rstrip(b"\x00")
                    saddr, ssize = struct.unpack_from("<QQ", cmds_data, sect_off+32)
                    if smeg == b"__TEXT" and sname == b"__text" and ssize > 0:
                        text_sections.append((saddr, ssize))
                    sect_off += 80
            off += cmdsize

    kind1_sites = []
    kind2_sites = []

    for saddr, ssize in text_sections:
        raw = read_vaddr(saddr, ssize)
        if not raw:
            print(f"Warning: Failed to read text section at 0x{saddr:x} (size {ssize})", file=sys.stderr)
            continue
        nwords = len(raw) // 4
        arr = np.frombuffer(raw[:nwords*4], dtype=np.uint32)
        masked = arr & 0xffffffe0
        idx1 = np.where(masked == 0xd53bd060)[0]
        idx2 = np.where(masked == 0xd53bd040)[0]
        for i in idx1:
            kind1_sites.append(saddr + int(i) * 4)
        for i in idx2:
            kind2_sites.append(saddr + int(i) * 4)

    # Close open cache file handles
    for fh in open_files.values():
        fh.close()

    print(f"Scan complete:")
    print(f"  Kind 1 (mrs Xn, tpidrro_el0): {len(kind1_sites)} (expected 10640)")
    print(f"  Kind 2 (mrs Xn, tpidr_el0):   {len(kind2_sites)} (expected 38)")
    total = len(kind1_sites) + len(kind2_sites)
    print(f"  Total:                       {total} (expected 10678)")

    if len(kind1_sites) != 10640:
        print(f"Notice: Kind 1 count mismatch: got {len(kind1_sites)}, expected 10640", file=sys.stderr)
    if len(kind2_sites) != 38:
        print(f"Notice: Kind 2 count mismatch: got {len(kind2_sites)}, expected 38", file=sys.stderr)

    entries = [va | (1 << 56) for va in kind1_sites] + [va | (2 << 56) for va in kind2_sites]
    entries.sort()

    os.makedirs(os.path.dirname(os.path.abspath(out_bin_path)), exist_ok=True)
    with open(out_bin_path, "wb") as f:
        # 8-byte magic b"IOSWTLS1", 16-byte UUID, u64 count
        f.write(b"IOSWTLS1")
        f.write(uuid)
        f.write(struct.pack("<Q", len(entries)))
        arr = np.array(entries, dtype=np.uint64)
        f.write(arr.tobytes())

    print(f"Wrote {len(entries)} entries to {out_bin_path}")


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--check":
        check_file(sys.argv[2])
    elif len(sys.argv) == 3:
        scan_cache(sys.argv[1], sys.argv[2])
    else:
        print("Usage:")
        print("  gen/tls_sites.py <cache main file> <out.bin>")
        print("  gen/tls_sites.py --check <out.bin>")
        sys.exit(1)


if __name__ == "__main__":
    main()
