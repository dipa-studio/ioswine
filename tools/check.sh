#!/usr/bin/env bash
set -euo pipefail

fail() {
    echo "CHECK FAILED: $*" >&2
    exit 1
}

echo "=== Checking build/rt-selftest ==="
[ -f build/rt-selftest ] || fail "build/rt-selftest not found"

ELF_HDR=$(llvm-readelf -h build/rt-selftest)
PROG_HDR=$(llvm-readelf -l build/rt-selftest)

echo "$ELF_HDR" | grep -q -E "Type:[[:space:]]*EXEC" || fail "rt-selftest is not Type EXEC"
echo "$ELF_HDR" | grep -q -E "Machine:[[:space:]]*AArch64" || fail "rt-selftest is not Machine AArch64"
if echo "$PROG_HDR" | grep -E -q "INTERP|DYNAMIC"; then
    fail "rt-selftest contains INTERP or DYNAMIC program header"
fi

if [ -f build/ioswine-loader ]; then
    echo "=== Checking build/ioswine-loader ==="
    L_ELF_HDR=$(llvm-readelf -h build/ioswine-loader)
    L_PROG_HDR=$(llvm-readelf -l build/ioswine-loader)

    echo "$L_ELF_HDR" | grep -q -E "Type:[[:space:]]*EXEC" || fail "ioswine-loader is not Type EXEC"
    echo "$L_ELF_HDR" | grep -q -E "Machine:[[:space:]]*AArch64" || fail "ioswine-loader is not Machine AArch64"
    if echo "$L_PROG_HDR" | grep -E -q "INTERP|DYNAMIC"; then
        fail "ioswine-loader contains INTERP or DYNAMIC program header"
    fi

    ENTRY=$(echo "$L_ELF_HDR" | awk '/Entry point address:/ { print $4 }')
    [ -n "$ENTRY" ] || fail "Could not determine entry point address of ioswine-loader"
    if ! (( ENTRY >= 0x7000000000 )); then
        fail "ioswine-loader entry point $ENTRY is < 0x7000000000"
    fi
fi

echo "=== Checking Mach-O tests private headers ==="
for t in build/tests/hello build/tests/fixups; do
    [ -f "$t" ] || fail "$t not found"
    HEADERS=$(llvm-objdump --macho --private-headers "$t")
    echo "$HEADERS" | grep -q "cmd LC_MAIN" || fail "$t missing LC_MAIN"
    echo "$HEADERS" | grep -q "cmd LC_DYLD_CHAINED_FIXUPS" || fail "$t missing LC_DYLD_CHAINED_FIXUPS"
    if echo "$HEADERS" | grep -q "cmd LC_LOAD_DYLIB"; then
        fail "$t contains LC_LOAD_DYLIB"
    fi
done

echo "=== Checking chained-fixups pointer_format = 2 ==="
FIXUPS_DUMP=$(llvm-objdump --macho --chained-fixups build/tests/fixups)
echo "$FIXUPS_DUMP" | grep -q "pointer_format = 2" || fail "build/tests/fixups does not show pointer_format = 2"

echo "=== Checking bind table (no binds) ==="
for t in build/tests/*; do
    [ -f "$t" ] || continue
    case "$t" in
        *.o) continue ;;
    esac
    BIND_DUMP=$(llvm-objdump --macho --bind "$t" 2>/dev/null || true)
    ENTRIES=$(echo "$BIND_DUMP" | grep -A 10000 "Bind table:" | grep -v "Bind table:" | grep -v "segment  section" | grep -v '^[[:space:]]*$' || true)
    if [ -n "$ENTRIES" ]; then
        fail "$t contains binds: $ENTRIES"
    fi
done

echo "=== Checking build/tests/hello contains svc #0x80 ==="
DISASM=$(llvm-objdump -d build/tests/hello)
echo "$DISASM" | grep -q -E 'svc[[:space:]]+#0x80' || fail "build/tests/hello does not contain svc #0x80"

echo "=== Checking libsys test programs ==="
for t in build/tests/libsys/write_hello build/tests/libsys/printf_hello build/tests/libsys/malloc_test build/tests/libsys/pthread_test build/tests/libsys/mach_test; do
    [ -f "$t" ] || fail "$t not found"
    HEADERS=$(llvm-objdump --macho --private-headers "$t")
    echo "$HEADERS" | grep -q "cmd LC_MAIN" || fail "$t missing LC_MAIN"
    echo "$HEADERS" | grep -q "cmd LC_DYLD_CHAINED_FIXUPS" || fail "$t missing LC_DYLD_CHAINED_FIXUPS"

    # Must have exactly one LC_LOAD_DYLIB, and it must be /usr/lib/libSystem.B.dylib
    DYLIB_COUNT=$(echo "$HEADERS" | grep -c "cmd LC_LOAD_DYLIB" || true)
    if [ "$DYLIB_COUNT" -ne 1 ]; then
        fail "$t has $DYLIB_COUNT LC_LOAD_DYLIB commands (expected 1)"
    fi
    echo "$HEADERS" | grep -A 2 "cmd LC_LOAD_DYLIB" | grep -q "/usr/lib/libSystem.B.dylib" || fail "$t load dylib is not /usr/lib/libSystem.B.dylib"

    # Must have LC_DYLD_CHAINED_FIXUPS with imports
    FIXUPS=$(llvm-objdump --macho --chained-fixups "$t")
    echo "$FIXUPS" | grep -q "dyld chained import" || fail "$t missing chained imports"
done

echo "=== Checking cg_hello test program ==="
[ -f build/tests/libsys/cg_hello ] || fail "build/tests/libsys/cg_hello not found"
CG_HEADERS=$(llvm-objdump --macho --private-headers build/tests/libsys/cg_hello)
echo "$CG_HEADERS" | grep -q "cmd LC_MAIN" || fail "cg_hello missing LC_MAIN"
echo "$CG_HEADERS" | grep -q "cmd LC_DYLD_CHAINED_FIXUPS" || fail "cg_hello missing LC_DYLD_CHAINED_FIXUPS"

CG_DYLIB_COUNT=$(echo "$CG_HEADERS" | grep -c "cmd LC_LOAD_DYLIB" || true)
if [ "$CG_DYLIB_COUNT" -ne 4 ]; then
    fail "cg_hello has $CG_DYLIB_COUNT LC_LOAD_DYLIB commands (expected 4)"
fi
echo "$CG_HEADERS" | grep -A 2 "cmd LC_LOAD_DYLIB" | grep -q "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics" || fail "cg_hello missing CoreGraphics"
echo "$CG_HEADERS" | grep -A 2 "cmd LC_LOAD_DYLIB" | grep -q "/System/Library/Frameworks/ImageIO.framework/ImageIO" || fail "cg_hello missing ImageIO"
echo "$CG_HEADERS" | grep -A 2 "cmd LC_LOAD_DYLIB" | grep -q "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation" || fail "cg_hello missing CoreFoundation"
echo "$CG_HEADERS" | grep -A 2 "cmd LC_LOAD_DYLIB" | grep -q "/usr/lib/libSystem.B.dylib" || fail "cg_hello missing libSystem.B"

CG_FIXUPS=$(llvm-objdump --macho --chained-fixups build/tests/libsys/cg_hello)
echo "$CG_FIXUPS" | grep -q "dyld chained import" || fail "cg_hello missing chained imports"

echo "=== Checking dispatch_test test program ==="
[ -f build/tests/libsys/dispatch_test ] || fail "build/tests/libsys/dispatch_test not found"
D_HEADERS=$(llvm-objdump --macho --private-headers build/tests/libsys/dispatch_test)
echo "$D_HEADERS" | grep -q "cmd LC_MAIN" || fail "dispatch_test missing LC_MAIN"
echo "$D_HEADERS" | grep -q "cmd LC_DYLD_CHAINED_FIXUPS" || fail "dispatch_test missing LC_DYLD_CHAINED_FIXUPS"

D_DYLIB_COUNT=$(echo "$D_HEADERS" | grep -c "cmd LC_LOAD_DYLIB" || true)
if [ "$D_DYLIB_COUNT" -ne 2 ]; then
    fail "dispatch_test has $D_DYLIB_COUNT LC_LOAD_DYLIB commands (expected 2)"
fi
echo "$D_HEADERS" | grep -A 2 "cmd LC_LOAD_DYLIB" | grep -q "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation" || fail "dispatch_test missing CoreFoundation"
echo "$D_HEADERS" | grep -A 2 "cmd LC_LOAD_DYLIB" | grep -q "/usr/lib/libSystem.B.dylib" || fail "dispatch_test missing libSystem.B"

D_FIXUPS=$(llvm-objdump --macho --chained-fixups build/tests/libsys/dispatch_test)
echo "$D_FIXUPS" | grep -q "dyld chained import" || fail "dispatch_test missing chained imports"

echo "=== Checking libsystem_kernel.dylib ==="
tools/check_lsk.sh

echo "CHECK OK"
