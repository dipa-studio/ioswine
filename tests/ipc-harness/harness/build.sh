#!/bin/sh
# build.sh - build and run the ipc.c host harness smoke test.
#
# Compiles ../ipc.c (unmodified) together
# with the harness (this directory) using plain cc, runs the smoke
# test, then repeats the whole thing under ASan+UBSan. Exits
# non-zero if any step fails.
#
# Layout:
#   harness/          this directory (linux.h shim, stub headers,
#                      stubs.c, hook.{c,h}, harness_smoke.c)
#   ../ipc.c ../ipc.h the files under test
#   build/            plain build artifacts (created as needed)
#   build-asan/       sanitizer build artifacts (created as needed)
#
# Flags:
#   -std=c99 -D_POSIX_C_SOURCE=200809L: the harness is host code
#     (pthreads, clock_gettime); ipc.c itself needs no libc.
#   -I. -I.. : harness/linux.h, launchd.h, bks.h, svchost.h shadow
#     the target headers; ipc.h comes from the work dir above.
#   ipc.c is built with -Wall -Wextra but WITHOUT -Werror: it has
#     one pre-existing -Wunused-function warning
#     (ipc_port_get_name_for_recv is static and currently unused).
#     See README.md. The harness's own
#     files are built with -Werror.
#
# No em dashes are used in this file.
set -eu
cd "$(dirname "$0")"

CC="${CC:-cc}"
STD="-std=c99 -D_POSIX_C_SOURCE=200809L"
WARN="-Wall -Wextra"
HARNESS_WARN="$WARN -Werror"
INCLUDES="-I. -I.."
IPC_SRC="../ipc.c"
HARNESS_SRCS="harness_smoke.c hook.c stubs.c"

build_one() {
    dir="$1"
    extra="$2"
    label="$3"
    echo "=== $label ==="
    rm -rf "$dir"
    mkdir -p "$dir"
    # ipc.c: the file under test, no -Werror (see header comment).
    # shellcheck disable=SC2086
    $CC $STD $WARN $INCLUDES -O1 $extra -c -o "$dir/ipc.o" "$IPC_SRC"
    # Harness files: -Werror.
    for src in $HARNESS_SRCS; do
        # shellcheck disable=SC2086
        $CC $STD $HARNESS_WARN $INCLUDES -O1 $extra -c \
            -o "$dir/$(basename "$src" .c).o" "$src"
    done
    # shellcheck disable=SC2086
    $CC $STD -O1 $extra -o "$dir/harness_smoke" "$dir"/*.o -pthread
    "$dir/harness_smoke"
}

build_one "build" "" "plain build"
build_one "build-asan" "-g -fsanitize=address,undefined -fno-sanitize-recover=all" "ASan+UBSan build"

echo "=== all harness builds passed ==="
