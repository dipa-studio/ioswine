#!/usr/bin/env python3
import os
import sys
import struct
import subprocess

CACHE_DIR = "vendor-local/23G90__iPhone17,3"
MAIN_CACHE = os.path.join(CACHE_DIR, "dyld_shared_cache_arm64e")

def read_uleb(data, off):
    res = 0
    shift = 0
    while True:
        b = data[off]
        off += 1
        res |= (b & 0x7f) << shift
        shift += 7
        if not (b & 0x80):
            break
    return res, off

class DSCCache:
    def __init__(self):
        self.mappings = [] # list of (addr, size, file_path, file_off, flags, initprot, maxprot)
        self.images = {} # name -> addr
        self._load()

    def _load(self):
        with open(MAIN_CACHE, "rb") as f:
            hdr = f.read(0x400)
        # subcache info
        subcache_off = struct.unpack_from("<I", hdr, 0x188)[0]
        subcache_cnt = struct.unpack_from("<I", hdr, 0x18c)[0]
        
        # read subcache entries
        subcache_files = [MAIN_CACHE]
        with open(MAIN_CACHE, "rb") as f:
            f.seek(subcache_off)
            for _ in range(subcache_cnt):
                entry = f.read(16 + 8 + 32)
                suffix = entry[24:56].split(b"\x00")[0].decode("ascii")
                subcache_files.append(MAIN_CACHE + suffix)

        # map mappings from all subcaches
        for sc_path in subcache_files:
            with open(sc_path, "rb") as f:
                sc_hdr = f.read(0x200)
                mapslide_off = struct.unpack_from("<I", sc_hdr, 0x138)[0]
                mapslide_cnt = struct.unpack_from("<I", sc_hdr, 0x13c)[0]
                if mapslide_cnt > 0:
                    f.seek(mapslide_off)
                    for _ in range(mapslide_cnt):
                        mdata = f.read(8*6 + 4*2)
                        addr, size, file_off, slide_off, slide_size, flags, maxprot, initprot = struct.unpack("<QQQQQQII", mdata)
                        self.mappings.append((addr, size, sc_path, file_off, flags, initprot, maxprot))

        # read images from main cache
        images_off = struct.unpack_from("<I", hdr, 0x1c0)[0]
        images_cnt = struct.unpack_from("<I", hdr, 0x1c4)[0]
        with open(MAIN_CACHE, "rb") as f:
            f.seek(images_off)
            entries = []
            for _ in range(images_cnt):
                e = f.read(8*3 + 4*2)
                addr, mtime, inode, path_off, pad = struct.unpack("<QQQII", e)
                entries.append((addr, path_off))
            for addr, path_off in entries:
                f.seek(path_off)
                # read null-terminated string
                chars = []
                while True:
                    b = f.read(64)
                    if not b:
                        break
                    idx = b.find(b"\x00")
                    if idx != -1:
                        chars.append(b[:idx])
                        break
                    chars.append(b)
                path = b"".join(chars).decode("ascii", errors="replace")
                self.images[path] = addr

    def va_to_file_off(self, va):
        for addr, size, sc_path, file_off, flags, initprot, maxprot in self.mappings:
            if addr <= va < addr + size:
                return sc_path, file_off + (va - addr)
        return None, None

    def read_bytes(self, va, size):
        sc_path, off = self.va_to_file_off(va)
        if not sc_path:
            return None
        with open(sc_path, "rb") as f:
            f.seek(off)
            return f.read(size)

    def read_uint64(self, va):
        b = self.read_bytes(va, 8)
        if b and len(b) == 8:
            return struct.unpack("<Q", b)[0]
        return None

    def read_uint32(self, va):
        b = self.read_bytes(va, 4)
        if b and len(b) == 4:
            return struct.unpack("<I", b)[0]
        return None

    def read_cstring(self, va, max_len=256):
        sc_path, off = self.va_to_file_off(va)
        if not sc_path:
            return ""
        with open(sc_path, "rb") as f:
            f.seek(off)
            b = f.read(max_len)
            idx = b.find(b"\x00")
            if idx != -1:
                b = b[:idx]
            return b.decode("utf-8", errors="replace")

    def lookup_symbol(self, mh, sym):
        raw = self.read_bytes(mh, 0x1000)
        magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved = struct.unpack("<IIIIIIII", raw[:32])
        cmd_data = self.read_bytes(mh + 32, sizeofcmds)
        off = 0
        le_slide = 0
        et_dataoff = 0
        et_datasize = 0
        for _ in range(ncmds):
            cmd, cmdsize = struct.unpack_from("<II", cmd_data, off)
            if cmd == 0x19: # LC_SEGMENT_64
                segname = cmd_data[off+8:off+24].split(b"\x00")[0].decode("ascii")
                vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<QQQQ", cmd_data, off + 24)
                if segname == "__LINKEDIT":
                    le_slide = vmaddr - fileoff
            elif cmd == 0x80000033: # LC_DYLD_EXPORTS_TRIE
                et_dataoff, et_datasize = struct.unpack_from("<II", cmd_data, off + 8)
            off += cmdsize
        if not et_datasize:
            return None
        trie_va = le_slide + et_dataoff
        trie = self.read_bytes(trie_va, et_datasize)
        if not trie:
            return None
        
        # Walk trie
        p = 0
        sym_rem = sym
        while p < len(trie):
            tsize, p = read_uleb(trie, p)
            if not sym_rem:
                if tsize == 0:
                    return None
                flags, p = read_uleb(trie, p)
                if flags & 0x08: # REEXPORT
                    return None
                if flags & 0x10: # STUB_AND_RESOLVER
                    stub, p = read_uleb(trie, p)
                    return mh + stub
                v, p = read_uleb(trie, p)
                return v if (flags & 3) == 2 else mh + v
            p += tsize
            nchildren = trie[p]
            p += 1
            matched = False
            for _ in range(nchildren):
                edge_start = p
                while trie[p] != 0:
                    p += 1
                edge = trie[edge_start:p].decode("ascii", errors="replace")
                p += 1
                child_node, p = read_uleb(trie, p)
                if not matched and sym_rem.startswith(edge):
                    sym_rem = sym_rem[len(edge):]
                    p = child_node
                    matched = True
                    break
            if not matched:
                return None
        return None

    def disass(self, va, num_bytes=64):
        code = self.read_bytes(va, num_bytes)
        if not code:
            return "Cannot read memory at " + hex(va)
        tmpdir = os.environ.get("TMPDIR", "build/tmp")
        os.makedirs(tmpdir, exist_ok=True)
        bin_path = os.path.join(tmpdir, "disass.bin")
        with open(bin_path, "wb") as f:
            f.write(code)
        cmd = [
            "aarch64-linux-gnu-objdump",
            "-D", "-b", "binary", "-m", "aarch64",
            f"--adjust-vma={hex(va)}",
            bin_path
        ]
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        lines = res.stdout.splitlines()
        out = []
        for line in lines:
            if "\t" in line and ":" in line:
                out.append(line)
        return "\n".join(out)

if __name__ == "__main__":
    dsc = DSCCache()
    mh_objc = dsc.images.get("/usr/lib/libobjc.A.dylib")
    print("libobjc mh:", hex(mh_objc))
    addr = dsc.lookup_symbol(mh_objc, "_objc_init")
    print("_objc_init:", hex(addr) if addr else "not found")
    addr2 = dsc.lookup_symbol(mh_objc, "_objc_autoreleasePoolPush")
    print("_objc_autoreleasePoolPush:", hex(addr2) if addr2 else "not found")
