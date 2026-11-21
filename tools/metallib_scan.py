#!/usr/bin/env python3
"""metallib_scan.py - parse a .metallib file and print its functions,
intrinsics, and types.

Host Python, standard library only. No Apple binaries are needed: it
reads the container format directly.

Usage:
  python3 metallib_scan.py <file.metallib> [--json]
  python3 metallib_scan.py /path/to/default.metallib

On the device side (no Apple file can be shipped to us), run it against
a real QuartzCore default.metallib like this:

  python3 metallib_scan.py \
    /path/to/QuartzCore.framework/default.metallib > scan.txt

or, from Termux on the device, copy the file out of the app bundle
first (it is a plain data file inside the framework):

  cp /path/to/QuartzCore.framework/default.metallib /sdcard/Download/
  python3 metallib_scan.py /sdcard/Download/default.metallib

What it prints:
  - header: platform, metallib version, library type, target OS and
    version, section offsets and sizes
  - header extension tags, when present (HDYN/VLST/ILST/HSRD/UUID)
  - per function: name, type (vertex/fragment/kernel/...), bitcode
    offset and size, AIR and language versions, SHA-256 hash when
    present, verified against the bitcode blob (a mismatch is
    reported; a mismatched blob must not be translated), and the
    public/private metadata tags at its OFFT offsets
  - per function bitcode blob: wrapper magic check, wrapper version /
    offset / size / CPU type
  - possible AIR intrinsic references: printable ASCII strings in the
    bitcode starting with "air." (HEURISTIC: the bitcode is a binary
    stream, so a string scan can miss intrinsics that are referenced
    by id, and can surface strings that are not calls. A real
    intrinsic listing needs the bitcode reader to walk the value
    symtab; that is a later stage.)
  - Metal data type names for single-byte type codes found in VATY /
    ARGR metadata payloads (best effort; the exact payload layout of
    those tags is not public).

Format notes (see src/metallib.h for the full layout):
  - 88-byte header, "MTLB" magic, all little-endian.
  - Function list: u32 count, then tag groups. A tag group is a u32
    size (the span from the size field itself to the next group's
    size field) followed by tags; a tag is a FourCC + u16 size +
    payload, and a group ends with ENDT, a bare 4-byte code with no
    size field. This tool scans groups to ENDT like the C reader and
    like the YuAo MetalLibraryArchive parser; it does not trust the
    header's function-list size as a group bound (that size lands on
    the last group's ENDT in real files).
  - OFFT is (u64, u64, u64): offsets of this function's data inside
    the public metadata section, the private metadata section, and
    the bitcode section, each relative to its own section start.
  - The embedded source code section item count is a UInt32, not a
    UInt16 as the YuAo doc table says (the parser reads UInt32).
"""

import hashlib
import json
import struct
import sys

HEADER_SIZE = 88
MAGIC = b"MTLB"

PLATFORMS = {0x0001: "iOS", 0x8001: "macOS"}
LIB_TYPES = {0: "executable", 1: "Core Image", 2: "dynamic",
             3: "symbol companion"}
TARGET_OS = {0x00: "unknown", 0x81: "macOS", 0x82: "iOS", 0x83: "tvOS",
             0x84: "watchOS", 0x85: "bridgeOS", 0x86: "macCatalyst",
             0x87: "iOS Simulator", 0x88: "tvOS Simulator",
             0x89: "watchOS Simulator"}
FUNC_TYPES = {0: "vertex", 1: "fragment", 2: "kernel",
              3: "unqualified", 4: "visible", 5: "extern",
              6: "intersection"}

# Metal data type table (YuAo MetalLibraryArchive doc).
METAL_TYPES = {
    0x00: "None", 0x01: "Struct", 0x02: "Array", 0x03: "Float",
    0x04: "Float2", 0x05: "Float3", 0x06: "Float4",
    0x07: "Float2x2", 0x08: "Float2x3", 0x09: "Float2x4",
    0x0A: "Float3x2", 0x0B: "Float3x3", 0x0C: "Float3x4",
    0x0D: "Float4x2", 0x0E: "Float4x3", 0x0F: "Float4x4",
    0x10: "Half", 0x11: "Half2", 0x12: "Half3", 0x13: "Half4",
    0x14: "Half2x2", 0x15: "Half2x3", 0x16: "Half2x4",
    0x17: "Half3x2", 0x18: "Half3x3", 0x19: "Half3x4",
    0x1A: "Half4x2", 0x1B: "Half4x3", 0x1C: "Half4x4",
    0x1D: "Int", 0x1E: "Int2", 0x1F: "Int3", 0x20: "Int4",
    0x21: "UInt", 0x22: "UInt2", 0x23: "UInt3", 0x24: "UInt4",
    0x25: "Short", 0x26: "Short2", 0x27: "Short3", 0x28: "Short4",
    0x29: "UShort", 0x2A: "UShort2", 0x2B: "UShort3", 0x2C: "UShort4",
    0x2D: "Char", 0x2E: "Char2", 0x2F: "Char3", 0x30: "Char4",
    0x31: "UChar", 0x32: "UChar2", 0x33: "UChar3", 0x34: "UChar4",
    0x35: "Bool", 0x36: "Bool2", 0x37: "Bool3", 0x38: "Bool4",
    0x3A: "Texture", 0x3B: "Sampler", 0x3C: "Pointer",
    0x3E: "R8Unorm", 0x3F: "R8Snorm", 0x40: "R16Unorm",
    0x41: "R16Snorm", 0x42: "RG8Unorm", 0x43: "RG8Snorm",
    0x44: "RG16Unorm", 0x45: "RG16Snorm", 0x46: "RGBA8Unorm",
    0x47: "RGBA8Unorm_sRGB", 0x48: "RGBA8Snorm", 0x49: "RGBA16Unorm",
    0x4A: "RGBA16Snorm", 0x4B: "RG10A2Unorm", 0x4C: "RG11B10Float",
    0x4D: "RGB9E5Float", 0x4E: "RenderPipeline",
    0x4F: "ComputePipeline", 0x50: "IndirectCommandBuffer",
    0x51: "Long", 0x52: "Long2", 0x53: "Long3", 0x54: "Long4",
    0x55: "ULong", 0x56: "ULong2", 0x57: "ULong3", 0x58: "ULong4",
    0x59: "Double", 0x5A: "Double2", 0x5B: "Double3", 0x5C: "Double4",
    0x5D: "Float8", 0x5E: "Float16", 0x5F: "Half8", 0x60: "Half16",
    0x61: "Int8", 0x62: "Int16", 0x63: "UInt8", 0x64: "UInt16",
    0x65: "Short8", 0x66: "Short16", 0x67: "UShort8",
    0x68: "UShort16", 0x69: "Char8", 0x6A: "Char16", 0x6B: "UChar8",
    0x6C: "UChar16", 0x6D: "Long8", 0x6E: "Long16", 0x6F: "ULong8",
    0x70: "ULong16", 0x71: "Double8", 0x72: "Double16",
    0x73: "VisibleFunctionTable", 0x74: "IntersectionFunctionTable",
    0x75: "PrimitiveAccelerationStructure",
    0x76: "InstanceAccelerationStructure", 0x77: "Bool8",
    0x78: "Bool16",
}


class ParseError(Exception):
    pass


def u16(data, off):
    return struct.unpack_from("<H", data, off)[0]


def u32(data, off):
    return struct.unpack_from("<I", data, off)[0]


def u64(data, off):
    return struct.unpack_from("<Q", data, off)[0]


def check_range(data, off, size, what):
    if off > len(data) or size > len(data) - off:
        raise ParseError("%s runs past the file (off=%d size=%d)" %
                         (what, off, size))


def scan_tags(data, start, end):
    """Scan FourCC + u16 size tags until ENDT or end. Returns a list
    of (code, payload_off, payload_size). ENDT is a bare 4-byte code."""
    tags = []
    pos = start
    while pos < end:
        if pos + 4 > end:
            raise ParseError("tag code runs past its group at %d" % pos)
        code = data[pos:pos + 4].decode("latin-1")
        if code == "ENDT":
            tags.append(("ENDT", pos, 0))
            return tags, pos + 4
        if pos + 6 > end:
            raise ParseError("tag size runs past its group at %d" % pos)
        size = u16(data, pos + 4)
        if size > end - (pos + 6):
            raise ParseError("tag %s size %d runs past its group" %
                             (code, size))
        tags.append((code, pos + 6, size))
        pos += 6 + size
    return tags, pos


def scan_group(data, pos, file_end):
    """One tag group: u32 size, then tags to ENDT. Returns
    (tags, next_pos). The size includes its own 4 bytes for function
    groups; it is only a sanity bound here, ENDT is authoritative."""
    if pos + 4 > file_end:
        raise ParseError("tag group size runs past the file at %d" % pos)
    size = u32(data, pos)
    if size < 4:
        raise ParseError("tag group size %d too small at %d" % (size, pos))
    if size > file_end - pos:
        raise ParseError("tag group size %d runs past the file" % size)
    tags, after = scan_tags(data, pos + 4, pos + size)
    if not any(c == "ENDT" for c, _, _ in tags):
        raise ParseError("tag group at %d has no ENDT" % pos)
    return tags, pos + size


def parse_header(data):
    if len(data) < HEADER_SIZE:
        raise ParseError("file smaller than the 88-byte header")
    if data[0:4] != MAGIC:
        raise ParseError("bad magic, not a metallib file")
    h = {
        "platform": u16(data, 4),
        "platform_name": PLATFORMS.get(u16(data, 4), "unknown"),
        "version_major": u16(data, 6),
        "version_minor": u16(data, 8),
        "lib_type": data[10],
        "lib_type_name": LIB_TYPES.get(data[10], "unknown"),
        "target_os": data[11],
        "target_os_name": TARGET_OS.get(data[11], "unknown"),
        "os_major": u16(data, 12),
        "os_minor": u16(data, 14),
        "file_size": u64(data, 16),
        "func_list_off": u64(data, 24),
        "func_list_size": u64(data, 32),
        "pub_meta_off": u64(data, 40),
        "pub_meta_size": u64(data, 48),
        "priv_meta_off": u64(data, 56),
        "priv_meta_size": u64(data, 64),
        "bitcode_off": u64(data, 72),
        "bitcode_size": u64(data, 80),
    }
    if h["file_size"] != len(data):
        raise ParseError("header file size %d != actual size %d" %
                         (h["file_size"], len(data)))
    for name in ("func_list", "pub_meta", "priv_meta", "bitcode"):
        check_range(data, h[name + "_off"], h[name + "_size"],
                    name + " section")
    return h


def parse_function_list(data, h):
    fl_off = h["func_list_off"]
    count = u32(data, fl_off)
    funcs = []
    pos = fl_off + 4
    running = 0
    for _ in range(count):
        tags, pos = scan_group(data, pos, len(data))
        fn = {"name": None, "type": None, "type_name": None,
              "bitcode_size": None, "offt": None, "hash": None,
              "air_major": None, "air_minor": None,
              "lang_major": None, "lang_minor": None,
              "layr": None, "tess": None, "soff": None}
        for code, off, size in tags:
            if code == "NAME":
                raw = data[off:off + size]
                nul = raw.find(b"\x00")
                fn["name"] = raw[:nul if nul >= 0 else size].decode(
                    "utf-8", "replace")
            elif code == "TYPE" and size >= 1:
                fn["type"] = data[off]
                fn["type_name"] = FUNC_TYPES.get(data[off], "unknown")
            elif code == "MDSZ" and size == 8:
                fn["bitcode_size"] = u64(data, off)
            elif code == "OFFT" and size == 24:
                fn["offt"] = (u64(data, off), u64(data, off + 8),
                              u64(data, off + 16))
            elif code == "HASH" and size == 32:
                fn["hash"] = data[off:off + 32].hex()
            elif code == "VERS" and size == 8:
                fn["air_major"] = u16(data, off)
                fn["air_minor"] = u16(data, off + 2)
                fn["lang_major"] = u16(data, off + 4)
                fn["lang_minor"] = u16(data, off + 6)
            elif code == "LAYR" and size >= 1:
                fn["layr"] = data[off]
            elif code == "TESS" and size >= 1:
                fn["tess"] = data[off]
            elif code == "SOFF" and size == 8:
                fn["soff"] = u64(data, off)
        if fn["name"] is None:
            raise ParseError("function entry without a NAME tag")
        # Bitcode offset: OFFT wins, else sequential layout.
        if fn["offt"] is not None:
            rel = fn["offt"][2]
        else:
            rel = running
        # Bitcode size: MDSZ wins, else next OFFT offset or section end.
        if fn["bitcode_size"] is None:
            fn["bitcode_size"] = None  # filled below, needs next fn
        fn["_rel"] = rel
        fn["_running"] = running
        running += fn["bitcode_size"] or 0
        funcs.append(fn)
    # MDSZ fallback pass.
    for i, fn in enumerate(funcs):
        if fn["bitcode_size"] is not None:
            continue
        nxt = h["bitcode_size"]
        for later in funcs[i + 1:]:
            if later["offt"] is not None:
                nxt = later["offt"][2]
                break
        fn["bitcode_size"] = nxt - fn["_rel"]
    for fn in funcs:
        fn["bitcode_off"] = h["bitcode_off"] + fn["_rel"]
        del fn["_rel"]
        del fn["_running"]
        check_range(data, fn["bitcode_off"], fn["bitcode_size"],
                    "bitcode for %s" % fn["name"])
    return funcs


def parse_header_extension(data, h):
    ext_off = h["func_list_off"] + h["func_list_size"] + 4
    if ext_off == h["pub_meta_off"]:
        return []
    if ext_off >= len(data):
        return []
    tags, _ = scan_tags(data, ext_off, len(data))
    out = []
    for code, off, size in tags:
        item = {"tag": code, "size": size}
        if code in ("HDYN", "VLST", "ILST", "HSRD", "HSRC") and size == 16:
            item["offset"] = u64(data, off)
            item["section_size"] = u64(data, off + 8)
        elif code == "UUID" and size == 16:
            item["uuid"] = data[off:off + 16].hex()
        out.append(item)
        if code == "ENDT":
            break
    return out


def parse_metadata_group(data, section_off, section_size, rel_off):
    """Walk one tag group inside a metadata section at a relative
    offset. Returns a list of (code, size, payload_hex). The group's
    u32 size field is NOT a bound here: in real files an empty
    metadata group is [u32 4][ENDT], spanning 8 bytes while its size
    says 4, so the size means something different than for function
    groups. Like the YuAo parser, scan tags until ENDT, bounded by
    the section end."""
    if rel_off is None:
        return []
    pos = section_off + rel_off
    if pos + 4 > len(data):
        return []
    tags, _ = scan_tags(data, pos + 4,
                        min(section_off + section_size, len(data)))
    out = []
    for code, off, size in tags:
        item = {"tag": code, "size": size,
                "payload_hex": data[off:off + min(size, 16)].hex()}
        if code in ("VATY", "ARGR"):
            # Best effort: decode payload bytes as Metal type codes.
            item["types_best_effort"] = [
                METAL_TYPES.get(b, "0x%02x" % b)
                for b in data[off:off + size]]
        out.append(item)
    return out


def scan_air_strings(blob):
    """Printable ASCII runs of length >= 4; return the ones starting
    with 'air.' (possible intrinsic references, heuristic)."""
    runs = []
    cur = bytearray()
    for byte in blob:
        if 32 <= byte < 127:
            cur.append(byte)
        else:
            if len(cur) >= 4:
                runs.append(bytes(cur).decode("ascii"))
            cur = bytearray()
    if len(cur) >= 4:
        runs.append(bytes(cur).decode("ascii"))
    seen = set()
    out = []
    for s in runs:
        if s.startswith("air.") and s not in seen:
            seen.add(s)
            out.append(s)
    return sorted(out)


def parse_metallib(path):
    with open(path, "rb") as f:
        data = f.read()
    h = parse_header(data)
    funcs = parse_function_list(data, h)
    ext = parse_header_extension(data, h)
    for fn in funcs:
        blob = data[fn["bitcode_off"]:
                    fn["bitcode_off"] + fn["bitcode_size"]]
        fn["bitcode_magic"] = "unknown"
        fn["wrapper"] = None
        if len(blob) >= 4:
            magic = u32(blob, 0)
            if magic == 0x0B17C0DE:
                fn["bitcode_magic"] = "wrapper"
                if len(blob) >= 20:
                    fn["wrapper"] = {
                        "version": u32(blob, 4),
                        "offset": u32(blob, 8),
                        "size": u32(blob, 12),
                        "cpu_type": u32(blob, 16),
                    }
            elif magic == 0xDEC04342:
                fn["bitcode_magic"] = "raw BC stream"
        fn["air_intrinsics_heuristic"] = scan_air_strings(blob)
        # Verify the bitcode against the HASH tag. A mismatch is
        # reported, not fatal: the scan is an inventory, and a
        # mismatch means the blob must not be translated.
        if fn["hash"] is not None:
            fn["hash_actual"] = hashlib.sha256(blob).hexdigest()
            fn["hash_match"] = (fn["hash_actual"] == fn["hash"])
        else:
            fn["hash_actual"] = None
            fn["hash_match"] = None
        if fn["offt"] is not None:
            fn["public_metadata"] = parse_metadata_group(
                data, h["pub_meta_off"], h["pub_meta_size"],
                fn["offt"][0])
            fn["private_metadata"] = parse_metadata_group(
                data, h["priv_meta_off"], h["priv_meta_size"],
                fn["offt"][1])
        else:
            fn["public_metadata"] = []
            fn["private_metadata"] = []
    return {"path": path, "header": h, "header_extension": ext,
            "functions": funcs}


def print_report(rep):
    h = rep["header"]
    print("metallib: %s" % rep["path"])
    print("header: platform %s (0x%04x), metallib %d.%d, type %s, "
          "target OS %s %d.%d, file size %d" % (
              h["platform_name"], h["platform"],
              h["version_major"], h["version_minor"],
              h["lib_type_name"], h["target_os_name"],
              h["os_major"], h["os_minor"], h["file_size"]))
    print("sections: function list off=%d size=%d; public metadata "
          "off=%d size=%d; private metadata off=%d size=%d; bitcode "
          "off=%d size=%d" % (
              h["func_list_off"], h["func_list_size"],
              h["pub_meta_off"], h["pub_meta_size"],
              h["priv_meta_off"], h["priv_meta_size"],
              h["bitcode_off"], h["bitcode_size"]))
    if rep["header_extension"]:
        print("header extension:")
        for item in rep["header_extension"]:
            print("  %s" % item)
    print("functions: %d" % len(rep["functions"]))
    for fn in rep["functions"]:
        print("- %s [%s] bitcode off=%d size=%d magic=%s" % (
            fn["name"], fn["type_name"] or "unknown",
            fn["bitcode_off"], fn["bitcode_size"],
            fn["bitcode_magic"]))
        if fn["air_major"] is not None:
            print("    air %d.%d, language %d.%d" % (
                fn["air_major"], fn["air_minor"],
                fn["lang_major"], fn["lang_minor"]))
        if fn["hash"]:
            print("    sha256 %s" % fn["hash"])
            if fn["hash_match"] is True:
                print("    sha256 MATCH")
            elif fn["hash_match"] is False:
                print("    sha256 MISMATCH (bitcode hashes to %s)" %
                      fn["hash_actual"])
        if fn["wrapper"]:
            w = fn["wrapper"]
            print("    wrapper: version %d, stream off %d, size %d, "
                  "cpu type 0x%08x" % (
                      w["version"], w["offset"], w["size"],
                      w["cpu_type"]))
        if fn["public_metadata"]:
            print("    public metadata: %s" % fn["public_metadata"])
        if fn["private_metadata"]:
            print("    private metadata: %s" % fn["private_metadata"])
        if fn["air_intrinsics_heuristic"]:
            print("    possible AIR intrinsics (heuristic string scan):")
            for s in fn["air_intrinsics_heuristic"]:
                print("      %s" % s)


def main(argv):
    if len(argv) < 2 or argv[1] in ("-h", "--help"):
        print(__doc__)
        return 2
    want_json = "--json" in argv[1:]
    path = argv[1]
    try:
        rep = parse_metallib(path)
    except (ParseError, OSError) as e:
        print("metallib_scan: error: %s" % e, file=sys.stderr)
        return 1
    if want_json:
        print(json.dumps(rep, indent=2, default=str))
    else:
        print_report(rep)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
