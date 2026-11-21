# ioswine-surface

Android-side presenter for **ioswine**: shows frames rendered in-process
by the iOS side (CPU, BGRA) fullscreen on the phone, and sends touches
back. No Apple code is involved; this is a plain C presenter plus a
test client.

## Layout

```
surface/
  PROTOCOL.md            exact wire format (read this first if you implement a client)
  BUILD.md               exact build / install / run / logcat commands
  README.md              this file
  src/common/            protocol.h / protocol.c: shared, host- and Android-safe
  src/server/            NativeActivity server (no Java of our own)
    server.c             socket thread, ANativeWindow presentation, input, vsync
    server_egl.c         EXPERIMENTAL, compile-time off EGL texture-upload path
    AndroidManifest.xml  package org.ioswine.surface, NativeActivity
    CMakeLists.txt       NDK r28, API 34, arm64-v8a
    package.sh           gradle-free APK packaging + client build
  src/client/            ioswine-client.c: plain C test client (adb shell binary)
  tests/host/            test_protocol.c: host unit test, plain `cc`
```

## How the iOS side will use it

1. Launch the presenter app (tap the icon, or `am start -n
   org.ioswine.surface/android.app.NativeActivity`).
2. Connect to the abstract unix socket `@ioswine-surface`
   (`AF_UNIX`, `SOCK_STREAM`).
3. Create one memfd of `stride * height * 2` bytes (two BGRA8888
   buffers), and send `HELLO` (version 1, width, height, stride,
   format 1 = BGRA8888 premultiplied) with the memfd attached via
   `SCM_RIGHTS` in the same `sendmsg`.
4. Wait for `READY` (then `INFO` with the screen/client mapping).
5. For each frame: write pixels into buffer `i`, send
   `PRESENT(frame_id, i, damage_rects)`. Wait for `FRAME_DONE` for
   that buffer before reusing it (max 2 frames in flight).
6. Read `TOUCH` (client-pixel coordinates, multi-touch, up to 10
   pointers), `KEY` (back), `INFO` (resize/rotation), `FOCUS`
   asynchronously on the same socket.
7. Send `GOODBYE` (or just close) on exit. Reconnecting with a fresh
   HELLO works; the server survives disconnects.

Full field offsets, state machine, limits and error codes:
[`PROTOCOL.md`](PROTOCOL.md). The shared header
[`src/common/protocol.h`](src/common/protocol.h) has the encode/decode
helpers: the iOS-side client should use the same code rather than
re-implementing the wire format.

## Design decisions (sane defaults)

- **Protocol v1, BGRA8888 premultiplied only**, little-endian, all
  lengths explicit. No version negotiation in v1 (see PROTOCOL.md §8).
- **Presentation: `ANativeWindow_lock` + CPU copy** (damage rects
  only), window format `RGBA_8888`, BGRA→RGBA swizzle per pixel,
  aspect-fit letterbox (black bars), nearest-neighbour scale. There
  is no public BGRA window format, so the swizzle is unavoidable on
  this path; logcat tells you which path is active (see BUILD.md).
- **Zero-copy: not claimed.** There is no public NDK API to wrap an
  existing memfd in an `AHardwareBuffer`, so a true zero-copy path
  from the client memfd does not exist with public APIs. The EGL
  texture-upload path in `src/server/server_egl.c` still copies via
  `glTexSubImage2D` (but moves the swizzle to the GPU); it is
  compile-time off, unwired, and **not tested**: see its header
  comment before enabling it.
- **Vsync:** server acks `FRAME_DONE` with the latest
  `AChoreographer` vsync timestamp; the client paces on acks (max 2
  in flight). A PRESENT for a buffer still in flight is **dropped**
  and acked with `status=1`: the server never blocks.
- **Scaling:** aspect-fit letterbox; the mapping is sent in `INFO`
  and unit-tested (screen↔client round-trip, letterbox clamping).
- **Multi-touch:** up to 10 pointers, stable ids, one `TOUCH`
  message per pointer per event, coordinates in client pixels
  (float).
- **Robustness:** server survives disconnect/reconnect, validates
  every length before reading, rejects malformed HELLO/PRESENT, and
  sends `ERROR` + closes on protocol errors. One client at a time.
- **Logging:** tag `ioswine-surface`, stats once a second, no
  per-frame spam.

## How to test (no iOS side needed)

```sh
# host only: unit test + loopback client (fake server, synthetic touches)
cc -std=c11 -Wall -Wextra -I src/common tests/host/test_protocol.c \
   src/common/protocol.c -o /tmp/test_protocol -lm && /tmp/test_protocol
cc -std=c11 -Wall -Wextra -I src/common -DIOSWINE_HOST_TEST \
   src/client/ioswine-client.c src/common/protocol.c \
   -o /tmp/ioswine-client-host -lm -lpthread
IOSWINE_LOOPBACK=1 /tmp/ioswine-client-host --width 640 --height 480 --fps 30 --seconds 3

# on the test phone (needs NDK r28 + SDK build-tools 34; see BUILD.md)
./src/server/package.sh install
adb shell /data/local/tmp/ioswine-client --width 1080 --height 2400 --fps 60
```

## Known limits

- One client at a time; a second connect is rejected with ERROR.
- No authentication on the socket (any local process can connect).
  Fine for a rooted dev phone; add an `SO_PEERCRED` uid check before
  wider use.
- `rotation` in INFO is 0 (portrait) or 90 (landscape) only :
  `AConfiguration` does not expose exact degrees. Touch mapping does
  not depend on it (it uses the window size + scale + offsets).
- The test client's animated pattern is deliberately heavy (a
  `sin`/`fmod` per pixel): at 1080×2400 it only reaches ~18 fps on
  this VM's shared CPU. That is the pattern's cost, not the
  protocol's: the loopback PRESENT→FRAME_DONE latency stays ~0.1 ms.
  The real iOS renderer should use damage rects for partial updates.
- The EGL path is experimental, unwired and untested on device.

## Verification (built 2026-11-11 on x86_64 Linux, NDK r28c + SDK build-tools 34.0.0)

- Host unit test: **124 checks, 0 failures** (`/tmp/test_protocol`).
- Host loopback client: end-to-end pass: READY, INFO mapping
  (640×480 on 1080×2400 → scale 1.6875, offset_y 795), synthetic
  touches move the dot, ~30 fps at target 30, PRESENT→FRAME_DONE
  latency ~0.1 ms.
- Built binaries in `build/`: `ioswine-surface.apk` (176,579 bytes,
  sha256 `cfb92ab2…e3162b3b`) and `ioswine-client` (arm64 PIE,
  sha256 `c9acf480…b2bd514f`). APK verified independently:
  `apksigner verify` → Verifies (v3 scheme, 1 signer, debug cert
  `CN=ioswine debug`), `zipalign -c` OK, `aapt2 dump badging` shows
  `org.ioswine.surface`, min/target SDK 34, launchable NativeActivity,
  `arm64-v8a`; `ANativeActivity_onCreate` exported; the `.so` is
  STORED (page-aligned) in the APK. Server/client compile with zero
  warnings from our own code (the only warnings are 6 unused-parameter
  notes inside the NDK's own `native_app_glue.c`).
- Fixes applied during this build (all in-tree, see git-less diff
  notes in BUILD.md): `frame_id` uninitialised on the no-READY exit
  path (client); `-lpthread` removed from the Android client link
  (bionic has no libpthread); `package.sh` cd'ed to `src/` instead
  of the tree root; CMake `../../common` → `../common`;
  `ALooper_pollAll` (unavailable in the NDK) → `ALooper_pollOnce`;
  deprecated `AChoreographer_postFrameCallback` →
  `postFrameCallback64` (the callback already had the 64-bit
  signature). `package.sh` now also exports `ANDROID_NDK_HOME`
  (CMakeLists reads it for `native_app_glue`) and finds `keytool`
  in the Temurin JRE.
- **Not tested: on-device.** No test phone is reachable via adb from this
  machine, so the APK has never launched, no frame has been shown,
  and no real touch has been mapped. First run on the test phone per BUILD.md
  is still the real acceptance test.
