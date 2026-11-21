#!/usr/bin/env bash
# Build and run every host unit test (plain cc + ASan/UBSan). Sources that include "linux.h" are built from a
# private copy of libsystem_kernel/src with tests/host/host-shim/linux.h in place of the aarch64 one.
#
# Features:
#   - HOSTTEST_VARIANT selects the build flavour: plain, asan, or both (default both).
#     Each test binary is named <name>-asan or <name>-plain.
#   - Wired tests:
#       device_log_replay_test - replays the device logs
#         (tests/host/fixtures/touch-019-*.txt) and asserts the exact
#         behavior; the selftest GATE FAILS on the unpatched tree
#         and passes with the fixes (see the test header).
#       e2e_touch_window_test - synthetic presenter DOWN/MOVE/UP
#         through the full chain to window hit-test and tab switch;
#         the wire tap gate FAILS on the unpatched tree.
#       workloop_race_test - drives the kq/wq state machine through
#         the device crash sequence (slot=0, flags 0x464005,
#         nevents=1); a conformance pin for workloop handling,
#         not a red/green gate for the libdispatch trap itself.
#     snapshot_pipeline_test damage-slot race fix: the damage section
#     reads slot (presents_seen - 1) % 128 after the ack instead
#     of sampling presents_seen before the present.
#   - Every run prints one PASS/FAIL/BUILD-FAIL/TIMEOUT/SKIP line per test
#     plus a TOTAL line at the end. The script exits non-zero on any
#     failure. Test binaries that print a check total keep doing so in
#     the PASS line (last output line).
set -euo pipefail
cd "$(dirname "$0")/.."
CC="${CC:-cc}"
VARIANT="${HOSTTEST_VARIANT:-both}"
case "$VARIANT" in
  plain|asan|both) ;;
  *) echo "HOSTTEST_VARIANT must be plain, asan, or both (got: $VARIANT)" >&2; exit 2 ;;
esac

fail=0
pass_count=0
fail_count=0
skip_count=0
total_tests=0

# run <name> [-D...|-I...|-l...] <sources...>
# -D, -I and -l arguments are compiler/linker flags; everything else is a source file.
# Uses globals: B (build dir), F (current flags), SUFFIX (variant suffix).
run() {
  local name=$1 rc=0
  shift
  local defs=()
  local libs=()
  local srcs=()
  local a
  for a in "$@"; do
    case "$a" in
      -D*|-I*) defs+=("$a") ;;
      -l*) libs+=("$a") ;;
      *) srcs+=("$a") ;;
    esac
  done
  set "${srcs[@]}"
  local bin="$B/$name$SUFFIX"
  total_tests=$((total_tests + 1))
  if $CC $F "${defs[@]}" -o "$bin" "$@" -lpthread "${libs[@]}" 2>"$bin.cc.log"; then
    local out
    if out=$(timeout 60 "$bin" 2>&1); then
      echo "PASS $name$SUFFIX: $(echo "$out" | tail -1)"
      pass_count=$((pass_count + 1))
    else
      rc=$?
      if [ "$rc" -eq 124 ]; then echo "TIMEOUT $name$SUFFIX (60s)"; else echo "FAIL $name$SUFFIX"; fi
      echo "$out" | tail -5
      fail=1; fail_count=$((fail_count + 1))
    fi
  else
    echo "BUILD-FAIL $name$SUFFIX"
    head -5 "$bin.cc.log"
    fail=1; fail_count=$((fail_count + 1))
  fi
  return 0
}

skip() {
  local name=$1 reason=$2
  total_tests=$((total_tests + 1))
  skip_count=$((skip_count + 1))
  echo "SKIP $name$SUFFIX: $reason"
}

#   - test_server_resize - drives the
#     presenter server's static on_cmd through CONFIG_CHANGED and
#     APP_CMD_WINDOW_RESIZED with stub Android headers; the
#     WINDOW_RESIZED gate FAILS on the unpatched tree (10 failures).
WIRED_TESTS="xpcwire_test bsxpc_test sim_test iokit_test snapshot_test kevent_pset_test workloop_crash_test present_test hostpresent_test touch_test device_log_replay_test e2e_touch_window_test workloop_race_test kevent_workloop_024_test hidevent_test metaldev_test cfprefsd_test notifyd_test hidsys_test mg_test lsd_test container_test csops_test daemons_test pathmap_test timing_plist_test thread_reg_test psynch_mutex_test cadisplay_test metallib_test bitcode_test air2spirv_test bkshid_test bkshid_deferring_test keyboard_test cadamage_test carender_test snapshot_pipeline_test cadisplay_stale_test cadisplaylink_test heap067_mainscreen_test presenter_geometry_test snapshot_geometry_test hacks_removal_test touch_plumbing_test e2e_touch_present_test snapshot_046_regression_test pump_workqueue_test fuzz_touch_damage_utf8_test render_kq_concurrent_test vm_xzone_test heap067_vm_test heap067_agtable_test heap067_repro_vm_locks_test sem_test necp_test attributegraph_test navigation_test backdrop_test webkit_test test_server_resize device_log_replay_021_test heap067_objc_test heap067_repro_test liquid_glass_test psynch_rwlock_test heap067_lock_test heap067_dict_test heap067_wildwrite_test mem_entry_test heap067_tuikb_validate_test heap067_kblayout_bytes_test"
# run_suite: run the whole list once under the current flags (F) and
# build dir (B). Globals S, P, C point at the prepared source copies.
run_suite() {
  local S=$B/src P=$B/srcp C=$B/srcc
  run xpcwire_test  tests/host/xpcwire_test.c  $S/xpcwire.c
  run bsxpc_test    tests/host/bsxpc_test.c    $S/xpcwire.c $S/bsxpc.c
  run sim_test      tests/host/sim_test.c      $S/xpcwire.c $S/bsxpc.c $S/svchost.c $S/fakeboard.c $S/fakerb.c
  run iokit_test    tests/host/iokit_test.c    $S/iokit.c $S/iosurface.c
  run snapshot_test -DSNAPSHOT_HOST_TEST=1 tests/host/snapshot_test.c $S/snapshot.c
  run navigation_test -DSNAPSHOT_HOST_TEST=1 tests/host/navigation_test.c $S/snapshot.c
  run backdrop_test -DSNAPSHOT_HOST_TEST=1 tests/host/backdrop_test.c $S/snapshot.c
  run kevent_pset_test -DKEVENT_HOST_TEST=1 tests/host/kevent_pset_test.c $S/kevent.c $S/ipc.c
  # ipc.c descriptor-walk bounds, OOL range
  # probe, receive-trailer overrun, ipc_msg_destroy deallocate flag.
  # The W1/W2/W3/W5 gates FAIL on the unpatched tree and pass with
  # the fix.
  run heap067_wildwrite_test -DKEVENT_HOST_TEST=1 tests/host/heap067_wildwrite_test.c $S/ipc.c $S/kevent.c $S/vm.c
  run workloop_crash_test -DKEVENT_HOST_TEST=1 tests/host/workloop_crash_test.c $S/kevent.c
  run workloop_race_test -DKEVENT_HOST_TEST=1 tests/host/workloop_race_test.c $S/kevent.c $S/ipc.c
  # kevent/workloop traps (a) Invalid wlh and (b) sync-waiter
  # ownership handoff. The DISCOVER / ESTALE gates FAIL on the unpatched
  # tree and pass with the fixes (see the test header).
  run kevent_workloop_024_test -DKEVENT_HOST_TEST=1 tests/host/kevent_workloop_024_test.c $S/kevent.c $S/ipc.c
  local F0="$F"
  F="${F/-iquote $B\/src/-iquote $P}"
  run present_test -DPRESENT_TEST tests/host/present_test.c $P/present.c $P/protocol.c
  run hostpresent_test -DPRESENT_TEST -DHOSTPRESENT_LIB -Itools/hostpresent tests/host/hostpresent_test.c tools/hostpresent/hostpresent.c $P/present.c $P/protocol.c $P/touch_input.c
  run touch_test -DPRESENT_TEST -DTOUCH_HOST_TEST=1 tests/host/touch_test.c $P/touch.c $P/present.c $P/protocol.c
  run device_log_replay_test -DPRESENT_TEST -DTOUCH_HOST_TEST=1 tests/host/device_log_replay_test.c $P/touch.c $P/present.c $P/protocol.c
  run e2e_touch_window_test -DPRESENT_TEST -DTOUCH_HOST_TEST=1 tests/host/e2e_touch_window_test.c $P/touch.c $P/present.c $P/protocol.c
  # Replay the device logs (crash B + crash C)
  # through route C with the KIF mock; the tracking gate
  # FAILS on the unpatched tree (TouchUpInside hack fires) and
  # passes with the fix. -I$C finds cadisplay.h for the
  # part 4 vsync liveness check; $P/touch.c + $P/present.c supply
  # the touch pipeline, $C/cadisplay.c the vsync clock.
  run device_log_replay_021_test -DPRESENT_TEST -DTOUCH_HOST_TEST=1 -I$C tests/host/device_log_replay_021_test.c $P/touch.c $P/present.c $P/protocol.c $C/cadisplay.c $S/attributegraph.c
  # touch.c route C lifetime fixes (fallback NSSet dangle,
  # UITouch alloc balance, HID event CFRetain balance). The gates FAIL
  # on the unpatched tree and pass with the fix (see the test header).
  run heap067_objc_test -DPRESENT_TEST -DTOUCH_HOST_TEST=1 tests/host/heap067_objc_test.c $P/touch.c $P/present.c $P/protocol.c
  # End-to-end random-tap repro (route C KIF
  # double-retain RED GATE: 2 intended failures on the unpatched
  # tree). Same F swap as device_log_replay_021_test.
  run heap067_repro_test -DPRESENT_TEST -DTOUCH_HOST_TEST=1 -DAG_HOST_TEST=1 -I$C tests/host/heap067_repro_test.c $P/touch.c $P/present.c $P/protocol.c $C/cadisplay.c $S/attributegraph.c
  F="$F0"
  run hidevent_test tests/host/hidevent_test.c $S/hidevent.c $S/protocol.c
  run metaldev_test tests/host/metaldev_test.c $S/metaldev.c $S/vkshim.c
  $CC $F -DCFP_OPENAT=cfp_host_openat -DCFP_READ=cfp_host_read \
         -DCFP_WRITE=cfp_host_write -DCFP_CLOSE=cfp_host_close \
         -DCFP_RENAMEAT=cfp_host_renameat -DCFP_FCNTL=cfp_host_fcntl \
         -c $S/cfprefsd.c -o $B/cfprefsd.o
  run cfprefsd_test tests/host/cfprefsd_test.c $B/cfprefsd.o $S/svchost.c $S/bplist.c $S/bsxpc.c $S/container.c $S/xpcwire.c
  run notifyd_test  tests/host/notifyd_test.c  $S/notifyd.c $S/xpcwire.c $S/bplist.c $S/bsxpc.c
  run hidsys_test   tests/host/hidsys_test.c   $S/hidsys.c
  run mg_test       tests/host/mg_test.c       $S/mg.c $S/bplist.c $S/xpcwire.c $S/container.c $S/svchost.c $S/bsxpc.c
  run lsd_test      tests/host/lsd_test.c      $S/lsd.c $S/svchost.c $S/xpcwire.c $S/bsxpc.c $S/bplist.c $S/container.c $S/csops.c
  run webkit_test   tests/host/webkit_test.c   $S/launchd.c $S/xpcwire.c
  run container_test tests/host/container_test.c $S/container.c
  run csops_test    tests/host/csops_test.c    $S/csops.c $S/bplist.c
  run daemons_test  tests/host/daemons_test.c  $S/daemons.c $S/svchost.c $S/bplist.c $S/xpcwire.c
  run pathmap_test  -DPATHMAP_HOST_TEST tests/host/pathmap_test.c $S/pathmap.c
  run timing_plist_test -DTIMING_PLIST_PATH='"sysroot-overlay/System/Library/EventTimingProfiles/D47.Default.plist"' tests/host/timing_plist_test.c
  run liquid_glass_test tests/host/liquid_glass_test.c $S/solarium.c
  run thread_reg_test -DTHREAD_HOST_TEST=1 tests/host/thread_reg_test.c $S/thread.c
  run psynch_mutex_test -DTHREAD_HOST_TEST=1 tests/host/psynch_mutex_test.c $S/thread.c
  run psynch_rwlock_test -DTHREAD_HOST_TEST=1 -latomic tests/host/psynch_rwlock_test.c $S/thread.c
  # ulock 64-bit wait lost wakeup (dispatch_once hangs)
  # plus the psynch mutexwait EINTR grant-loss hardening. The H2
  # gates FAIL on the unpatched tree and pass with the fix.
  run heap067_lock_test -DTHREAD_HOST_TEST=1 tests/host/heap067_lock_test.c $S/thread.c
  # TUIKBGraphSerialization _deserializationCache race.
  # Phase A expects the race (base behaviour), phase B expects exactly
  # one decode per offset (guarded). `base` mode FAILS by design.
  run heap067_dict_test -DTUIKB_LOCK_HOST_TEST=1 tests/host/heap067_dict_test.c $S/tuikb_lock.c
  F0="$F"
  F="${F/-iquote $B\/src/-iquote $C}"
  run cadisplay_test tests/host/cadisplay_test.c $C/cadisplay.c $S/attributegraph.c
  F="$F0"
  local A=gpu/air2spirv/src
  for t in metallib bitcode air2spirv; do run ${t}_test -I$A gpu/air2spirv/tests/host/${t}_test.c $A/$t.c; done
  # Tests wired explicitly from tests/host/.
  run bkshid_test -DBKSHID_HOST_TEST=1 tests/host/bkshid_test.c $S/bkshid.c $S/bsxpc.c $S/xpcwire.c $S/svchost.c
  run bkshid_deferring_test -DBKSHID_HOST_TEST=1 tests/host/bkshid_deferring_test.c $S/bkshid.c $S/bsxpc.c $S/xpcwire.c $S/svchost.c
  F0="$F"
  F="${F/-iquote $B\/src/-iquote $P}"
  run keyboard_test -DPRESENT_TEST -DKEYBOARD_HOST_TEST=1 tests/host/keyboard_test.c $P/keyboard.c $P/present.c $P/protocol.c
  run touch_plumbing_test -DPRESENT_TEST -DTOUCH_HOST_TEST=1 tests/host/touch_plumbing_test.c $P/touch.c $P/present.c $P/protocol.c $P/touch_input.c $P/hidevent.c
  run e2e_touch_present_test -DPRESENT_TEST -DTOUCH_HOST_TEST=1 tests/host/e2e_touch_present_test.c $P/touch.c $P/present.c $P/protocol.c
  run fuzz_touch_damage_utf8_test -DTOUCH_HOST_TEST=1 tests/host/fuzz_touch_damage_utf8_test.c $P/touch.c $P/present.c $P/protocol.c
  run snapshot_pipeline_test -DSNAPSHOT_HOST_TEST=1 -DPRESENT_TEST tests/host/snapshot_pipeline_test.c $P/snapshot.c $P/present.c $P/present_api.c $P/protocol.c
  F="$F0"
  run cadamage_test tests/host/cadamage_test.c $S/cadamage.c
  run carender_test -DCARENDER_HOST_TEST=1 tests/host/carender_test.c $S/carender.c $S/cadamage.c
  # Frames + kq events driven concurrently. All sources
  # build under the $S shim (snapshot.c's presenter calls are weak
  # stubs when present.c is not linked), one cc invocation.
  run render_kq_concurrent_test -DCARENDER_HOST_TEST=1 -DSNAPSHOT_HOST_TEST=1 -DKEVENT_HOST_TEST=1 tests/host/render_kq_concurrent_test.c $S/carender.c $S/cadamage.c $S/snapshot.c $S/kevent.c $S/ipc.c
  run snapshot_geometry_test -DSNAPSHOT_HOST_TEST=1 tests/host/snapshot_geometry_test.c $S/snapshot.c
  run snapshot_046_regression_test -DSNAPSHOT_HOST_TEST=1 tests/host/snapshot_046_regression_test.c $S/snapshot.c
  run hacks_removal_test -DSNAPSHOT_HOST_TEST=1 tests/host/hacks_removal_test.c $S/snapshot.c
  run presenter_geometry_test tests/host/presenter_geometry_test.c $S/protocol.c
  # AttributeGraph update-cycle precondition guard.
  run attributegraph_test -DAG_HOST_TEST=1 tests/host/attributegraph_test.c $S/attributegraph.c
  run pump_workqueue_test -DTHREAD_HOST_TEST=1 tests/host/pump_workqueue_test.c $S/thread.c
  # VM layer defects behind the xzone SIGTRAP (deallocate
  # rounding leak, FIXED clobber, aligned-allocation over-allocation).
  # vm.c needs no -D flag; the host-shim linux.h supplies sys_mmap /
  # sys_munmap / sys_mprotect and MAP_FIXED_NOREPLACE.
  run vm_xzone_test tests/host/vm_xzone_test.c $S/vm.c
  # TUIKB deserializationCache
  # validation swizzle (bisect tool, off by default on device).
  run heap067_tuikb_validate_test -DTUIKB_VALIDATE_HOST_TEST=1 tests/host/heap067_tuikb_validate_test.c $S/tuikb_validate.c
  # VM layer defects (vm_region fabrication, remap
  # copy/FIXED handling, vm_copy validation, 16 KiB rounding). The
  # D1-D4 gates FAIL on the unpatched tree and pass with the fix.
  run heap067_vm_test tests/host/heap067_vm_test.c $S/vm.c
  # AG table grow_region remap (protection inheritance
  # plus copy=FALSE move). The post-grow write gate FAILS on the
  # unpatched tree (child dies of SIGSEGV) and passes with the fix.
  run heap067_agtable_test tests/host/heap067_agtable_test.c $S/vm.c
  # VM/locks repro (T1 TUIKB concurrent decode,
  # T2 remap clobber, T5 dict backing UAF, T6 pool/TSD mixing). The
  # 4 remap gates are intended failures on the unpatched tree.
  run heap067_repro_vm_locks_test -DAG_HOST_TEST=1 -DTHREAD_HOST_TEST=1 tests/host/heap067_repro_vm_locks_test.c $S/vm.c $S/thread.c $S/attributegraph.c
  # mach_make_memory_entry_64 implemented;
  # re-enabled. Needs the carender_init stub in the test (carender_init
  # was moved into ipc.c's 50th-send lazy arm).
  run mem_entry_test tests/host/mem_entry_test.c $S/vm.c $S/ipc.c
  # KBLayouts .dat bytes through lsk_mmap;
  # skips the real-file part if the sysroot
  # .dat files are absent, synthetic part always runs.
  run heap067_kblayout_bytes_test tests/host/heap067_kblayout_bytes_test.c $S/vm.c
  run sem_test tests/host/sem_test.c $S/sem.c
  run necp_test tests/host/necp_test.c $S/necp.c
  # Presenter server resize wiring. The test #includes
  # server.c (white-box access to static on_cmd/g_s), so server.c is
  # not a link source; the Android APIs it calls are stubbed by
  # android/surface/tests/host/stubs (test-only, never in the APK).
  run test_server_resize -Iandroid/surface/tests/host/stubs -Iandroid/surface/src/common -Iandroid/surface/src/server android/surface/tests/host/test_server_resize.c android/surface/src/server/ui.c android/surface/src/server/sysinfo.c android/surface/src/common/protocol.c android/surface/src/common/touch_input.c android/surface/tests/host/stubs/test_server_stubs.c
  F0="$F"
  F="${F/-iquote $B\/src/-iquote $C}"
  run cadisplay_stale_test tests/host/cadisplay_stale_test.c $C/cadisplay.c $S/attributegraph.c
  run cadisplaylink_test -DCADISPLAYLINK_HOST_TEST=1 tests/host/cadisplaylink_test.c $C/cadisplay.c $S/attributegraph.c
  # +[UIScreen mainScreen] nil-screen boot race. The
  # hook waits bounded for the first scene instead of throwing.
  run heap067_mainscreen_test -DMAINSCREEN_HOST_TEST=1 tests/host/heap067_mainscreen_test.c $C/mainscreen.c
  F="$F0"
}

# setup_build_dir <dir>: prepare the private source copies for one variant.
setup_build_dir() {
  local dir=$1
  B="$dir"
  rm -rf "$B"
  mkdir -p "$B/src" "$B/srcp" "$B/srcc"
  cp libsystem_kernel/src/*.[ch] "$B/src/"
  cp tests/host/host-shim/linux.h "$B/src/linux.h"
  cp android/surface/src/common/protocol.[ch] "$B/src/"
  cp android/surface/src/common/touch_input.[ch] "$B/src/"
  cp "$B/src/"*.[ch] "$B/srcp/"
  cp tests/host/host-shim-present/linux.h "$B/srcp/linux.h"
  cp libsystem_kernel/src/cadisplay.[ch] libsystem_kernel/src/present.h libsystem_kernel/src/iosurface.h libsystem_kernel/src/iokit.h "$B/srcc/"
  cp libsystem_kernel/src/attributegraph.[ch] "$B/srcc/" # cadisplay.c includes it for tick wiring
  cp libsystem_kernel/src/mainscreen.[ch] "$B/srcc/" # mainScreen hook, wired next to cadisplaylink_test
  cp tests/host/cadisplay-mock/*.h "$B/srcc/"
}

if [ "$VARIANT" = "plain" ] || [ "$VARIANT" = "both" ]; then
  SUFFIX="-plain"
  setup_build_dir build/hosttests-plain
  F="-std=gnu11 -O1 -g -w -iquote $B/src -iquote tests/host"
  run_suite
fi
if [ "$VARIANT" = "asan" ] || [ "$VARIANT" = "both" ]; then
  SUFFIX="-asan"
  setup_build_dir build/hosttests
  F="-std=gnu11 -O1 -g -w -fsanitize=address,undefined -fno-sanitize-recover=all -iquote $B/src -iquote tests/host"
  run_suite
fi

echo "TOTAL: $total_tests tests, $pass_count passed, $fail_count failed, $skip_count skipped (variant: $VARIANT)"
exit $fail
