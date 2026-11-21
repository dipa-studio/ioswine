#!/bin/sh
# build-integration.sh: build the turnip vkshim integration
# and run the host test.
#
#   1. turnip.c + vkshim.c freestanding, NDK r28c aarch64 clang,
#      -ffreestanding -Wall -Wextra -Werror (the device-side code).
#   2. gpu-helper + host test with host gcc, plain and ASan/UBSan.
#   3. Run the host test against the helper (host mode, Mesa
#      llvmpipe via the system libvulkan).
#
# Env overrides: IOSWINE_SRC (default ~/workspace/ioswine/src-1112b),
# NDK (default ~/workspace/tools/android-ndk-r28c).
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SRC=${IOSWINE_SRC:-$HOME/workspace/ioswine/src-1112b}
NDK=${NDK:-$HOME/workspace/tools/android-ndk-r28c}
BUILD=$DIR/build-host
mkdir -p "$BUILD"

NDK_CLANG=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang
if [ ! -x "$NDK_CLANG" ]; then
    echo "build.sh: NDK clang not found at $NDK_CLANG" >&2
    exit 1
fi
if [ ! -f "$SRC/libsystem_kernel/src/vkshim.c" ]; then
    echo "build.sh: src tree not found at $SRC" >&2
    exit 1
fi

echo "== freestanding: turnip.c + vkshim.c (NDK r28c aarch64) =="
"$NDK_CLANG" --target=aarch64-linux-android34 -ffreestanding \
    -Wall -Wextra -Werror \
    -I"$SRC/libsystem_kernel/src" -I"$DIR/src" \
    -c "$DIR/src/turnip.c" -o "$BUILD/turnip-arm64.o"
"$NDK_CLANG" --target=aarch64-linux-android34 -ffreestanding \
    -Wall -Wextra -Werror \
    -I"$SRC/libsystem_kernel/src" \
    -c "$SRC/libsystem_kernel/src/vkshim.c" -o "$BUILD/vkshim-arm64.o"
echo "freestanding build OK"

echo "== host: gpu-helper (gcc) =="
gcc -Wall -Wextra -Werror -I"$DIR/src" \
    "$DIR/src/gpu-helper.c" -o "$BUILD/gpu-helper" -ldl
gcc -Wall -Wextra -Werror -fsanitize=address,undefined -g -I"$DIR/src" \
    "$DIR/src/gpu-helper.c" -o "$BUILD/gpu-helper-asan" -ldl
echo "gpu-helper OK"

echo "== host: turnip_test (gcc, plain + ASan/UBSan) =="
# NOTE: turnip.c is compiled WITHOUT the src tree on -I. The src
# tree ships its own libsystem_kernel/src/errno.h (the lsk's
# freestanding errno mapping), and with that directory on -I a
# host #include <errno.h> resolves to it instead of the system
# header, so `errno`/`EINTR` go undeclared. turnip.c needs no src
# header in host mode, so it is built with -I"$DIR/src" only.
# turnip_test.c and vkshim.c do need vkshim.h from the src tree;
# their system headers do not pull in <errno.h>, so the src -I is
# safe for them.
gcc -Wall -Wextra -Werror -DTURNIP_HOST_TEST -I"$DIR/src" \
    -c "$DIR/src/turnip.c" -o "$BUILD/turnip-host.o"
gcc -Wall -Wextra -Werror -DTURNIP_HOST_TEST \
    -I"$DIR/src" -I"$SRC/libsystem_kernel/src" \
    -c "$DIR/tests/host/turnip_test.c" -o "$BUILD/turnip_test.o"
gcc -Wall -Wextra -Werror -I"$SRC/libsystem_kernel/src" \
    -c "$SRC/libsystem_kernel/src/vkshim.c" -o "$BUILD/vkshim-host.o"
gcc -Wall -Wextra -Werror \
    "$BUILD/turnip_test.o" "$BUILD/turnip-host.o" \
    "$BUILD/vkshim-host.o" -o "$BUILD/turnip_test"
gcc -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    -DTURNIP_HOST_TEST -I"$DIR/src" \
    -c "$DIR/src/turnip.c" -o "$BUILD/turnip-host-asan.o"
gcc -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    -DTURNIP_HOST_TEST \
    -I"$DIR/src" -I"$SRC/libsystem_kernel/src" \
    -c "$DIR/tests/host/turnip_test.c" -o "$BUILD/turnip_test-asan.o"
gcc -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    -I"$SRC/libsystem_kernel/src" \
    -c "$SRC/libsystem_kernel/src/vkshim.c" -o "$BUILD/vkshim-host-asan.o"
gcc -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    "$BUILD/turnip_test-asan.o" "$BUILD/turnip-host-asan.o" \
    "$BUILD/vkshim-host-asan.o" -o "$BUILD/turnip_test_asan"
echo "turnip_test OK"

echo "== run: host test (plain) =="
"$BUILD/turnip_test" "$BUILD/gpu-helper"

echo "== run: host test (ASan/UBSan) =="
"$BUILD/turnip_test_asan" "$BUILD/gpu-helper-asan"

echo "build-integration.sh: ALL PASS"
