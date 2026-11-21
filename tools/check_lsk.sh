#!/usr/bin/env bash
set -euo pipefail

fail() {
    echo "LSK CHECK FAILED: $*" >&2
    exit 1
}

DYLIB="build/libsystem_kernel.dylib"
TSV="gen/data/libsystem_kernel.exports.tsv"

[ -f "$DYLIB" ] || fail "$DYLIB not found"
[ -f "$TSV" ] || fail "$TSV not found"

echo "=== Checking $DYLIB private headers ==="
HEADERS=$(llvm-objdump --macho --private-headers "$DYLIB")

# Verify filetype DYLIB
echo "$HEADERS" | grep -q "DYLIB" || fail "$DYLIB is not filetype DYLIB"

# Verify cpusubtype arm64e (displayed as 'E' with PAC00 in llvm-objdump, or CPU_SUBTYPE_ARM64E in llvm-readobj)
if ! echo "$HEADERS" | grep -E -q "ARM64[[:space:]]+E" && ! llvm-readobj -h "$DYLIB" | grep -q "CPU_SUBTYPE_ARM64E"; then
    fail "$DYLIB cpusubtype is not arm64e"
fi

# Verify LC_ID_DYLIB install_name
echo "$HEADERS" | grep -q "cmd LC_ID_DYLIB" || fail "$DYLIB missing LC_ID_DYLIB"
echo "$HEADERS" | grep -q "/usr/lib/system/libsystem_kernel.dylib" || fail "$DYLIB wrong install_name"

# Verify no LC_LOAD_DYLIB
if echo "$HEADERS" | grep -q "cmd LC_LOAD_DYLIB"; then
    fail "$DYLIB contains LC_LOAD_DYLIB"
fi

echo "=== Verifying all TSV exports are present in $DYLIB ==="
python3 - "$TSV" "$DYLIB" << 'EOF'
import subprocess, sys

tsv_path = sys.argv[1]
dylib_path = sys.argv[2]

tsv_names = set()
with open(tsv_path, "r", encoding="utf-8") as f:
    for line in f:
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if parts[0] == "name":
            continue
        tsv_names.add(parts[0])

out = subprocess.check_output(["llvm-nm", "-gU", dylib_path]).decode("utf-8")
dylib_symbols = set()
for line in out.splitlines():
    line = line.strip()
    if not line:
        continue
    parts = line.split()
    if len(parts) >= 2:
        dylib_symbols.add(parts[-1])

missing = tsv_names - dylib_symbols
print(f"TSV exports count: {len(tsv_names)}")
print(f"Dylib exported symbols count: {len(dylib_symbols)}")
if missing:
    sample = sorted(list(missing))[:10]
    sys.stderr.write(f"ERROR: {len(missing)} TSV symbols missing from dylib exports: {sample}\n")
    sys.exit(1)
print(f"All {len(tsv_names)} TSV export names verified in dylib.")
EOF

echo "=== Verifying _write is not a stub ==="
WRITE_DISASM=$(llvm-objdump -d --disassemble-symbols=_write "$DYLIB")
[ -n "$WRITE_DISASM" ] || fail "_write symbol disassembly not found in $DYLIB"
if echo "$WRITE_DISASM" | grep -q "___iosw_unimplemented"; then
    fail "_write branches to ___iosw_unimplemented (it is a stub)"
fi

echo "LSK OK"
