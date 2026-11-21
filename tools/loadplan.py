#!/usr/bin/env python3
"""Offline load plan generator for ioswine.

Usage: tools/loadplan.py <app-dir-or-macho> [--cache-dir DIR] [--output FILE]

Computes and prints:
- Images in BFS load order (as the loader will map them)
- Install-name aliases taken
- @rpath resolutions to bundle frameworks
- Bind counts per kind for images with fixups

Dry-run mode matching loader option --loader-plan.
"""
import argparse
import os
import plistlib
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if REPO not in sys.path:
    sys.path.insert(0, REPO)

from tools.importcheck import (
    Cache,
    DEFAULT_CACHE_DIR,
    load_macho_file,
    uleb,
    sleb,
    cstr,
    LC_RPATH,
)

EXPLICIT_ALIASES = {
    "/usr/lib/swift/libswiftNetwork.dylib": "/System/Library/Frameworks/Network.framework/Network",
    "/usr/lib/swift/libswiftWebKit.dylib": "/System/Library/Frameworks/WebKit.framework/WebKit",
}


def find_app_binary(target):
    if os.path.isfile(target):
        return os.path.abspath(target), os.path.dirname(os.path.abspath(target))

    if os.path.isdir(target):
        target = os.path.abspath(target)
        plist_path = os.path.join(target, "Info.plist")
        if os.path.isfile(plist_path):
            with open(plist_path, "rb") as f:
                try:
                    plist = plistlib.load(f)
                    exe_name = plist.get("CFBundleExecutable")
                    if exe_name:
                        p = os.path.join(target, exe_name)
                        if os.path.isfile(p):
                            return p, target
                except Exception:
                    pass
        base = os.path.basename(target.rstrip("/"))
        if base.endswith(".app"):
            base = base[:-4]
        cand = os.path.join(target, base)
        if os.path.isfile(cand):
            return cand, target
        raise ValueError(f"Could not find executable binary in {target}")
    raise FileNotFoundError(target)


def count_dyldinfo_0x40(raw_data, off, size, is_lazy=False):
    if not size or off + size > len(raw_data):
        return 0
    b = raw_data[off : off + size]
    p = 0
    c = 0
    n = len(b)
    while p < n:
        op = b[p]
        opc = op & 0xF0
        p += 1
        if opc == 0x00:
            if not is_lazy:
                break
        elif opc == 0x40:
            c += 1
            e = b.find(b"\0", p)
            if e < 0:
                break
            p = e + 1
        elif opc in (0x20, 0x70, 0x80, 0xA0):
            _, p = uleb(b, p)
        elif opc == 0x60:
            _, p = sleb(b, p)
        elif opc == 0xC0:
            _, p = uleb(b, p)
            _, p = uleb(b, p)
        elif opc in (0x10, 0x30, 0x50, 0x90, 0xB0):
            pass
        elif opc == 0xD0:
            imm = op & 0x0F
            if imm == 0:
                _, p = uleb(b, p)
    return c


def compute_load_plan(target_path, cache_dir=DEFAULT_CACHE_DIR):
    exe_file, app_dir = find_app_binary(target_path)
    cache = Cache(cache_dir)
    exe_img = load_macho_file(exe_file)
    exe_basename = os.path.basename(exe_file)

    aliases_taken = []
    seen_aliases = set()

    def resolve_alias(name):
        if name in cache.images:
            return name
        target = EXPLICIT_ALIASES.get(name)
        if not target and name.startswith("/usr/lib/swift/libswift") and name.endswith(".dylib"):
            foo = name[len("/usr/lib/swift/libswift") : -len(".dylib")]
            cand = f"/System/Library/Frameworks/{foo}.framework/{foo}"
            if cand in cache.images:
                target = cand
        if target:
            if name not in seen_aliases:
                seen_aliases.add(name)
                aliases_taken.append((name, target))
            return target
        return None

    rpath_resolutions = []
    seen_rpaths = set()
    loaded_frameworks = {}  # name -> (file_path, Image)

    def resolve_rpath(name, requesting_img):
        loader_dir = os.path.dirname(requesting_img.path)
        cands = []
        if name.startswith("@rpath/"):
            tail = name[7:]
            rps = []
            for rp in requesting_img.rpaths:
                rps.append(rp.replace("@executable_path", app_dir).replace("@loader_path", loader_dir))
            if requesting_img is not exe_img:
                for rp in exe_img.rpaths:
                    rps.append(rp.replace("@executable_path", app_dir).replace("@loader_path", app_dir))
            for r in rps:
                cands.append(os.path.join(r, tail))
        elif name.startswith("@executable_path/"):
            cands.append(os.path.join(app_dir, name[17:]))
        elif name.startswith("@loader_path/"):
            cands.append(os.path.join(loader_dir, name[13:]))
        else:
            return None

        for c in cands:
            c = os.path.normpath(c)
            if os.path.isfile(c):
                rel = c
                if rel.startswith(app_dir + "/"):
                    rel = rel[len(app_dir) + 1 :]
                if name not in seen_rpaths:
                    seen_rpaths.add(name)
                    rpath_resolutions.append((name, rel))
                if name not in loaded_frameworks:
                    loaded_frameworks[name] = (c, load_macho_file(c))
                return loaded_frameworks[name][1]
        return None

    # BFS traversal
    images = [exe_basename]
    image_objs = [exe_img]
    via_upward = [False]
    seen_images = {exe_basename}

    i = 0
    while i < len(images):
        cur_img = image_objs[i]
        cur_up = via_upward[i]
        for kind, name, cmd in cur_img.dylibs:
            up = 1 if kind == "upward" else (2 if kind == "lazy" else 0)
            if up == 2 or (up and cur_up):
                continue

            target_img = None
            target_name = None

            if name.startswith(("@rpath/", "@executable_path/", "@loader_path/")):
                target_img = resolve_rpath(name, cur_img)
                if target_img:
                    target_name = name
            else:
                aliased = resolve_alias(name)
                if aliased and aliased in cache.images:
                    target_name = aliased
                    target_img = cache.image(aliased)

            if target_name and target_name not in seen_images:
                seen_images.add(target_name)
                images.append(target_name)
                image_objs.append(target_img)
                via_upward.append((up == 1) or cur_up)
        i += 1

    # Binds per kind for images with fixups
    binds_per_image = []
    total_binds = 0

    # Main image
    exe_binds = {}
    if exe_img.fixups:
        off, size = exe_img.fixups
        cf = exe_img.linkedit_read(off, size)
        ver, starts_off, imports_off, symbols_off, count, fmt, sfmt = struct.unpack_from("<7I", cf, 0)
        exe_binds["chained"] = count
        total_binds += count
    if exe_img.dyld_info:
        raw = exe_img._data(0, exe_img.segments[0][2] if exe_img.segments else 0) # file data
        with open(exe_file, "rb") as f:
            raw = f.read()
        b_cnt = count_dyldinfo_0x40(raw, exe_img.dyld_info[2], exe_img.dyld_info[3], False)
        l_cnt = count_dyldinfo_0x40(raw, exe_img.dyld_info[6], exe_img.dyld_info[7], True)
        w_cnt = count_dyldinfo_0x40(raw, exe_img.dyld_info[4], exe_img.dyld_info[5], False)
        if b_cnt:
            exe_binds["bind"] = b_cnt
            total_binds += b_cnt
        if l_cnt:
            exe_binds["lazy"] = l_cnt
            total_binds += l_cnt
        if w_cnt:
            exe_binds["weak"] = w_cnt
            total_binds += w_cnt
    if exe_binds:
        binds_per_image.append((exe_basename, exe_binds))

    # Frameworks
    for fw_name, (fw_path, fw_img) in loaded_frameworks.items():
        fw_base = os.path.basename(fw_path)
        fw_binds = {}
        with open(fw_path, "rb") as f:
            raw = f.read()
        if fw_img.fixups:
            off, size = fw_img.fixups
            cf = raw[off : off + size]
            ver, starts_off, imports_off, symbols_off, count, fmt, sfmt = struct.unpack_from("<7I", cf, 0)
            fw_binds["chained"] = count
            total_binds += count
        if fw_img.dyld_info:
            b_cnt = count_dyldinfo_0x40(raw, fw_img.dyld_info[2], fw_img.dyld_info[3], False)
            l_cnt = count_dyldinfo_0x40(raw, fw_img.dyld_info[6], fw_img.dyld_info[7], True)
            w_cnt = count_dyldinfo_0x40(raw, fw_img.dyld_info[4], fw_img.dyld_info[5], False)
            if b_cnt:
                fw_binds["bind"] = b_cnt
                total_binds += b_cnt
            if l_cnt:
                fw_binds["lazy"] = l_cnt
                total_binds += l_cnt
            if w_cnt:
                fw_binds["weak"] = w_cnt
                total_binds += w_cnt
        if fw_binds:
            binds_per_image.append((fw_base, fw_binds))

    # Format output text
    lines = []
    lines.append("=== Load Plan ===")
    lines.append(f"Images ({len(images)}):")
    for idx, img_name in enumerate(images):
        lines.append(f"  [{idx:4d}] {img_name}")
    lines.append("")
    lines.append(f"Aliases taken ({len(aliases_taken)}):")
    for src, dst in aliases_taken:
        lines.append(f"  {src} -> {dst}")
    lines.append("")
    lines.append(f"@rpath resolution ({len(rpath_resolutions)}):")
    for src, dst in rpath_resolutions:
        lines.append(f"  {src} -> {dst}")
    lines.append("")
    lines.append("Binds per kind:")
    for name, bmap in binds_per_image:
        lines.append(f"  {name}:")
        for kind, cnt in bmap.items():
            lines.append(f"    {kind}: {cnt}")
    lines.append(f"  Total binds: {total_binds}")
    lines.append("")

    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description="Generate offline load plan")
    parser.add_argument("app", help="Path to .app bundle or main Mach-O executable")
    parser.add_argument("--cache-dir", default=DEFAULT_CACHE_DIR, help="Path to shared cache directory")
    parser.add_argument("--output", "-o", help="Output file (defaults to stdout)")
    args = parser.parse_args()

    plan = compute_load_plan(args.app, args.cache_dir)
    if args.output:
        with open(args.output, "w") as f:
            f.write(plan)
    else:
        sys.stdout.write(plan)


if __name__ == "__main__":
    main()
