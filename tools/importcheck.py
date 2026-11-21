#!/usr/bin/env python3
"""Offline import check: do all imports of an arm64 Mach-O resolve against the iOS shared cache?

usage: tools/importcheck.py <macho> [--rpath-dir DIR]... [--main MACHO] [--alias OLD=NEW]... [--json] [--cache-dir DIR]

Parses LC_LOAD_*/LC_REEXPORT_DYLIB/LC_RPATH, the imports of LC_DYLD_CHAINED_FIXUPS (all three import formats) and
classic LC_DYLD_INFO bind opcodes, then resolves every import against the named library:
  * cache images: export trie, following LC_REEXPORT_DYLIB and trie re-exports (incl. renamed ones) recursively
  * @rpath/@loader_path/@executable_path libraries: found in the bundle (--rpath-dir, LC_RPATH, ../Frameworks)
    and parsed with their own export trie (LC_DYLD_EXPORTS_TRIE or LC_DYLD_INFO).
Special ordinals: self (0) = the image itself, main (-1) = --main exe (else unchecked), flat (-2) / weak-lookup (-3) =
search the image, its dependency closure and the cache images it links.
Missing non-weak symbols abort the load in dyld; missing weak ones (and everything from a missing weak library) bind to 0.
Python 3 stdlib only.
"""
import argparse
import json
import mmap
import os
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_CACHE_DIR = os.path.join(REPO, "vendor-local/23G90__iPhone17,3")

LC_SEGMENT_64 = 0x19
LC_LOAD_DYLIB = 0xC
LC_ID_DYLIB = 0xD
LC_LOAD_WEAK_DYLIB = 0x80000018
LC_REEXPORT_DYLIB = 0x8000001F
LC_LOAD_UPWARD_DYLIB = 0x80000023
LC_LAZY_LOAD_DYLIB = 0x20
LC_RPATH = 0x8000001C
LC_DYLD_INFO = 0x22
LC_DYLD_INFO_ONLY = 0x80000022
LC_DYLD_EXPORTS_TRIE = 0x80000033
LC_DYLD_CHAINED_FIXUPS = 0x80000034
DYLIB_CMDS = {LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB, LC_REEXPORT_DYLIB, LC_LOAD_UPWARD_DYLIB, LC_LAZY_LOAD_DYLIB}
DYLIB_KIND = {LC_LOAD_DYLIB: "load", LC_LOAD_WEAK_DYLIB: "weak", LC_REEXPORT_DYLIB: "reexport",
              LC_LOAD_UPWARD_DYLIB: "upward", LC_LAZY_LOAD_DYLIB: "lazy"}

EXPORT_REEXPORT = 0x08
EXPORT_STUB_AND_RESOLVER = 0x10

ORD_SELF, ORD_MAIN, ORD_FLAT, ORD_WEAK = 0, -1, -2, -3


def uleb(b, p):
    r = 0
    s = 0
    while True:
        x = b[p]
        p += 1
        r |= (x & 0x7F) << s
        s += 7
        if not x & 0x80:
            return r, p


def sleb(b, p):
    r = 0
    s = 0
    while True:
        x = b[p]
        p += 1
        r |= (x & 0x7F) << s
        s += 7
        if not x & 0x80:
            if x & 0x40:
                r -= 1 << s
            return r, p


def cstr(b, p):
    e = b.find(b"\0", p)
    return bytes(b[p:e]).decode("utf-8", "replace")


def trie_lookup(trie, sym):
    """Walk an export trie. Returns None (absent), ('sym',) or ('reexport', ordinal, importname)."""
    rem = sym.encode()
    node = 0
    n = len(trie)
    while True:
        if node >= n:
            return None
        tsize, p = uleb(trie, node)
        if not rem:
            if not tsize:
                return None
            flags, p = uleb(trie, p)
            if flags & EXPORT_REEXPORT:
                o, p = uleb(trie, p)
                name = cstr(trie, p)
                return ("reexport", o, name)
            return ("sym",)
        p += tsize
        nch = trie[p]
        p += 1
        nxt = None
        for _ in range(nch):
            e = trie.index(b"\0", p)
            edge = bytes(trie[p:e])
            child, p = uleb(trie, e + 1)
            if rem.startswith(edge):
                rem = rem[len(edge):]
                nxt = child
                break
        if nxt is None:
            return None
        node = nxt


class Image:
    """A parsed Mach-O (from a file or from the cache) with the pieces needed for resolution."""

    def __init__(self, path, data, base_off=0, linkedit_slide=None, cache=None):
        # data: object supporting slicing; for cache images `data` is a callable(va,size)->bytes
        self.path = path
        self.install_name = None
        self.dylibs = []      # (kind, name, cmd)
        self.rpaths = []
        self.segments = []    # (name, vmaddr, vmsize, fileoff, filesize)
        self.trie = b""
        self.fixups = None    # (off, size)
        self.dyld_info = None  # tuple of 10 u32
        self._parse(data, base_off, cache)

    def _parse(self, data, base_off, cache):
        hdr = data(base_off, 32)
        magic, _ct, _cs, _ft, ncmds, sizeofcmds = struct.unpack_from("<IIIIII", hdr, 0)
        if magic != 0xFEEDFACF:
            raise ValueError("%s: not a 64-bit little-endian Mach-O (magic %#x)" % (self.path, magic))
        cmds = data(base_off + 32, sizeofcmds)
        off = 0
        trie_ref = None
        for _ in range(ncmds):
            cmd, size = struct.unpack_from("<II", cmds, off)
            if cmd == LC_SEGMENT_64:
                nm = cmds[off + 8:off + 24].split(b"\0")[0].decode()
                vm, vs, fo, fs = struct.unpack_from("<QQQQ", cmds, off + 24)
                self.segments.append((nm, vm, vs, fo, fs))
            elif cmd in DYLIB_CMDS:
                noff = struct.unpack_from("<I", cmds, off + 8)[0]
                self.dylibs.append((DYLIB_KIND[cmd], cstr(cmds, off + noff), cmd))
            elif cmd == LC_ID_DYLIB:
                noff = struct.unpack_from("<I", cmds, off + 8)[0]
                self.install_name = cstr(cmds, off + noff)
            elif cmd == LC_RPATH:
                noff = struct.unpack_from("<I", cmds, off + 8)[0]
                self.rpaths.append(cstr(cmds, off + noff))
            elif cmd in (LC_DYLD_INFO, LC_DYLD_INFO_ONLY):
                self.dyld_info = struct.unpack_from("<10I", cmds, off + 8)
                if trie_ref is None and self.dyld_info[9]:
                    trie_ref = (self.dyld_info[8], self.dyld_info[9])
            elif cmd == LC_DYLD_EXPORTS_TRIE:
                trie_ref = struct.unpack_from("<II", cmds, off + 8)
            elif cmd == LC_DYLD_CHAINED_FIXUPS:
                self.fixups = struct.unpack_from("<II", cmds, off + 8)
            off += size
        self._data = data
        self._base = base_off
        self._cache = cache
        self._trie_ref = trie_ref

    def linkedit_read(self, fileoff, size):
        """Read bytes at a file offset (file images) / the matching address in __LINKEDIT (cache images)."""
        if self._cache is None:
            return self._data(self._base + fileoff, size)
        for nm, vm, vs, fo, fs in self.segments:
            if nm == "__LINKEDIT":
                return self._data(vm + (fileoff - fo), size)
        raise ValueError("no __LINKEDIT in " + self.path)

    def get_trie(self):
        if not self.trie and self._trie_ref:
            self.trie = bytes(self.linkedit_read(*self._trie_ref))
        return self.trie

    def own_name(self):
        return self.install_name or self.path


class Cache:
    def __init__(self, cache_dir):
        main = os.path.join(cache_dir, "dyld_shared_cache_arm64e")
        self.mm = {}
        self.mappings = []  # (addr, size, path, fileoff)
        self.images = {}    # path -> mach header address
        with open(main, "rb") as f:
            hdr = f.read(0x400)
        sc_off, sc_cnt = struct.unpack_from("<II", hdr, 0x188)
        files = [main]
        with open(main, "rb") as f:
            f.seek(sc_off)
            for _ in range(sc_cnt):
                e = f.read(56)
                files.append(main + e[24:56].split(b"\0")[0].decode())
        for p in files:
            with open(p, "rb") as f:
                self.mm[p] = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
            m = self.mm[p]
            moff, mcnt = struct.unpack_from("<II", m, 0x138)
            for i in range(mcnt):
                addr, size, fo = struct.unpack_from("<QQQ", m, moff + i * 56)
                self.mappings.append((addr, size, p, fo))
        m = self.mm[main]
        ioff, icnt = struct.unpack_from("<II", m, 0x1C0)
        for i in range(icnt):
            addr, _mt, _ino, poff, _pad = struct.unpack_from("<QQQII", m, ioff + i * 32)
            self.images[cstr(m, poff)] = addr
        self._img = {}

    def read(self, va, size):
        for addr, sz, p, fo in self.mappings:
            if addr <= va < addr + sz:
                o = fo + (va - addr)
                return self.mm[p][o:o + size]
        raise ValueError("unmapped cache address %#x" % va)

    def image(self, path):
        if path not in self.images:
            return None
        if path not in self._img:
            self._img[path] = Image(path, self.read, self.images[path], cache=self)
        return self._img[path]


def load_macho_file(path, want_arm64=True):
    with open(path, "rb") as f:
        raw = f.read()
    base = 0
    magic = struct.unpack_from(">I", raw, 0)[0]
    if magic in (0xCAFEBABE, 0xCAFEBABF):
        n = struct.unpack_from(">I", raw, 4)[0]
        is64 = magic == 0xCAFEBABF
        base = None
        for i in range(n):
            if is64:
                ct, cs, off, sz, al = struct.unpack_from(">iiQQI", raw, 8 + i * 32)
            else:
                ct, cs, off, sz, al = struct.unpack_from(">iiIII", raw, 8 + i * 20)
            if ct == 0x0100000C:
                base = off
                break
        if base is None:
            raise ValueError(path + ": no arm64 slice")
    return Image(path, lambda o, s: raw[o:o + s], base)


# ---------------------------------------------------------------- imports

def parse_chained_imports(img):
    """Return list of dicts for LC_DYLD_CHAINED_FIXUPS imports."""
    if not img.fixups:
        return []
    off, size = img.fixups
    cf = img.linkedit_read(off, size)
    (ver, starts_off, imports_off, symbols_off, count, fmt, sfmt) = struct.unpack_from("<7I", cf, 0)
    out = []
    for i in range(count):
        if fmt == 1:
            v = struct.unpack_from("<I", cf, imports_off + i * 4)[0]
            ordinal, weak, noff, addend = v & 0xFF, (v >> 8) & 1, v >> 9, 0
            if ordinal > 0xF0:
                ordinal -= 0x100
        elif fmt == 2:
            v, addend = struct.unpack_from("<Ii", cf, imports_off + i * 8)
            ordinal, weak, noff = v & 0xFF, (v >> 8) & 1, v >> 9
            if ordinal > 0xF0:
                ordinal -= 0x100
        elif fmt == 3:
            v, addend = struct.unpack_from("<QQ", cf, imports_off + i * 16)
            ordinal, weak, noff = v & 0xFFFF, (v >> 16) & 1, v >> 32
            if ordinal > 0xFFF0:
                ordinal -= 0x10000
        else:
            raise ValueError("unknown chained import format %d" % fmt)
        out.append({"ordinal": ordinal, "weak": bool(weak), "name": cstr(cf, symbols_off + noff),
                    "addend": addend, "source": "chained"})
    return out


def parse_bind_opcodes(img):
    """Return imports from classic LC_DYLD_INFO bind / weak_bind / lazy_bind streams."""
    if not img.dyld_info:
        return []
    (_ro, _rs, bo, bs, wo, ws, lo, ls, _eo, _es) = img.dyld_info
    out = []
    seen = set()

    def run(off, size, kind):
        if not size:
            return
        b = img.linkedit_read(off, size)
        p = 0
        ordinal = 0
        name = ""
        flags = 0
        while p < len(b):
            op = b[p]
            imm = op & 0xF
            opc = op & 0xF0
            p += 1
            if opc == 0x00:      # DONE (lazy stream has several; keep going)
                if kind != "lazy":
                    break
            elif opc == 0x10:
                ordinal = imm
            elif opc == 0x20:
                ordinal, p = uleb(b, p)
            elif opc == 0x30:
                ordinal = 0 if imm == 0 else imm - 16
            elif opc == 0x40:
                e = b.index(b"\0", p)
                name = bytes(b[p:e]).decode("utf-8", "replace")
                p = e + 1
                flags = imm
            elif opc == 0x50:    # SET_TYPE_IMM
                pass
            elif opc == 0x60:    # SET_ADDEND_SLEB
                _, p = sleb(b, p)
            elif opc == 0x70:    # SET_SEGMENT_AND_OFFSET_ULEB
                _, p = uleb(b, p)
            elif opc == 0x80:    # ADD_ADDR_ULEB
                _, p = uleb(b, p)
            elif opc in (0x90, 0xA0, 0xB0):   # DO_BIND, DO_BIND_ADD_ADDR_ULEB, DO_BIND_ADD_ADDR_IMM_SCALED
                if opc == 0xA0:
                    _, p = uleb(b, p)
                if name:
                    key = (ordinal, name, bool(flags & 1))
                    if key not in seen:
                        seen.add(key)
                        out.append({"ordinal": ordinal, "weak": bool(flags & 1), "name": name, "addend": 0,
                                    "source": kind})
            elif opc == 0xC0:    # DO_BIND_ULEB_TIMES_SKIPPING_ULEB
                _, p = uleb(b, p)
                _, p = uleb(b, p)
                if name:
                    key = (ordinal, name, bool(flags & 1))
                    if key not in seen:
                        seen.add(key)
                        out.append({"ordinal": ordinal, "weak": bool(flags & 1), "name": name, "addend": 0,
                                    "source": kind})
            elif opc == 0xD0 or opc == 0xE0 or opc == 0xF0:
                # 0xD0 THREADED (threaded bind rebase, subopcode in imm) - not used by arm64 chained-fixup binaries
                if opc == 0xD0 and imm == 0:   # SET_BIND_ORDINAL_TABLE_SIZE_ULEB
                    _, p = uleb(b, p)
            else:
                raise ValueError("bad bind opcode %#x at %d" % (op, p - 1))

    run(bo, bs, "bind")
    run(lo, ls, "lazy")
    # weak_bind: name-only entries, resolved by coalescing across images; report as weak-lookup imports
    if ws:
        b = img.linkedit_read(wo, ws)
        p = 0
        while p < len(b):
            op = b[p]
            opc = op & 0xF0
            p += 1
            if opc == 0x40:
                e = b.index(b"\0", p)
                name = bytes(b[p:e]).decode("utf-8", "replace")
                p = e + 1
                key = (ORD_WEAK, name, True)
                if key not in seen:
                    seen.add(key)
                    out.append({"ordinal": ORD_WEAK, "weak": True, "name": name, "addend": 0, "source": "weakbind"})
            elif opc in (0x20, 0x70, 0x80, 0xA0):
                _, p = uleb(b, p)
            elif opc == 0x60:
                _, p = sleb(b, p)
            elif opc == 0xC0:
                _, p = uleb(b, p)
                _, p = uleb(b, p)
    return out


# ---------------------------------------------------------------- resolver

class Resolver:
    def __init__(self, cache, rpath_dirs, main_img, exe_dir, aliases=None):
        self.aliases = aliases or {}
        self.cache = cache
        self.rpath_dirs = rpath_dirs
        self.main = main_img
        self.exe_dir = exe_dir
        self.files = {}      # real path -> Image or None
        self.lib_cache = {}  # (name, loader-path) -> Image | None

    def find_lib(self, name, loader):
        """Locate a dependency by install name. Returns (Image|None, origin) origin in cache/bundle/None."""
        name = self.aliases.get(name, name)
        key = (name, loader.path if name.startswith(("@loader_path", "@rpath")) else None)
        if key in self.lib_cache:
            return self.lib_cache[key]
        res = (None, None)
        cands = []
        loader_dir = os.path.dirname(os.path.abspath(loader.path))
        if name.startswith("@rpath/"):
            tail = name[7:]
            rps = []
            for rp in loader.rpaths + (self.main.rpaths if self.main is not loader else []):
                rps.append(rp.replace("@executable_path", self.exe_dir).replace("@loader_path", loader_dir))
            rps += self.rpath_dirs
            for d in rps:
                cands.append(os.path.join(d, tail))
        elif name.startswith("@executable_path/"):
            cands.append(os.path.join(self.exe_dir, name[17:]))
        elif name.startswith("@loader_path/"):
            cands.append(os.path.join(loader_dir, name[13:]))
        else:
            im = self.cache.image(name)
            if im is None:
                # try realpath-style alias: foo.framework/Versions/A/foo -> foo.framework/foo
                alt = name.replace("/Versions/A/", "/").replace("/Versions/C/", "/")
                im = self.cache.image(alt) if alt != name else None
            if im is not None:
                res = (im, "cache")
            else:
                cands.append(name)  # absolute path possibly in the bundle? unlikely; recorded as missing
                cands = []
        for c in cands:
            c = os.path.normpath(c)
            if os.path.isfile(c):
                if c not in self.files:
                    try:
                        self.files[c] = load_macho_file(c)
                    except Exception:
                        self.files[c] = None
                if self.files[c] is not None:
                    res = (self.files[c], "bundle")
                    break
        self.lib_cache[key] = res
        return res

    def lookup(self, img, sym, _seen=None):
        """Resolve sym in img (exports + reexports). Returns the defining image path or None."""
        if _seen is None:
            _seen = set()
        if id(img) in _seen:
            return None
        _seen.add(id(img))
        r = trie_lookup(img.get_trie(), sym)
        if r is not None:
            if r[0] == "sym":
                return img.own_name()
            ordinal, iname = r[1], r[2] or sym
            if 1 <= ordinal <= len(img.dylibs):
                tgt, _o = self.find_lib(img.dylibs[ordinal - 1][1], img)
                if tgt is None:
                    return None   # re-exported from a library we cannot find
                return self.lookup(tgt, iname, _seen)
            return None
        for kind, nm, _cmd in img.dylibs:
            if kind == "reexport":
                tgt, _o = self.find_lib(nm, img)
                if tgt is not None:
                    hit = self.lookup(tgt, sym, _seen)
                    if hit:
                        return hit
        return None

    def closure(self, img):
        """Dependency closure (BFS over all dylib load commands) as a list of Images."""
        out, seen, q = [], {id(img)}, [img]
        while q:
            cur = q.pop(0)
            out.append(cur)
            for _k, nm, _c in cur.dylibs:
                t, _o = self.find_lib(nm, cur)
                if t is not None and id(t) not in seen:
                    seen.add(id(t))
                    q.append(t)
        return out


def check(path, rpath_dirs, main_path, cache, aliases=None):
    img = load_macho_file(path)
    exe_dir = os.path.dirname(os.path.abspath(main_path or path))
    abspath = os.path.abspath(path)
    dirs = [os.path.abspath(d) for d in rpath_dirs]
    # a framework binary inside a bundle: its siblings live in the same Frameworks dir
    if "/Frameworks/" in abspath:
        dirs.append(abspath[:abspath.index("/Frameworks/") + len("/Frameworks")])
    main_img = load_macho_file(main_path) if main_path else img
    rs = Resolver(cache, dirs, main_img, exe_dir, aliases)

    imports = parse_chained_imports(img) + parse_bind_opcodes(img)
    libs = []  # per dylib in load order
    for kind, nm, _c in img.dylibs:
        tgt, origin = rs.find_lib(nm, img)
        libs.append({"ordinal": len(libs) + 1, "name": nm, "kind": kind, "found": origin,
                     "resolved": 0, "missing": 0, "missing_weak": 0, "_img": tgt})
    flat_pool = None
    special = {"self": {"resolved": 0, "missing": 0, "missing_weak": 0},
               "main": {"resolved": 0, "missing": 0, "missing_weak": 0, "unchecked": 0},
               "flat": {"resolved": 0, "missing": 0, "missing_weak": 0},
               "weak-lookup": {"resolved": 0, "missing": 0, "missing_weak": 0}}
    miss_nonweak, miss_weak = [], []
    per_lib_missing = {}

    for imp in imports:
        o, name, weak = imp["ordinal"], imp["name"], imp["weak"]
        status, where, libname = "resolved", None, None
        if o > 0:
            if o > len(libs):
                imp["status"], imp["lib"] = "bad-ordinal", "ordinal %d" % o
                miss_nonweak.append(imp)
                continue
            L = libs[o - 1]
            libname = L["name"]
            if L["_img"] is None:
                where = None
                # missing library: weak lib or weak import binds 0
                weak_eff = weak or L["kind"] == "weak"
            else:
                where = rs.lookup(L["_img"], name)
                weak_eff = weak
            bucket = L
        else:
            key = {ORD_SELF: "self", ORD_MAIN: "main", ORD_FLAT: "flat", ORD_WEAK: "weak-lookup"}[o]
            libname = "<%s>" % key
            bucket = special[key]
            weak_eff = weak
            if key == "self":
                where = rs.lookup(img, name)
            elif key == "main":
                if main_path:
                    where = rs.lookup(rs.main, name)
                else:
                    bucket["unchecked"] += 1
                    imp["status"], imp["lib"], imp["found_in"] = "unchecked", libname, None
                    continue
            else:
                if flat_pool is None:
                    flat_pool = rs.closure(img)
                where = rs.lookup(img, name)
                if not where and key == "flat":
                    for cand in flat_pool:
                        where = rs.lookup(cand, name)
                        if where:
                            break
                elif not where:
                    for cand in flat_pool:
                        where = rs.lookup(cand, name)
                        if where:
                            break
        imp["lib"] = libname
        if where:
            imp["status"], imp["found_in"] = "resolved", where
            bucket["resolved"] += 1
        else:
            imp["found_in"] = None
            if weak_eff:
                imp["status"] = "missing-weak"
                bucket["missing_weak"] += 1
                miss_weak.append(imp)
            else:
                imp["status"] = "missing"
                bucket["missing"] += 1
                miss_nonweak.append(imp)
            per_lib_missing.setdefault(libname, []).append(imp)

    for L in libs:
        L.pop("_img")
    return {
        "file": path,
        "arch": "arm64",
        "rpaths": img.rpaths,
        "rpath_dirs_searched": dirs,
        "imports_total": len(imports),
        "imports_by_source": {s: sum(1 for i in imports if i["source"] == s) for s in
                              sorted({i["source"] for i in imports})},
        "libraries": libs,
        "special": special,
        "missing_libraries": [L["name"] for L in libs if not L["found"] and L["kind"] != "weak"],
        "missing_weak_libraries": [L["name"] for L in libs if not L["found"] and L["kind"] == "weak"],
        "missing_nonweak": sorted(({"lib": i["lib"], "name": i["name"], "source": i["source"]} for i in miss_nonweak),
                                  key=lambda d: (d["lib"], d["name"])),
        "missing_weak": sorted(({"lib": i["lib"], "name": i["name"], "source": i["source"]} for i in miss_weak),
                               key=lambda d: (d["lib"], d["name"])),
    }


def print_summary(r, out=sys.stdout):
    w = lambda s="": print(s, file=out)
    w("== %s" % r["file"])
    w("imports: %d  %s" % (r["imports_total"], r["imports_by_source"]))
    w("libraries: %d   missing (non-weak): %d   missing (weak): %d" %
      (len(r["libraries"]), len(r["missing_libraries"]), len(r["missing_weak_libraries"])))
    for n in r["missing_libraries"]:
        w("  MISSING LIB      %s" % n)
    for n in r["missing_weak_libraries"]:
        w("  missing weak lib %s" % n)
    w()
    w("%-4s %-8s %-8s %6s %7s %7s  %s" % ("ord", "kind", "found", "ok", "miss", "m-weak", "library"))
    for L in r["libraries"]:
        if L["resolved"] or L["missing"] or L["missing_weak"] or not L["found"]:
            w("%-4d %-8s %-8s %6d %7d %7d  %s" % (L["ordinal"], L["kind"], L["found"] or "-", L["resolved"],
                                                   L["missing"], L["missing_weak"], L["name"]))
    for k, v in r["special"].items():
        if any(v.values()):
            w("%-4s %-8s %-8s %6d %7d %7d  <%s>%s" % ("-", "special", "-", v["resolved"], v["missing"],
                                                       v["missing_weak"], k,
                                                       "  unchecked=%d" % v["unchecked"] if v.get("unchecked") else ""))
    w()
    w("MISSING non-weak symbols (would abort load): %d" % len(r["missing_nonweak"]))
    for d in r["missing_nonweak"]:
        w("  %s: %s" % (d["lib"], d["name"]))
    w("missing weak symbols (bind to 0): %d" % len(r["missing_weak"]))
    for d in r["missing_weak"]:
        w("  %s: %s" % (d["lib"], d["name"]))


def main():
    ap = argparse.ArgumentParser(description="Check Mach-O imports against the iOS shared cache")
    ap.add_argument("macho")
    ap.add_argument("--rpath-dir", action="append", default=[], help="extra directory to resolve @rpath/ against")
    ap.add_argument("--main", help="main executable (to resolve 'main executable' ordinal imports of a framework)")
    ap.add_argument("--alias", action="append", default=[], metavar="OLD=NEW",
                    help="pretend install name OLD is NEW (what a loader-side redirect would do)")
    ap.add_argument("--cache-dir", default=DEFAULT_CACHE_DIR)
    ap.add_argument("--json", action="store_true", help="print JSON instead of the human summary")
    a = ap.parse_args()
    cache = Cache(a.cache_dir)
    aliases = dict(x.split("=", 1) for x in a.alias)
    r = check(a.macho, a.rpath_dir, a.main, cache, aliases)
    if aliases:
        r["aliases"] = aliases
    if a.json:
        json.dump(r, sys.stdout, indent=1)
        print()
    else:
        print_summary(r)
    return 0


if __name__ == "__main__":
    sys.exit(main())
