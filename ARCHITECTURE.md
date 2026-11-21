# ioswine architecture

Wine-like layer that runs arm64/arm64e iOS Mach-O code directly on an arm64 Android kernel.
No CPU emulation, no VM. One iOS app = one Linux process started by our static loader.

This is the original design document. The milestone notes are kept as a record of how the project got here.

## 0. Goals
- Run third-party iOS apps (own backends, unencrypted IPAs supplied by the user) natively on rooted Android.
  First real target: Aidoku 0.9-4.
- Secondary, not urgent, keep in mind in every design: **look like a stock, sandboxed iPhone** to the app. No jailbreak
  artifacts (paths, URL schemes, injected dylibs, debugger flags), sandbox-correct failures (fork, writes outside the
  container), plausible sysctl/kernel/commpage answers, and later integrity checks of Apple code that we patch (TLS sites,
  lsk trampolines) should be able to see original bytes. Do not make design choices that would make this impossible.
- Out of reach by design: App Attest / DeviceCheck (server-verified Apple hardware keys), Apple services that verify the
  device (Maps, iCloud).

## 1. Process model

```
 Linux process (bionic-free)
 ┌───────────────────────────────────────────────────────────────┐
 │ ioswine-loader  static ELF, NDK clang, freestanding, raw svc #0│
 │   - maps Mach-O main image, dyld shared cache, commpage        │
 │   - applies fixups (rebase/bind, chained, arm64e PAC re-sign)  │
 │   - installs seccomp "Darwin syscall trap" + SIGSYS handler    │
 │   - builds Darwin initial stack (argc/argv/envp/apple[])       │
 │   - jumps to entry, never returns                              │
 │                                                                │
 │ Apple code (user-supplied IPSW, vendor-local/ only)            │
 │   libSystem.B, libsystem_c, libdispatch, ... from shared cache │
 │                                                                │
 │ our libsystem_kernel.dylib (arm64e Mach-O, freestanding C)     │
 │   replaces Apple's: every export → our impl via trampolines    │
 │   implements BSD syscalls, Mach traps, TSD on raw Linux svc #0 │
 └───────────────────────────────────────────────────────────────┘
```

Rules:
- No bionic, no libc in the process. The loader and our dylib each carry their own tiny runtime.
- `tpidr_el0` belongs to the iOS side (the Darwin TSD base after TLS patching, see §6).
  The loader never uses `tpidr_el0` after handoff (no compiler TLS in the loader: `-ftls-model` unused,
  no `__thread`, `-fno-stack-protector` because the stack guard reads TLS on Android).
- Later: one Android Activity per iOS process; the Java/Kotlin side spawns the loader via a
  root-less path. Out of scope until UIKit.

## 2. Address space (Linux arm64, 39-bit VA minimum = 512 GiB user space)

| range                         | what                                            |
|-------------------------------|-------------------------------------------------|
| 0x0 – 0x1_0000_0000           | `__PAGEZERO` of main image, kept unmapped       |
| 0x1_0000_0000 + slide         | main executable (slide 0 in M1)                 |
| 0x1_8000_0000 …               | dyld shared cache (arm64e cache base, unslid)   |
| 0xF_FFFF_C000 (16 KiB)        | Darwin commpage `_COMM_PAGE64_BASE_ADDRESS`     |
| 0x70_0000_0000                | loader image (`--image-base`, ET_EXEC)          |
| kernel-chosen (top)           | loader stack, vdso, our mmap arena              |

TODO M2: `__DATA_CONST` (flag SG_READ_ONLY) must become read-only after fixups like dyld does.

Every fixed mapping uses `MAP_FIXED_NOREPLACE` (kernel >= 4.17) so a collision fails loudly instead of clobbering.
Commpage address and cache base must be re-verified against the pinned IPSW (M2).

## 3. Page size

Mach-O arm64 segments are 16 KiB aligned (file offset and vmaddr), therefore valid on both 4 KiB and
16 KiB kernels. The loader reads `AT_PAGESZ` and:
- maps each segment with `mmap(fileoff, vmaddr)` when both are host-page aligned (always true for 4K/16K),
- zero-fills the tail of the last file page beyond `filesize` and maps anonymous memory for the rest of `vmsize`,
- applies final protections with `mprotect` after fixups.
On a 4 KiB kernel there is no issue. On 64 KiB kernels (not supported) it would need copying.

## 4. Mach-O loading (loader/macho.c)

Supported in M1: `MH_EXECUTE`, 64-bit, cputype ARM64, subtype ALL or ARM64E.
Load commands handled: `LC_SEGMENT_64`, `LC_MAIN`, `LC_UNIXTHREAD`, `LC_DYLD_CHAINED_FIXUPS`,
`LC_DYLD_INFO(_ONLY)` (rebase opcodes only in M1), `LC_LOAD_DYLINKER` (ignored), others skipped.
M1 refuses images that import symbols (no dylibs yet).

Entry:
- `LC_MAIN`: we act as dyld. Call `main(argc, argv, envp, apple)` on the Darwin stack, then
  `exit(ret)` through the loader's own raw syscall.
- `LC_UNIXTHREAD`: jump to `pc` with `sp` pointing at `argc` (kernel-style Darwin stack).

Darwin initial stack (top → bottom): strings, then
`[argc][argv...][0][envp...][0][apple...][0]`. `apple[0] = "executable_path=<path>"`.
Later also `ptr_munge=`, `main_stack=`, `executable_file=`, `dyld_file=`, `th_port=`.

## 5. Darwin syscalls

Darwin arm64 ABI: `svc #0x80`, number in `x16` (>0 BSD, <0 Mach trap, `0x80000000` machdep),
args x0–x7, error = carry flag set (`PSTATE.C`) with errno in x0; success clears C.
Linux ignores the svc immediate and dispatches on `x8`, so a Darwin svc would run a random Linux
syscall. Two mechanisms:

1. **Primary (M2+):** our `libsystem_kernel` contains every syscall wrapper the system libraries use,
   implemented in C on raw Linux `svc #0`. No trap involved. Fast path.
2. **Safety net (M1+): seccomp trap.** Before handoff the loader installs a seccomp BPF filter:
   - syscalls whose `instruction_pointer` is inside the allowed ranges (loader text, later our
     libsystem_kernel text) → `SECCOMP_RET_ALLOW`,
   - everything else → `SECCOMP_RET_TRAP` → `SIGSYS` → loader handler decodes the Darwin call from
     `x16`/x0–x7 in the `ucontext`, performs it, writes x0/x1, sets/clears `PSTATE.C` (bit 29),
     returns. `pc` already points after the svc.
   - the SIGSYS handler uses `SA_RESTORER` with a restorer inside loader text, so `rt_sigreturn`
     itself passes the filter. `PR_SET_NO_NEW_PRIVS` is set first (no root needed).
   This catches raw `svc #0x80` in apps (anti-debug, Go/Rust runtimes) and is how M1's test prints.

The Darwin→Linux mapping table (numbers, flag translation, errno translation, struct layouts) lives in
`libsystem_kernel/` (generated parts in `gen/`). The loader's SIGSYS handler only knows the few calls
the M1 tests need (`exit`=1, `write`=4) until the shared table exists.

## 6. arm64e / PAC

Requirement: device has `paca`+`pacg` in `/proc/cpuinfo` Features (non-hint PAC instructions SIGILL
otherwise). Linux enables PAC keys per process (`PR_PAC_RESET_KEYS`, `PR_PAC_SET_ENABLED_KEYS`), key
values are random per exec. Therefore:
- Every authenticated pointer in Apple images must be **signed at load time by us** with the
  process's live keys: the loader decodes chained fixups (`DYLD_CHAINED_PTR_ARM64E*`) and the
  shared cache slide info (v5 for arm64e caches), and for each auth fixup computes
  `pac{ia,ib,da,db}(target, diversity ^ (addrDiv ? location : 0))` with the right key using the
  real `pacia/pacib/pacda/pacdb` instructions (blend: `diversity<<48 | (addr & 0xffffffffffff)`).
- The loader is compiled as plain arm64 but executes PAC instructions via inline asm; it must run
  in the same process (keys are per-process, preserved across our handoff since we never exec).
- Darwin uses IA for code pointers with discriminator 0 for C function pointers, IB for return
  addresses (`pacibsp`/`retab`), DA/DB for data. Linux keeps all four keys enabled by default.

### ld64.lld limitation (verified 2026-11-09, NDK 28 lld)
`ld64.lld -arch arm64e` links objects **without** authenticated relocations, but fails on any
`ARM64_RELOC_AUTHENTICATED_POINTER` ("INVALID relocation") and refuses `-fixup_chains` for arm64e.
Consequences:
- Our arm64e dylib must contain **no statically initialized function pointers** (no function
  pointer tables in data, no C++ vtables). Build such tables at runtime. `-fptrauth-calls` codegen is
  fine; it is only static data relocs that break.
- Test programs that need arm64e auth fixups must be hand-assembled or come from Apple binaries.
- arm64 (non-e) tests can use `-fixup_chains` (pointer_format 2 = `DYLD_CHAINED_PTR_64`).

## 7. TLS / TSD (M2)

Darwin reads its TSD base with `mrs xN, tpidrro_el0` (plus masking of low bits on some paths).
`tpidrro_el0` is kernel-written on Linux and not settable from EL0. Plan: at load time scan the
`__TEXT` of every mapped image for `mrs xN, tpidrro_el0` (encoding `0xd53bd060 | N`) and patch it to
`mrs xN, tpidr_el0` (`0xd53bd040 | N`); our `libsystem_kernel` sets `tpidr_el0` via the
`_os_tsd_set_base`-equivalent per thread. Any Darwin use of `tpidr_el0` (cpu number) must be found by
disassembly of the pinned version first and handled (patched to a constant/0 or to a TSD slot).
Patch only verified instruction sites (decode, never blind byte replace in data).

## 8. Shared cache and our libsystem_kernel (M2)

- Extract the cache from the user's IPSW with `ipsw` into `vendor-local/` (never committed).
- Map all cache subcaches at their unslid addresses (slide 0 first; ASLR later), apply slide info,
  re-sign auth pointers (§6), patch TLS (§7).
- **Swap:** build `libsystem_kernel/` as an arm64e dylib, load it like any image (outside the cache,
  in our arena). Then overwrite the entry of every export of the cache's original
  `libsystem_kernel.dylib` with a 16-byte trampoline `ldr x16, #8; br x16; .quad <our impl>`.
  This covers direct `bl` from other cache dylibs, GOT entries and dlsym results without rewriting
  callers. Exports we do not implement point at generated stubs that log the symbol name and abort.
  (Requires the original text pages to be made writable briefly: mprotect RW, patch, flush icache,
  mprotect RX.)
- Export list is generated from the pinned cache (`gen/`), stubs generated, implementations added by
  measured frequency of use.

## 9. Build

- Loader: NDK clang `--target=aarch64-linux-android29 -ffreestanding -nostdlib -static -fno-pie
  -fno-stack-protector -fno-builtin -Wl,--image-base=0x7000000000`, ET_EXEC, no libgcc runtime calls
  (provide memcpy/memset ourselves).
- Mach-O tests and our dylib: host clang `-target arm64-apple-ios17.0` or `arm64e-apple-ios17.0`,
  `-ffreestanding -nostdlib -fno-stack-protector`, linked with the NDK's `ld64.lld -arch <a>
  -platform_version ios 17.0 17.0`.
- One top-level `Makefile`; outputs in `build/`. Env `ANDROID_NDK_HOME` selects the NDK.

## 10. Device and tooling

- Real rooted arm64 phone, accessed only through `tools/dev.sh` with `DEV_SERIAL` from
  `tools/device.env` (gitignored). Working dir `/data/local/tmp/ioswine`.

## 11. Milestones

- M0 environment ✔ 2026-11-09. Test device: Xiaomi 12 Pro (SD 8 Gen 1, kernel 5.10 GKI,
  4K pages, 39-bit VA, paca+pacg+bti, KernelSU root, SELinux Enforcing; M1 needs no setenforce).
  ioswine only touches /data/local/tmp/ioswine, nothing system-level.
- M1 ✔ 2026-11-09: `ioswine-loader ./hello` prints hello/argc/apple0 via the seccomp trap,
  `./fixups` passes (8 DYLD_CHAINED_PTR_64 rebases).
- M2 ✔ 2026-11-09 (same day): shared cache + our libsystem_kernel + PAC + TLS patch + dyld-light (objc,
  Swift, dlopen); write/printf/malloc/pthread tests pass on the device as uid shell.
  Known debt: 619 images loaded (libobjc's downward closure; trim), eager 750 MB rebase (make lazy),
  dyld slots still stubbed: swift prespecialized data, version tokens, objc class count.
- M3 visual ✔ 2026-11-10: CoreGraphics + ImageIO render a 1024x1024 PNG on the device.
- M3a ✔ 2026-11-10: in-process Mach ports, mach_msg (descriptors, OOL, port sets, trailers), mk_timer, no-senders and
  send-once notifications; mach_test 12/12, cg_hello still passes with real IPC. Debt: a receive on
  a reply port with no outstanding send/send-once rights returns MACH_RCV_PORT_DIED instead of blocking (deadlock breaker
  until launchd shim exists; not XNU); kernel notifications are delivered with msgh_local_port NULL; bootstrap 0x203 is a
  placeholder without receiver.
- M3 runtime (in progress): (b) workqueue threads +
  kevent (timers, EVFILT_MACHPORT, fds via epoll, EVFILT_USER) so libdispatch global/main queues and CFRunLoop run,
  then (c) machd + launchd shim (§14).

## 12. M2 plan (pinned: iOS 26.6.2 23G90, iPhone17,3)

Facts (measured 2026-11-09): cache base 0x180000000, region 5 GB up to 0x2C2544000, 83 subcaches,
slide info v5, 4315 images. In executable mappings: 10640 `mrs Xn, tpidrro_el0`, 38 `mrs Xn, tpidr_el0`,
0 `msr tpidr_el0`, 693 `svc #0x80`, 15 `mrs cntvct_el0`. libsystem_kernel `__TEXT` at 0x232d9b000
(0x3bb20 bytes), ~1600 symbols.

Device layout: cache files pushed to `/data/local/tmp/ioswine/dsc/` (≈6 GB, only on the device's /data).
The loader finds them via env `IOSWINE_DSC=<path to main cache file>`.

Steps (each has a device test):
- **M2a cache mapping** (loader/dsc.c): map every subcache mapping at its unslid address
  (`MAP_PRIVATE` file-backed, `MAP_FIXED_NOREPLACE`), process slide info v5 on the data mappings:
  plain pointers become `value_add + runtimeOffset`, auth pointers are signed with IA or DA
  (`keyIsData`) using the live keys (§6). Test: loader option `--dsc-selftest` looks up an exported
  pure function (`_platform_strlen` in libsystem_platform) through the export trie and calls it;
  authenticates a few signed pointers with `autia/autda` without faulting.
- **M2b our libsystem_kernel** (gen/ + libsystem_kernel/): our arm64e
  dylib is loaded as a normal image at fixed `0x300000000` (above the cache), then every export entry of the
  cache's libsystem_kernel is overwritten with a trampoline to ours: `b <imm26>` if within ±128 MiB,
  else `ldr x16, #8; br x16; .quad target` (needs 16 bytes until the next symbol; the generator reports gaps).
  Our dylib text range is added to the seccomp allow list. Test: call cache `_write` → our impl prints.
- **M2c TLS + init**: TLS patch (offline-precomputed site list from `__text` sections only, applied at load);
  per-thread TSD set up by our libsystem_kernel in `tpidr_el0`; commpage mapped and filled; run
  libSystem.B's initializers (`libSystem_initializer(argc, argv, envp, apple, &ProgramVars)`); provide the
  minimal dyld side (libdyld's `gDyld` APIs object) that libSystem needs.
  Main executable binds resolved against cache export tries (with re-exports).
  Test: arm64 hello linked against libSystem (`.tbd` stub) prints via `printf`.
- **M2d** pthread_create/join + malloc/free test.

Open questions answered during bring-up: users of `tpidr_el0` reads, what follows
`tpidrro_el0` reads, svc sites outside libsystem_kernel, libdyld → dyld interface, libSystem init order,
commpage fields read.

## 13. Decisions from M2 bring-up (2026-11-09)

- **PAC mode per process (as XNU):** arm64 (non-e) executables, i.e. normal App Store apps, run with all user
  PAC keys disabled via `prctl(PR_PAC_SET_ENABLED_KEYS, 0xf, 0)` (verified on kernel 5.10): PAC instructions
  become NOPs, so unsigned callbacks from app code work in arm64e libraries. arm64e executables keep the keys
  and the loader signs every auth pointer. Decided before the cache is rebased.
- **Device tests run as uid `shell`, never root.** SELinux (`execmod`) forbids making a written file
  mapping executable for `shell`, so non-writable segments are mapped with final protection directly, and
  patched cache code (TLS sites, lsk trampolines) is replaced by anonymous copies (`execmem` is allowed;
  verified) via `text_make_writable()` / `text_seal()`.
- **tpidr_el0 readers (38 sites, cpu number on Darwin)** are patched to `movz xN, #0`; tpidrro readers
  (10640) to `mrs xN, tpidr_el0`. Offline list `build/dsc/tls_sites.bin` with cache UUID check.
- **Temporary TSD:** the loader sets `tpidr_el0` to a zeroed page before handoff; libpthread installs the
  real one via `__thread_set_tsd_base` (ours) or machdep svc (x16=0x80000000, x3=2, handled by SIGSYS).
- **libSystem mode boot** (`IOSWINE_DSC` set): loader/dyld.c. Binds of the exe are resolved eagerly against
  cache export tries (chained imports and classic/lazy opcodes; no dyld_stub_binder).
- ld64.lld arm64e output uses classic lazy binding (`dyld_stub_binder` must be in the .tbd), resolved eagerly.

## 14. Mach IPC design (M3)

Goal: real Mach port semantics for iOS processes on Linux, enough for libxpc/launchd bootstrap, cfprefsd, notifyd,
and later backboardd/FrontBoard-style UI services that we implement ourselves.

**Kernel object: `ioswine-machd`** (one per device user session, like wineserver): a native Linux daemon (static,
freestanding-friendly C, may use bionic since it runs no Apple code) that owns all port state.
- Ports live in machd: `{id, receiver task, queue, seqno, qlimit, kobject type, send-right refcount, send-once count,
  dead-name notifications, no-senders, port-destroyed}`.
- Each task has a name space `{name -> (port id, rights: RECEIVE|SEND(urefs)|SEND_ONCE|PORT_SET|DEAD_NAME)}` kept in
  machd; names are generated Darwin-style (index << 8 | generation).
- Transport task <-> machd: one `AF_UNIX SOCK_SEQPACKET` connection per task (fd inherited/opened at boot by the
  loader, number fixed, e.g. fd 1000, hidden from the app by our lsk `close`/`dup2` guards).
- `mach_msg2_trap`/`mach_msg_trap` (our lsk) send `{header, body, inline data}` to machd; port rights in the message
  are translated by machd (MOVE/COPY/MAKE_SEND/MAKE_SEND_ONCE dispositions). Out-of-line memory: memfd + `SCM_RIGHTS`
  fd passing, receiver maps it (copy semantics; `vm_deallocate` on the sender side when `deallocate` is set).
  File descriptors (fileport / xpc fd objects): `SCM_RIGHTS` too.
- Receive: the task blocks in `recvmsg` on its socket for messages addressed to a port it holds RECEIVE for (machd
  pushes per-port; the lsk keeps a small local demux by port name, timeouts via `ppoll`).
- Port sets and `mach_port_request_notification` handled in machd.
- Kernel objects (task, thread, host, semaphore, clock, voucher) are machd-side kobjects with MIG servers in machd:
  `task_info`, `thread_info`, `host_info`, `mach_port_*`, `semaphore_*`, `task_get_special_port`,
  `task_set_special_port`, `bootstrap_port`.
- **launchd shim** inside machd (or a separate `ioswine-launchd`): bootstrap namespace (`bootstrap_look_up`,
  `bootstrap_check_in` via the XPC pipe protocol that libxpc speaks to launchd: `xpc_pipe_routine` on the bootstrap
  port with xpc dictionaries), service table from a plist dir under `/data/local/tmp/ioswine/services/`.
- System daemons we need early: `cfprefsd` (prefs: we can run Apple's own cfprefsd binary from the IPSW under our
  loader once IPC works, or write a small compatible server), `notifyd` (notify_* : own implementation),
  `logd`/os_log (sink to logcat/stderr), `securityd`/keychain (own minimal implementation later).
- Single-process shortcut for bring-up: until machd exists, lsk keeps an in-process port table so that ports a task
  sends to itself work (needed by libdispatch mach channels, CFRunLoop wakeup ports); cross-process sends fail with
  MACH_SEND_INVALID_DEST.

Order: (1) in-process port table + mach_msg loopback in lsk, (2) machd with task registration and kobject MIG,
(3) launchd shim + XPC pipe, (4) cfprefsd/notifyd, (5) multi-process (app + our UI server).
