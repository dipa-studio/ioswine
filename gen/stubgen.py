#!/usr/bin/env python3
"""
gen/stubgen.py: Reads libsystem_kernel exports TSV and defined symbols from
implementation objects, and generates arm64e assembly stubs for unimplemented symbols.
Python 3, standard library only.
"""

import sys
import os
import subprocess
import argparse


def quote_sym(name: str) -> str:
    """Quotes symbol names containing '$' or characters that require quoting in assembler."""
    if "$" in name or not name.isidentifier():
        return f'"{name}"'
    return name


def get_defined_symbols(objs, nm_bin="llvm-nm"):
    """Runs nm --defined-only on object files and returns set of defined symbol names."""
    defined = set()
    valid_objs = [f for f in objs if os.path.isfile(f)]
    if not valid_objs:
        return defined

    cmd = [nm_bin, "--defined-only"] + valid_objs
    try:
        out = subprocess.check_output(cmd, stderr=subprocess.DEVNULL).decode("utf-8")
    except (subprocess.CalledProcessError, FileNotFoundError):
        # Fallback to plain 'nm' if nm_bin fails
        try:
            cmd = ["nm", "--defined-only"] + valid_objs
            out = subprocess.check_output(cmd, stderr=subprocess.DEVNULL).decode("utf-8")
        except Exception as e:
            sys.stderr.write(f"Warning: failed to run nm on {valid_objs}: {e}\n")
            return defined

    for line in out.splitlines():
        line = line.strip()
        if not line or line.endswith(":"):
            continue
        parts = line.split()
        if len(parts) >= 2:
            sym_name = parts[-1]
            defined.add(sym_name)

    return defined


def main():
    parser = argparse.ArgumentParser(description="Generate libsystem_kernel stubs")
    parser.add_argument("--tsv", default="gen/data/libsystem_kernel.exports.tsv",
                        help="Path to exports TSV file")
    parser.add_argument("--out", default="build/lsk/gen/stubs.S",
                        help="Path to output stubs.S file")
    parser.add_argument("--nm", default=os.environ.get("LLVM_NM", os.environ.get("NM", "llvm-nm")),
                        help="Path to nm binary")
    parser.add_argument("objs", nargs="*", help="Implementation object files (.o)")

    args, unknown = parser.parse_known_args()

    # Handle positional invocation: stubgen.py <tsv> <out> [objs...]
    positional = unknown + args.objs
    tsv_path = args.tsv
    out_path = args.out
    objs = []

    if positional:
        # Check if positional arguments were used without flags
        if len(positional) >= 2 and positional[0].endswith(".tsv") and (positional[1].endswith(".S") or positional[1].endswith(".s")):
            tsv_path = positional[0]
            out_path = positional[1]
            objs = positional[2:]
        else:
            objs = positional

    defined_symbols = get_defined_symbols(objs, nm_bin=args.nm)

    if not os.path.isfile(tsv_path):
        sys.stderr.write(f"Error: TSV file not found: {tsv_path}\n")
        sys.exit(1)

    code_stubs = []
    cstrings = []
    data_stubs = []
    stub_idx = 0

    with open(tsv_path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if parts[0] == "name":
                continue

            name = parts[0]
            kind = parts[1] if len(parts) > 1 else "code"

            # Skip symbols implemented in our objects
            if name in defined_symbols:
                continue
            # Skip internal implementation symbols if they appear
            if name == "___iosw_unimplemented":
                continue

            qname = quote_sym(name)

            if kind == "code":
                label = f"Lname_{stub_idx}"
                code_stubs.append(
                    f".globl {qname}\n"
                    f".p2align 2\n"
                    f"{qname}:\n"
                    f"    adrp x1, {label}@PAGE\n"
                    f"    add  x1, x1, {label}@PAGEOFF\n"
                    f"    b    ___iosw_unimplemented\n"
                )
                cstrings.append(
                    f"{label}:\n"
                    f"    .asciz \"{name}\"\n"
                )
                stub_idx += 1
            else:
                data_stubs.append(
                    f".globl {qname}\n"
                    f".p2align 4\n"
                    f"{qname}:\n"
                    f"    .zero 64\n"
                )

    out_dir = os.path.dirname(out_path)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)

    with open(out_path, "w", encoding="utf-8") as f:
        f.write("/* Auto-generated stubs for libsystem_kernel.dylib */\n\n")
        f.write(".text\n")
        f.write("\n".join(code_stubs))
        f.write("\n.section __TEXT,__cstring,cstring_literals\n")
        f.write("\n".join(cstrings))
        f.write("\n.section __DATA,__data\n")
        f.write("\n".join(data_stubs))

    print(f"stubgen: generated {len(code_stubs)} code stubs and {len(data_stubs)} data objects in {out_path}")


if __name__ == "__main__":
    main()
