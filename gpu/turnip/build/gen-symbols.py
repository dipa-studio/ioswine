#!/usr/bin/env python3
"""Generate symbols.txt for libvulkan_freedreno.so.

Reads the dynamic undefined symbols of the built Turnip .so and attributes
each one to the DT_NEEDED library that provides it, using the NDK sysroot
stub .so files (API 34) for libc/libm/libdl/liblog/libz and Mesa's own
android_stub .so files for libhardware/libnativewindow/libsync.

Usage: python3 gen-symbols.py <path-to-libvulkan_freedreno.so>
Writes symbols.txt in the current directory.
"""
import collections
import os
import re
import subprocess
import sys

NDK = os.environ.get("ANDROID_NDK_HOME", "/opt/android-ndk")
SYSROOT = os.path.join(
    NDK, "toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/34"
)
LLVM_NM = os.path.join(
    NDK, "toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-nm"
)


def nm_defined(path):
    """Return set of defined dynamic symbol names in a .so."""
    out = subprocess.run(
        [LLVM_NM, "-D", "--defined-only", path],
        capture_output=True, text=True, check=True,
    ).stdout
    names = set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3:
            # Defined names carry a version suffix too (abort@@LIBC);
            # strip it so it matches the undefined base name.
            names.add(parts[2].split("@")[0])
    return names


def nm_undefined(path):
    """Return list of (kind, name) for undefined dynamic symbols.

    kind is 'U' (strong undefined) or 'w' (weak undefined).
    """
    out = subprocess.run(
        [LLVM_NM, "-D", "--undefined-only", path],
        capture_output=True, text=True, check=True,
    ).stdout
    syms = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 2:
            kind, name = parts[0], parts[1]
            if kind in ("U", "w"):
                syms.append((kind, name))
    return syms


def base_name(name):
    """Strip a symbol-version suffix ('@LIBC', '@@LIBC_R') for matching."""
    return name.split("@")[0]


# Symbols the provider stubs do not list, attributed by known API ownership.
# Mesa's android_stub .so files are minimal (they only declare the symbols
# Mesa's own code references through the stub headers), so the real device
# libraries provide more. AHardwareBuffer_* live in libnativewindow.so,
# sync_* in libsync.so, hw_get_module/hw_device_t in libhardware.so.
KNOWN_API = [
    ("AHardwareBuffer_", "libnativewindow.so"),
    ("ANativeWindow_", "libnativewindow.so"),
    ("sync_", "libsync.so"),
    ("hw_get_module", "libhardware.so"),
    ("hw_device", "libhardware.so"),
]


def readelf_dynamic(path):
    out = subprocess.run(
        [os.path.join(NDK, "toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf"),
         "-d", path],
        capture_output=True, text=True, check=True,
    ).stdout
    needed = re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]", out)
    soname = re.findall(r"\(SONAME\)\s+Library soname: \[([^\]]+)\]", out)
    return needed, soname


def main():
    if len(sys.argv) < 2:
        print("usage: python3 gen-symbols.py <path-to-libvulkan_freedreno.so>",
              file=sys.stderr)
        sys.exit(2)
    so = sys.argv[1]
    if not os.path.isfile(so):
        print("gen-symbols.py: no such file: %s" % so, file=sys.stderr)
        sys.exit(2)
    if not os.path.isfile(LLVM_NM):
        print("gen-symbols.py: llvm-nm not found at %s" % LLVM_NM,
              file=sys.stderr)
        sys.exit(2)
    build_dir = os.path.dirname(os.path.abspath(so))
    stub_dir = os.path.join(build_dir, "src/android_stub")

    try:
        needed, soname = readelf_dynamic(so)
    except subprocess.CalledProcessError:
        print("gen-symbols.py: readelf failed on %s (not a valid ELF .so?)"
              % so, file=sys.stderr)
        sys.exit(1)
    if not needed:
        print("gen-symbols.py: no DT_NEEDED found in %s (not a valid .so?)"
              % so, file=sys.stderr)
        sys.exit(1)

    providers = {}
    for lib in needed:
        if lib in ("libc.so", "libm.so", "libdl.so", "liblog.so", "libz.so"):
            p = os.path.join(SYSROOT, lib)
        elif lib in ("libhardware.so", "libnativewindow.so", "libsync.so"):
            p = os.path.join(stub_dir, lib)
        else:
            p = None
        if p is not None and os.path.exists(p):
            providers[lib] = nm_defined(p)
        else:
            providers[lib] = set()

    undef = nm_undefined(so)
    if not undef:
        print("gen-symbols.py: llvm-nm found zero undefined symbols in %s"
              % so, file=sys.stderr)
        print("gen-symbols.py: refusing to write symbols.txt", file=sys.stderr)
        sys.exit(1)
    by_lib = collections.defaultdict(list)
    weak_by_lib = collections.defaultdict(list)
    unmatched = []
    unmatched_weak = []
    for kind, name in undef:
        base = base_name(name)
        hit = None
        for lib in needed:
            if base in providers.get(lib, ()):
                hit = lib
                break
        if hit is None:
            for prefix, lib in KNOWN_API:
                if base.startswith(prefix) or base == prefix:
                    hit = lib
                    break
        if hit is None:
            if kind == "w":
                unmatched_weak.append(name)
            else:
                unmatched.append(name)
        elif kind == "w":
            weak_by_lib[hit].append(name)
        else:
            by_lib[hit].append(name)

    defined = subprocess.run(
        [LLVM_NM, "-D", "--defined-only", so],
        capture_output=True, text=True, check=True,
    ).stdout.splitlines()

    # Write to a temp file and rename, so a failure mid-write never
    # leaves a truncated symbols.txt behind.
    tmp_path = "symbols.txt.tmp"
    with open(tmp_path, "w") as f:
        f.write("Undefined-symbol inventory for libvulkan_freedreno.so\n")
        f.write("Mesa 26.2.7 Turnip (freedreno, KGSL-only), NDK r28c, API 34\n")
        f.write("Generated by gen-symbols.py (llvm-nm from the NDK).\n\n")
        f.write("DT_NEEDED (in link order):\n")
        for lib in needed:
            f.write("  %s\n" % lib)
        f.write("SONAME: %s\n\n" % (soname[0] if soname else "(none)"))
        f.write("Defined dynamic exports (%d):\n" % len(defined))
        for line in defined:
            f.write("  %s\n" % line.strip())
        f.write("\n")
        total_strong = sum(len(v) for v in by_lib.values()) + len(unmatched)
        total_weak = sum(len(v) for v in weak_by_lib.values()) + len(unmatched_weak)
        f.write("Undefined dynamic symbols: %d total (%d strong 'U', %d weak 'w')\n"
                % (len(undef), total_strong, total_weak))
        f.write("Attribution: a symbol is counted under the first DT_NEEDED\n"
                "library (in DT_NEEDED order) whose export list contains it.\n"
                "Provider export lists come from the NDK sysroot API-34 stubs\n"
                "(libc/libm/libdl/liblog/libz) and Mesa's android_stub .so\n"
                "files (libhardware/libnativewindow/libsync).\n\n")
        f.write("Strong undefined symbols by providing library:\n")
        for lib in needed:
            n = len(by_lib.get(lib, []))
            f.write("  %-20s %d\n" % (lib, n))
        f.write("  %-20s %d  (strong, matched by no DT_NEEDED lib)\n"
                % ("UNMATCHED", len(unmatched)))
        f.write("\nWeak undefined symbols by providing library:\n")
        for lib in needed:
            n = len(weak_by_lib.get(lib, []))
            f.write("  %-20s %d\n" % (lib, n))
        f.write("  %-20s %d  (weak, matched by no DT_NEEDED lib)\n"
                % ("UNMATCHED", len(unmatched_weak)))
        f.write("\n")
        for lib in needed:
            names = sorted(by_lib.get(lib, []))
            if names:
                f.write("=== %s: %d strong undefined ===\n" % (lib, len(names)))
                for n in names:
                    f.write("  %s\n" % n)
                f.write("\n")
            wnames = sorted(weak_by_lib.get(lib, []))
            if wnames:
                f.write("=== %s: %d weak undefined ===\n" % (lib, len(wnames)))
                for n in wnames:
                    f.write("  %s\n" % n)
                f.write("\n")
        if unmatched:
            f.write("=== UNMATCHED: %d strong undefined (no DT_NEEDED lib exports them) ===\n"
                    % len(unmatched))
            for n in sorted(unmatched):
                f.write("  %s\n" % n)
            f.write("\n")
        if unmatched_weak:
            f.write("=== UNMATCHED: %d weak undefined (no DT_NEEDED lib exports them) ===\n"
                    % len(unmatched_weak))
            for n in sorted(unmatched_weak):
                f.write("  %s\n" % n)
            f.write("\n")
    os.replace(tmp_path, "symbols.txt")

    print("strong by lib:", {lib: len(by_lib.get(lib, [])) for lib in needed},
          "UNMATCHED strong:", len(unmatched))
    print("weak by lib:", {lib: len(weak_by_lib.get(lib, [])) for lib in needed},
          "UNMATCHED weak:", len(unmatched_weak))
    print("wrote symbols.txt")


if __name__ == "__main__":
    main()
