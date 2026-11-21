#!/bin/sh
# run_tests.sh - build and run every air2spirv host test with one command.
#
#   tests/host/run_tests.sh [fuzz_iterations]
#
# Does, in order:
#   1. NDK freestanding compile check for src/*.c
#      (-ffreestanding -Wall -Wextra -Werror, aarch64 clang).
#   2. Host builds of the three tests (plain cc, ASan+UBSan).
#   3. metallib_test (round-trip, error paths, fuzz) + dumps a
#      synthetic .metallib for the scanner.
#   4. bitcode_test (event sequence, error paths, fuzz).
#   5. air2spirv_test (emits vertex.spv / fragment.spv /
#      fragment_color.spv, validates with spirv-val when present).
#   6. tools/metallib_scan.py on the synthetic .metallib.
#   7. spirv-val + spirv-dis on the emitted modules.
#   8. The 2015 reconstruction: rebuild_2015.py rebuilds the
#      HelloTriangle .metallib from the hex dump printed in
#      zhuowei's writeup, ml_2015_check.c asserts the container
#      facts against the C reader, and metallib_scan.py parses it
#      (HASH mismatches are expected: the article does not print
#      the bitcode bodies, so they are zero-filled).
#
# Fuzz iterations default to 20000 (override with the first argument).
# Build artifacts go to a temp dir; nothing is left in the tree.
# Exits non-zero on the first failure.
set -e

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/src"
TESTS="$ROOT/tests/host"
TOOLS="$ROOT/tools"
NDK_CLANG="$HOME/workspace/tools/android-ndk-r28c/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android34-clang"
FUZZ_ITERS="${1:-20000}"
BUILD="$(mktemp -d /tmp/a2s-tests.XXXXXX)"
SPV_DIR="$BUILD/spv"
METALLIB="$BUILD/synth.metallib"

echo "build dir: $BUILD"

echo "== NDK freestanding check =="
for f in metallib bitcode air2spirv; do
    "$NDK_CLANG" -std=c99 -ffreestanding -Wall -Wextra -Werror -O2 \
        -I"$SRC" -c "$SRC/$f.c" -o "$BUILD/$f.o"
    echo "  OK $f.c"
done

echo "== host builds (ASan+UBSan) =="
CC="${CC:-cc}"
for t in metallib bitcode air2spirv; do
    "$CC" -std=c99 -Wall -Wextra -O1 -g -fsanitize=address,undefined \
        -fno-sanitize-recover=all -I"$SRC" \
        -o "$BUILD/${t}_test" "$TESTS/${t}_test.c" "$SRC/$t.c"
    echo "  OK ${t}_test"
done

echo "== metallib_test (fuzz $FUZZ_ITERS) =="
"$BUILD/metallib_test" "$FUZZ_ITERS" "$METALLIB"

echo "== bitcode_test (fuzz $FUZZ_ITERS) =="
"$BUILD/bitcode_test" "$FUZZ_ITERS"

echo "== air2spirv_test =="
mkdir -p "$SPV_DIR"
"$BUILD/air2spirv_test" "$SPV_DIR"

echo "== metallib_scan.py on the synthetic file =="
python3 "$TOOLS/metallib_scan.py" "$METALLIB" | tee "$BUILD/scan.txt"
grep -q "vertexMain" "$BUILD/scan.txt"
grep -q "fragmentMain" "$BUILD/scan.txt"
python3 "$TOOLS/metallib_scan.py" "$METALLIB" --json > "$BUILD/scan.json"
grep -q "vertexMain" "$BUILD/scan.json"
echo "  scan OK (text + json)"

echo "== 2015 reconstruction (worthdoingbadly hex dump) =="
python3 "$TESTS/rebuild_2015.py" "$BUILD/hello2015.metallib"
"$CC" -std=c99 -Wall -Wextra -O1 -g -fsanitize=address,undefined \
    -fno-sanitize-recover=all -I"$SRC" \
    -o "$BUILD/ml_2015_check" "$TESTS/ml_2015_check.c" "$SRC/metallib.c"
"$BUILD/ml_2015_check" "$BUILD/hello2015.metallib"
python3 "$TOOLS/metallib_scan.py" "$BUILD/hello2015.metallib" | \
    tee "$BUILD/scan2015.txt"
grep -q "vertexShader" "$BUILD/scan2015.txt"
grep -q "fragmentShader" "$BUILD/scan2015.txt"
# The reconstruction zero-fills the bitcode bodies past 0x19f (the
# article does not print them), so both HASH tags must mismatch.
grep -q "MISMATCH" "$BUILD/scan2015.txt"
echo "  2015 reconstruction OK (container parses; HASH mismatches expected)"

if command -v spirv-val >/dev/null 2>&1; then
    echo "== spirv-val =="
    for f in vertex fragment fragment_color; do
        spirv-val "$SPV_DIR/$f.spv"
        echo "  OK $f.spv"
    done
else
    echo "spirv-val not found, skipping external validation"
    exit 1
fi

if command -v spirv-dis >/dev/null 2>&1; then
    echo "== spirv-dis (vertex, first 12 lines) =="
    spirv-dis "$SPV_DIR/vertex.spv" | head -12
fi

echo "ALL GREEN"
echo "artifacts: $BUILD"
