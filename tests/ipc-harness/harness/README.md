# ipc.c host harness

Runs `libsystem_kernel/src/ipc.c` (the copy at `../ipc.c`) on the
Linux host with plain cc, so scenario tests can exercise
Mach IPC without the device. ipc.c itself is compiled unmodified;
everything host-specific lives in this directory.

## Files

| file | what it is |
| --- | --- |
| `linux.h` | Host shim for the target `linux.h`. Same aarch64 syscall numbers, implemented with libc / the x86_64 syscalls (futex 202, mmap 9, munmap 11). Must be first on the `-I` path. |
| `launchd.h`, `bks.h`, `svchost.h` | Minimal stub headers declaring only the `*_process_msg` function ipc.c calls. They shadow the real headers (same filenames); the real `svchost.h` would pull in xpcwire.h. |
| `stubs.c` | Host-only definitions for the symbols ipc.c needs from the rest of libsystem_kernel (see "Stubs" below). |
| `hook.h`, `hook.c` | Mock `g_enqueue_hook`: records every enqueue as (port name, msgh_size) in a log the tests can inspect. |
| `harness_smoke.c` | The smoke test (see "What the smoke test proves"). |
| `build.sh` | Builds and runs the smoke test with plain cc, then again under ASan+UBSan. |

## Build and run

```
./build.sh
```

It creates `build/` (plain) and `build-asan/` (sanitizers) and runs
both binaries. It works from a clean checkout and from any working
directory. Exit code is non-zero if any step fails.

To run one binary by hand:

```
./build/harness_smoke          # plain
./build-asan/harness_smoke     # ASan+UBSan
```

To rebuild only the plain binary with tracing on, edit
`harness_smoke.c` to call `ipc_set_trace(1)` after `ipc_init()`;
ipc.c then logs every send/receive to stderr.

## What the smoke test proves

`harness_smoke.c` runs 31 checks, 0 failures, plain and under
ASan+UBSan (verified 5 runs each, plus a clean rebuild):

- **round_trip**: a receiver thread blocks in `ipc_msg` on a plain
  receive port; the main thread sends it a request carrying a
  send-once reply port; the receiver answers on the send-once
  right and the main thread receives the reply. Checks the message
  contents both ways, that the receiver really blocked (still not
  done 200 ms after it started waiting, with no message queued),
  and that the enqueue hook logged both enqueues with the right
  port and size.
- **receive_timeout**: receiving on an empty port with
  `MACH_RCV_TIMEOUT` returns `MACH_RCV_TIMED_OUT` after about the
  requested time (measured 100 ms for a 100 ms timeout) instead of
  hanging.

## How to add a new scenario test

1. Add a `test_<name>(void)` function in `harness_smoke.c` (or a
   new `*_test.c` file; if you add a file, add it to `HARNESS_SRCS`
   in `build.sh`), call it from `main()`, and use the `CHECK` /
   `CHECK_ERR` macros. `main` returns 1 if any check fails.
2. Setup, per test: `ipc_init()` once (it is idempotent; the smoke
   test calls it in the round-trip test), then
   `harness_hook_install()` if the test inspects the hook log.
   The hook log is per-process: call `harness_hook_clear()` between
   tests if you want a fresh log, or use `harness_hook_find(port,
   size)` which searches the whole log.
3. Ports, the way the device tests do it (`tests/libsys/mach_test.c`):
   allocate a RECEIVE right, then insert a SEND right on the same
   name:
   ```
   mach_port_t task = ipc_task_self(); /* 0x103 */
   mach_port_name_t srv;
   ipc_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &srv);
   ipc_port_insert_right(task, srv, srv, MACH_MSG_TYPE_MAKE_SEND);
   ```
   Do NOT allocate a DEAD_NAME and insert over it: the send path
   treats any entry with the DEAD_NAME bit as dead, even if a SEND
   right was added to it.
4. Reply ports: `ipc_mach_reply_port()` gives a receive port for
   replies. Carry it as the message's local port with disposition
   `MACH_MSG_TYPE_MAKE_SEND_ONCE` (remote disposition
   `MACH_MSG_TYPE_COPY_SEND` for the destination). On receive, the
   peer sees the reply right as `msgh_remote_port` with bits
   remote = `MACH_MSG_TYPE_PORT_SEND_ONCE`; answer it with
   `MACH_MSG_TYPE_MOVE_SEND_ONCE` as the remote disposition.
5. Receiving: `ipc_msg(buf, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
   sizeof(buf), name, timeout_ms, 0, NULL, 0)`. The buffer must be
   at least the message size plus the trailer (8 bytes by
   default). Always pass a timeout in tests so a broken wake-up
   fails the test instead of hanging the run. After a receive,
   `msgh_local_port` is the receiving port's name and
   `msgh_remote_port` is the reply right (or NULL if the sender
   carried none).
6. Cleanup: `ipc_port_destroy(task, name)` destroys a port and
   drains its queue. `ipc_port_deallocate` only drops one send
   reference; it does not destroy a receive right.
7. Hook log: `harness_hook_count()`, `harness_hook_get(i)`,
   `harness_hook_find(port, size)`, `harness_hook_dropped()`.
   The hook runs under `ipc_lock`, so a test must never call an
   `ipc_*` function from inside a hook callback. The log holds
   4096 entries; beyond that calls are dropped and counted.

## Stubs (what is faked, and what it means)

`stubs.c` provides host-only definitions for the symbols ipc.c
needs from the rest of libsystem_kernel. None of these ship in
the freestanding dylib.

- `launchd_process_msg`, `bks_display_process_msg`,
  `svchost_process_msg`, `iokit_process_msg`: return 0, meaning
  "not handled". ipc.c's kobject dispatch then sends no reply for
  that message (it still returns `MACH_MSG_SUCCESS` and fires the
  enqueue hook with the reply name and a zero reply size). So
  sends to kobject ports (bootstrap 0x203, BKS display, service
  ports, IOKit ports) succeed in the harness but produce no reply.
  Do not wait on a reply port for one of those sends without a
  timeout. Service tests replace these stubs with the real
  dispatch files (launchd.c, bks.c, svchost.c, iokit.c) or with
  richer scripted stubs.
- `mach_absolute_time`: `CLOCK_MONOTONIC` nanoseconds. Paired
  with `mach_timebase_info` returning {1, 1}, ticks are
  nanoseconds and the receive-timeout math in `ipc_rcv_locked`
  works as-is.
- `mach_vm_allocate` / `mach_vm_deallocate`: libc mmap/munmap.
  The target argument (ipc.c passes 0x103) and the flags are
  ignored. Used for OOL message copies and for `ipc_port_names`
  buffers.

The four `mach_*` functions are declared in ipc.c with asm labels
(`"_mach_absolute_time"` and friends). The stubs repeat the asm
label on a separate declaration before the plain definition; an
asm label written directly on a definition is rejected by gcc.

## What ipc.c needs from linux.h

ipc.c uses only these host primitives, all provided by the shim
in this directory:

- `sys0(SYS_getpid)` (trailer pid), `sys3`/`sys4(SYS_futex, ...)`
  (the ipc mutex and the generation futex the receiver waits
  on), `sys_write` (trace logging to fd 2), `sys_mmap` /
  `sys_munmap` (large-message fallback in `kmsg_alloc`).
- `PROT_READ`, `PROT_WRITE`, `MAP_PRIVATE`, `MAP_ANONYMOUS`,
  `FUTEX_WAIT_PRIVATE`, `FUTEX_WAKE_PRIVATE`.

ipc.c does NOT reference kevent.c or workq.c at all: the only
coupling point is `g_enqueue_hook`, which this harness installs
directly with `ipc_set_enqueue_hook`. No kevent/workq stubs are
needed here. (The lock-order checker in harness-tests uses the
real kevent.c/workq.c pieces.)

## Required changes to ipc.c: none

ipc.c as it stands in this work dir compiles and runs on the host
with no source change. No change is needed
for the harness to work.

Two things to know about:

- `ipc_port_get_name_for_recv` (ipc.c:481) is static and currently
  unused, so compiling ipc.c with `-Wall` prints one
  `-Wunused-function` warning. `build.sh` therefore builds ipc.c
  with `-Wall -Wextra` but WITHOUT `-Werror`; the harness's own
  files are built with `-Werror`. If that
  function is removed or used, the warning goes away.
- The harness compiles with `-std=c99
  -D_POSIX_C_SOURCE=200809L`. Do not switch the harness to
  `-std=gnu99`: the point is to prove ipc.c builds in the same
  strict mode the other host tests use.

## What works / what does not

Works: ipc.c compiles and links on the x86_64 host with plain cc
and under ASan+UBSan; two-thread send/receive with a send-once
reply round-trips correctly; blocking receive wakes on
`ipc_notify_all`; receive timeout returns `MACH_RCV_TIMED_OUT`
after about the requested time; the enqueue hook log records
every enqueue; `mach_vm_allocate`/`mach_vm_deallocate` back the
OOL path; `ipc_port_names` allocates its buffers through the
stub (not exercised by the smoke test).

Out of scope for this smoke harness: kobject dispatch
(launchd, BKS, svchost, IOKit) is stubbed and produces no reply;
no kevent/workq integration in this directory; full scenario tests
(notifications, MOVE_RECEIVE, port name reuse, special reply port,
MACH_RCV_TOO_LARGE, sync send+wait) live in harness-tests/;
no device testing (this is a host harness by design).

## Layout note

The harness is self-contained in this directory except for
`../ipc.c` and `../ipc.h`, which it reads but never modifies.
`build.sh` puts its objects in `build/` and `build-asan/`; both
are generated and can be deleted at any time.
