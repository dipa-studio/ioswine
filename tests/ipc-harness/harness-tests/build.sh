#!/bin/sh
# build.sh - build and run the ipc-xnu scenario tests.
#
# Builds every test in this directory against the chosen ipc.c,
# plain and under ASan+UBSan, runs each binary, then runs the
# static lock-order analysis (lock_order.py). Exits non-zero if
# any test binary fails or the lock-order check reports a
# violation.
#
#   IPC_SRC=base    (default) use the reference tree ipc.c
#   IPC_SRC=patched use the patched copy
#
# Failures against IPC_SRC=base are EXPECTED for the scenarios the
# patch fixes (notifications hook, TSD[8] staleness, stale
# descriptor pointer, lock-order cycle). A clean run under IPC_SRC=patched
# means the patch satisfies every scenario.
#
# Layout:
#   harness-tests/  this directory (test_*.c, tsd8_port.*, lock_order.py)
#   ../harness/     the smoke harness, reused read-only
#                   (linux.h shim, stub headers, hook.{c,h}, stubs.c)
#   $SRC_DIR        ipc.c / ipc.h / kevent.c / workq.c under test
#   build/          plain build artifacts (created as needed)
#   build-asan/     sanitizer build artifacts (created as needed)
#
# Flags (same as the harness): -std=c99 -D_POSIX_C_SOURCE=200809L.
# -I. -I../harness -I$SRC_DIR: harness/linux.h and the stub headers
# shadow the target headers; ipc.h comes from $SRC_DIR. ipc.c is
# built with -Wall -Wextra but WITHOUT -Werror (one pre-existing
# -Wunused-function warning, ipc_port_get_name_for_recv); the test
# and harness files are built with -Werror.
#
# No em dashes are used in this file.
set -eu
cd "$(dirname "$0")"

CC="${CC:-cc}"
STD="-std=c99 -D_POSIX_C_SOURCE=200809L"
WARN="-Wall -Wextra"
TEST_WARN="$WARN -Werror"
HARNESS_DIR="../harness"
PATCHED_SRC="${PATCHED_SRC:-$(cd "$(dirname "$0")/../../.." && pwd)/libsystem_kernel/src}"
BASE_SRC="${BASE_SRC:-$PATCHED_SRC}"

IPC_SRC="${IPC_SRC:-base}"
case "$IPC_SRC" in
    base)
        SRC_DIR="$BASE_SRC"
        TSD8_PATCHED=0
        ;;
    patched)
        SRC_DIR="$PATCHED_SRC"
        TSD8_PATCHED=1
        ;;
    *)
        echo "IPC_SRC must be base or patched (got '$IPC_SRC')" >&2
        exit 2
        ;;
esac

IPC_C="$SRC_DIR/ipc.c"
INCLUDES="-I. -I$HARNESS_DIR -I$SRC_DIR"
HARNESS_SRCS="$HARNESS_DIR/hook.c $HARNESS_DIR/stubs.c"

# test_name -> extra .c files (besides test_<name>.c)
# test_special_reply needs the TSD[8] host port.
extra_srcs() {
    case "$1" in
        test_special_reply) echo "tsd8_port.c" ;;
        *) echo "" ;;
    esac
}

TESTS="test_notifications test_move_receive test_port_names test_special_reply test_too_large test_sync_send test_m6"

PASS_COUNT=0
FAIL_COUNT=0
FAILED_TESTS=""

run_binary() {
    _rb_bin="$1"
    _rb_label="$2"
    echo "--- run $_rb_label ---"
    if "$_rb_bin"; then
        echo "RESULT $_rb_label: PASS"
        PASS_COUNT=$((PASS_COUNT + 1))
    else
        echo "RESULT $_rb_label: FAIL"
        FAIL_COUNT=$((FAIL_COUNT + 1))
        FAILED_TESTS="$FAILED_TESTS $_rb_label"
    fi
}

build_one() {
    dir="$1"
    extra="$2"
    label="$3"
    echo "=== $label (IPC_SRC=$IPC_SRC, TSD8_PATCHED=$TSD8_PATCHED) ==="
    rm -rf "$dir"
    mkdir -p "$dir/stage"
    # Stage ipc.c into a directory of its own: ipc.c uses quoted
    # #includes ("linux.h", "launchd.h", "bks.h", "svchost.h") and
    # gcc searches the including file's directory first, so
    # compiling it in place in the reference tree would pick up the
    # target (aarch64) linux.h and the real svchost.h instead of the
    # harness shim and stub headers. Only ipc.c is staged, not
    # ipc.h: every #include "ipc.h" (from ipc.c, the stub headers,
    # stubs.c, hook.c and the tests) must resolve to the same
    # $SRC_DIR/ipc.h or #pragma once cannot deduplicate it. The
    # staged copy is generated and deleted on rebuild; the sources
    # are never modified.
    cp "$SRC_DIR/ipc.c" "$dir/stage/"
    # ipc.c: the file under test, no -Werror (see header comment).
    # shellcheck disable=SC2086
    $CC $STD $WARN $INCLUDES -O1 $extra -c -o "$dir/ipc.o" "$dir/stage/ipc.c"
    # Harness files: -Werror.
    for src in $HARNESS_SRCS; do
        # shellcheck disable=SC2086
        $CC $STD $TEST_WARN $INCLUDES -O1 $extra -c \
            -o "$dir/$(basename "$src" .c).o" "$src"
    done
    for t in $TESTS; do
        # shellcheck disable=SC2086
        $CC $STD $TEST_WARN $INCLUDES -O1 $extra \
            -DTSD8_PATCHED=$TSD8_PATCHED \
            -c -o "$dir/$t.o" "$t.c"
        for x in $(extra_srcs "$t"); do
            # shellcheck disable=SC2086
            $CC $STD $TEST_WARN $INCLUDES -O1 $extra \
                -DTSD8_PATCHED=$TSD8_PATCHED \
                -c -o "$dir/$(basename "$x" .c).o" "$x"
        done
        # shellcheck disable=SC2086
        $CC $STD -O1 $extra -o "$dir/$t" "$dir/ipc.o" "$dir/hook.o" \
            "$dir/stubs.o" "$dir/$t.o" \
            $(for x in $(extra_srcs "$t"); do echo "$dir/$(basename "$x" .c).o"; done) \
            -pthread
        run_binary "$dir/$t" "$label:$t"
    done
    echo "--- $label lock-order (static) ---"
    if python3 lock_order.py --ipc "$IPC_C" \
            --kevent "$SRC_DIR/kevent.c" --workq "$SRC_DIR/workq.c"; then
        echo "RESULT $label:lock_order: PASS"
        PASS_COUNT=$((PASS_COUNT + 1))
    else
        echo "RESULT $label:lock_order: FAIL"
        FAIL_COUNT=$((FAIL_COUNT + 1))
        FAILED_TESTS="$FAILED_TESTS $label:lock_order"
    fi
}

build_one "build" "" "plain build"
build_one "build-asan" "-g -fsanitize=address,undefined -fno-sanitize-recover=all" "ASan+UBSan build"

echo "=== summary (IPC_SRC=$IPC_SRC) ==="
echo "passed: $PASS_COUNT, failed: $FAIL_COUNT"
if [ -n "$FAILED_TESTS" ]; then
    echo "failed tests:$FAILED_TESTS"
fi
if [ "$FAIL_COUNT" -gt 0 ]; then
    if [ "$IPC_SRC" = "base" ]; then
        echo "note: failures against base are expected for the scenarios"
        echo "the patch fixes; see RESULTS.md for the expected-before-patch table."
    fi
    exit 1
fi
echo "=== all harness-tests passed ==="
