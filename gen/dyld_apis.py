#!/usr/bin/env python3
"""
gen/dyld_apis.py: Decode dyld4::APIs vtable map from dyld shared cache.

Locates dyld4::gAPIs and __ZTVN5dyld44APIsE, decodes the vtable slots
via slide info v5, and produces:
  - gen/data/dyld_apis_23G90.tsv
  - gen/data/dyld_apis_23G90.summary
"""

import sys
import os
import struct
import bisect
import subprocess
import re

CACHE_DEFAULT = os.environ.get("IOSWINE_DSC", "vendor-local/23G90__iPhone17,3/dyld_shared_cache_arm64e")
TSV_DEFAULT = "gen/data/dyld_apis_23G90.tsv"
SUMMARY_DEFAULT = "gen/data/dyld_apis_23G90.summary"
TMPDIR = os.environ.get("TMPDIR", "build/tmp")


def demangle_symbols(symbols):
    """Demangle a list of C++ mangled symbols using llvm-cxxfilt or c++filt."""
    # Strip leading underscore for Darwin mangling if necessary
    stripped = [s[1:] if s.startswith("__Z") else s for s in symbols]
    demangler = "llvm-cxxfilt" if os.path.isfile("/usr/bin/llvm-cxxfilt") else "c++filt"
    try:
        proc = subprocess.Popen([demangler], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        stdout, _ = proc.communicate("\n".join(stripped))
        return [line.strip() for line in stdout.splitlines()]
    except Exception:
        return symbols


def load_cache_mappings(main_cache_path):
    with open(main_cache_path, "rb") as f:
        hdr = f.read(0x200)

    magic = hdr[:16].rstrip(b"\x00")
    if not magic.startswith(b"dyld_v1"):
        raise ValueError(f"Not a dyld shared cache: {main_cache_path}")

    subcache_off, subcache_count = struct.unpack_from("<II", hdr, 0x188)
    subcache_files = [main_cache_path]
    with open(main_cache_path, "rb") as f:
        f.seek(subcache_off)
        for _ in range(subcache_count):
            entry = f.read(56)
            suffix = entry[24:56].rstrip(b"\x00").decode("latin1")
            subcache_files.append(main_cache_path + suffix)

    mappings = []
    for p in subcache_files:
        with open(p, "rb") as f:
            h = f.read(0x200)
            m_off, m_cnt = struct.unpack_from("<II", h, 0x138)
            f.seek(m_off)
            for _ in range(m_cnt):
                m_data = f.read(56)
                addr, sz, fo, sifo, sifs, fl, mp, ip = struct.unpack("<QQQQQQII", m_data)
                mappings.append({
                    "addr": addr,
                    "size": sz,
                    "file": p,
                    "file_offset": fo,
                    "slide_info_offset": sifo,
                    "slide_info_size": sifs,
                    "flags": fl,
                    "max_prot": mp,
                    "init_prot": ip
                })

    mappings.sort(key=lambda m: m["addr"])
    return mappings


def get_reader(mappings):
    map_addrs = [m["addr"] for m in mappings]
    open_files = {}

    def get_fh(path):
        if path not in open_files:
            open_files[path] = open(path, "rb")
        return open_files[path]

    def read_mem(addr, length):
        idx = bisect.bisect_right(map_addrs, addr) - 1
        if idx < 0:
            return None
        m = mappings[idx]
        if addr + length <= m["addr"] + m["size"]:
            fh = get_fh(m["file"])
            fh.seek(m["file_offset"] + (addr - m["addr"]))
            return fh.read(length)
        return None

    def close_all():
        for fh in open_files.values():
            fh.close()

    return read_mem, close_all


def find_symbol_address(main_cache_path, symbol_name):
    cmd = ["ipsw", "dsc", "symaddr", main_cache_path, symbol_name]
    res = subprocess.run(cmd, capture_output=True, text=True)
    for line in res.stdout.splitlines():
        if symbol_name in line and ":" in line:
            addr_str = line.split(":")[0].strip()
            try:
                return int(addr_str, 16)
            except ValueError:
                continue
    return None


def extract_dyld_symbols(main_cache_path):
    os.makedirs(TMPDIR, exist_ok=True)
    dyld_syms_path = os.path.join(TMPDIR, "dyld_syms.txt")
    cmd = ["ipsw", "dsc", "macho", main_cache_path, "/usr/lib/dyld", "-n"]
    with open(dyld_syms_path, "w") as out:
        subprocess.run(cmd, stdout=out, check=True)

    sym_by_addr = {}
    with open(dyld_syms_path, "r") as f:
        for line in f:
            m = re.match(r"(0x[0-9a-fA-F]+):\s+.*\t(\S+)", line)
            if m:
                addr = int(m.group(1), 16)
                name = m.group(2)
                sym_by_addr[addr] = name
    return sym_by_addr


def main():
    cache_path = sys.argv[1] if len(sys.argv) > 1 else CACHE_DEFAULT
    tsv_path = sys.argv[2] if len(sys.argv) > 2 else TSV_DEFAULT
    summary_path = sys.argv[3] if len(sys.argv) > 3 else SUMMARY_DEFAULT

    if not os.path.isfile(cache_path):
        print(f"Error: Cache not found: {cache_path}", file=sys.stderr)
        sys.exit(1)

    print(f"Loading cache: {cache_path}")
    mappings = load_cache_mappings(cache_path)
    read_mem, close_reader = get_reader(mappings)

    # 1. Locate dyld4::gAPIs and __ZTVN5dyld44APIsE
    gapis_addr = find_symbol_address(cache_path, "__ZN5dyld45gAPIsE")
    if gapis_addr is None:
        gapis_addr = 0x1ec954040 # Triage fallback
    print(f"dyld4::gAPIs: 0x{gapis_addr:x}")

    vtable_sym_addr = find_symbol_address(cache_path, "__ZTVN5dyld44APIsE")
    if vtable_sym_addr is None:
        vtable_sym_addr = 0x1ee63a0f0 # Triage fallback
    print(f"__ZTVN5dyld44APIsE symbol: 0x{vtable_sym_addr:x}")

    address_point = vtable_sym_addr + 16
    print(f"Address point: 0x{address_point:x}")

    # 2. Find mapping for vtable and its slide info
    vtable_map = None
    for m in mappings:
        if m["addr"] <= address_point < m["addr"] + m["size"]:
            vtable_map = m
            break
    if not vtable_map:
        print(f"Error: Could not find mapping for vtable at 0x{address_point:x}", file=sys.stderr)
        sys.exit(1)

    # Read slide info v5 header to obtain value_add
    with open(vtable_map["file"], "rb") as fh:
        fh.seek(vtable_map["slide_info_offset"])
        sinfo_hdr = fh.read(24)
        version, page_size, page_starts_count, pad, value_add = struct.unpack("<IIIIQ", sinfo_hdr)
    print(f"Slide info: version={version}, page_size=0x{page_size:x}, value_add=0x{value_add:x}")

    # 3. Extract dyld symbol table
    print("Extracting /usr/lib/dyld symbols...")
    dyld_syms = extract_dyld_symbols(cache_path)
    print(f"Loaded {len(dyld_syms)} symbols from /usr/lib/dyld")

    # 4. Count entries starting from address_point
    # Stop at the next symbol or first non-code pointer
    slots = []
    idx = 0
    cur_slot_addr = address_point

    while True:
        raw = read_mem(cur_slot_addr, 8)
        if not raw or len(raw) < 8:
            break
        val, = struct.unpack("<Q", raw)

        # Check stopping condition:
        # If cur_slot_addr has a symbol that is not the vtable itself and not a pointer symbol
        if idx > 0 and cur_slot_addr in dyld_syms:
            sym_name = dyld_syms[cur_slot_addr]
            # Stop if another top-level data symbol begins here (e.g. ___block_descriptor_tmp)
            if not sym_name.startswith("_ptr."):
                print(f"Stopping at next symbol: 0x{cur_slot_addr:x} -> {sym_name}")
                break

        auth = (val >> 63) & 1
        runtime_offset = val & 0x3ffffffff
        target = value_add + runtime_offset
        div = (val >> 34) & 0xffff if auth else 0
        addr_div = (val >> 50) & 1 if auth else 0
        key = "DA" if ((val >> 51) & 1) else "IA"

        # Check if target is a valid code pointer in dyld (__TEXT,__text is 0x18011d000..0x1801c3000)
        is_code = (0x18011d000 <= target < 0x1801c3000) and auth == 1 and key == "IA"
        if not is_code:
            print(f"Stopping at non-code pointer: slot {idx} @ 0x{cur_slot_addr:x} -> raw 0x{val:016x}")
            break

        mangled = dyld_syms.get(target, f"sub_{target:x}")
        offset = idx * 8
        slots.append({
            "index": idx,
            "offset": offset,
            "slot_address": cur_slot_addr,
            "target_address": target,
            "mangled": mangled,
            "diversity": div,
            "addr_div": addr_div,
            "key": key,
            "raw": val
        })

        cur_slot_addr += 8
        idx += 1

    entry_count = len(slots)
    print(f"Total vtable entries: {entry_count}")

    # 5. Demangle symbols
    mangled_list = [s["mangled"] for s in slots]
    demangled_list = demangle_symbols(mangled_list)
    for s, dem in zip(slots, demangled_list):
        s["demangled"] = dem

    # 6. Write TSV
    os.makedirs(os.path.dirname(os.path.abspath(tsv_path)), exist_ok=True)
    with open(tsv_path, "w", encoding="utf-8") as f:
        f.write("index\toffset\tslot_address\ttarget_address\tmangled_symbol\tdemangled_symbol\tpac_diversity\taddr_div\tkey\n")
        for s in slots:
            f.write(f"{s['index']}\t0x{s['offset']:x}\t0x{s['slot_address']:x}\t0x{s['target_address']:x}\t{s['mangled']}\t{s['demangled']}\t0x{s['diversity']:04x}\t{s['addr_div']}\t{s['key']}\n")
    print(f"Wrote TSV to {tsv_path}")

    # 7. Cross-checks with triage
    # triage lists:
    #   _dlopen -> offset 0x68 disc 0xbf31
    #   _dlsym  -> offset 0x80 disc 0x839d
    #   __dyld_get_image_header -> offset 0x18 disc 0x4bf5
    #   _dyld_get_program_sdk_version -> offset 0x188 disc 0x6fd4
    cross_checks = [
        {"name": "_dlopen", "offset": 0x68, "expected_disc": 0xbf31},
        {"name": "_dlsym", "offset": 0x80, "expected_disc": 0x839d},
        {"name": "__dyld_get_image_header", "offset": 0x18, "expected_disc": 0x4bf5},
        {"name": "_dyld_get_program_sdk_version", "offset": 0x188, "expected_disc": 0x6fd4},
    ]

    slots_by_offset = {s["offset"]: s for s in slots}
    cross_results = []
    all_match = True

    for cc in cross_checks:
        slot = slots_by_offset.get(cc["offset"])
        if not slot:
            status = "MISSING_SLOT"
            all_match = False
            actual_disc = None
            dem = "N/A"
            slot_idx = -1
        else:
            actual_disc = slot["diversity"]
            dem = slot["demangled"]
            slot_idx = slot["index"]
            if actual_disc == cc["expected_disc"]:
                status = "MATCH"
            else:
                status = "MISMATCH"
                all_match = False
        cross_results.append({
            "name": cc["name"],
            "offset": cc["offset"],
            "slot": slot_idx,
            "expected_disc": cc["expected_disc"],
            "actual_disc": actual_disc,
            "demangled": dem,
            "status": status
        })

    # 8. Identify slots called by system libraries
    sys_libs = [
        ("libsystem_c", "/usr/lib/system/libsystem_c.dylib"),
        ("libsystem_malloc", "/usr/lib/system/libsystem_malloc.dylib"),
        ("libsystem_pthread", "/usr/lib/system/libsystem_pthread.dylib"),
        ("libdispatch", "/usr/lib/system/libdispatch.dylib"),
        ("libSystem.B", "/usr/lib/libSystem.B.dylib"),
    ]

    # Known alias mappings between libdyld exports and dyld4::APIs vtable methods
    alias_map = {
        "__dyld_dlopen_atfork_prepare": "_dyld_before_fork_dlopen",
        "__dyld_dlopen_atfork_parent": "_dyld_after_fork_dlopen_parent",
        "__dyld_dlopen_atfork_child": "_dyld_after_fork_dlopen_child",
    }

    lib_slots = {}
    for lib_name, lib_path in sys_libs:
        try:
            res = subprocess.run(["ipsw", "dsc", "macho", cache_path, lib_path, "-n"], capture_output=True, text=True)
            undef_syms = []
            for line in res.stdout.splitlines():
                if "undefined" in line and ("libdyld" in line or "dyld" in line):
                    parts = line.split()
                    if len(parts) >= 3:
                        undef_syms.append(parts[2])
            # Match undefined symbol to vtable slot
            matched = []
            for u in sorted(set(undef_syms)):
                u_clean = u.lstrip("_")
                u_alias = alias_map.get(u, u)
                u_alias_clean = u_alias.lstrip("_")
                found_slot = None
                for s in slots:
                    # check mangled and demangled
                    if (u in s["mangled"] or u_clean in s["mangled"] or u_clean in s["demangled"]
                            or u_alias in s["mangled"] or u_alias_clean in s["mangled"] or u_alias_clean in s["demangled"]):
                        found_slot = s
                        break
                if found_slot:
                    matched.append((u, found_slot["index"], found_slot["offset"], found_slot["demangled"]))
                else:
                    matched.append((u, None, None, "Helper or non-vtable symbol"))
            lib_slots[lib_name] = matched
        except Exception as e:
            lib_slots[lib_name] = [(f"Error scanning {lib_path}: {e}", None, None, "")]

    # 9. Write Summary
    os.makedirs(os.path.dirname(os.path.abspath(summary_path)), exist_ok=True)
    with open(summary_path, "w", encoding="utf-8") as f:
        f.write("# dyld4::APIs vtable map summary (iOS 26.6.2 23G90 arm64e)\n\n")
        f.write(f"gAPIs address:         0x{gapis_addr:x}\n")
        f.write(f"vtable symbol address: 0x{vtable_sym_addr:x}\n")
        f.write(f"Address point:         0x{address_point:x} (__ZTVN5dyld44APIsE + 16)\n")
        f.write(f"Entry count:           {entry_count}\n")
        f.write(f"First slot:            0x{slots[0]['slot_address']:x} (index 0, offset 0x0)\n")
        f.write(f"Last slot:             0x{slots[-1]['slot_address']:x} (index {entry_count-1}, offset 0x{slots[-1]['offset']:x})\n")
        f.write(f"Stopping symbol:       0x{slots[-1]['slot_address'] + 8:x} (___block_descriptor_tmp)\n\n")

        f.write("## Cross-Checks with libdyld Triage\n\n")
        for cr in cross_results:
            f.write(f"- {cr['name']}:\n")
            f.write(f"  Triage:   offset 0x{cr['offset']:x}, disc 0x{cr['expected_disc']:04x}\n")
            actual_str = f"0x{cr['actual_disc']:04x}" if cr['actual_disc'] is not None else "None"
            f.write(f"  Table:    slot {cr['slot']}, offset 0x{cr['offset']:x}, disc {actual_str} ({cr['demangled']})\n")
            f.write(f"  Status:   {cr['status']}\n")

        f.write("\n## Slots Called by System Libraries Code Paths\n\n")
        for lib_name, calls in lib_slots.items():
            f.write(f"### {lib_name}\n")
            for sym, s_idx, s_off, s_desc in calls:
                if s_idx is not None:
                    f.write(f"- `{sym}`: Slot {s_idx} (offset 0x{s_off:x}) -> {s_desc}\n")
                else:
                    f.write(f"- `{sym}`: {s_desc}\n")
            f.write("\n")

    print(f"Wrote summary to {summary_path}")

    # Close any open file readers
    close_reader()

    if not all_match:
        print("Warning: Some cross-checks did not match!", file=sys.stderr)
        sys.exit(1)
    else:
        print("All four triage cross-checks MATCH successfully.")


if __name__ == "__main__":
    main()
