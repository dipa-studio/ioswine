# ioswine-surface: build & run

## Prerequisites

| Tool | Expected location / version |
|---|---|
| Android NDK | r28c, at `~/workspace/tools/android-ndk-r28c` (set `ANDROID_NDK_HOME` to override). **Do not download it yourself if it is still downloading: the build commands below assume it is there.** |
| Android SDK build-tools | 34.0.0 or newer, at `$ANDROID_SDK_ROOT/build-tools/<ver>/` (`aapt2`, `zipalign`, `apksigner`). `platforms/android-34/android.jar` is also needed. |
| JDK | any JDK with `keytool` (17+ is fine) for the debug keystore |
| Host compiler | `cc` (gcc/clang) for the unit test and the loopback client |
| adb | with the phone reachable; use `DEV_SERIAL` if several devices are attached |

### If the SDK / build-tools are missing

The NDK alone is not enough for the gradle-free APK step. Install the
command-line tools (no Android Studio needed):

```sh
# Linux x86_64, adjust the URL to the current revision if it 404s:
cd ~/workspace/tools
curl -O https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip
unzip -q commandlinetools-linux-11076708_latest.zip -d android-sdk/cmdline-tools
mv android-sdk/cmdline-tools/cmdline-tools android-sdk/cmdline-tools/latest
export ANDROID_SDK_ROOT=$HOME/workspace/tools/android-sdk
export PATH=$ANDROID_SDK_ROOT/cmdline-tools/latest/bin:$PATH
yes | sdkmanager --licenses
sdkmanager "platform-tools" "platforms;android-34" "build-tools;34.0.0"
```

Verify: `aapt2 version`, `apksigner --version`, `adb --version`.

## Host unit test (no NDK needed)

```sh
cd ~/workspace/ioswine/surface
cc -std=c11 -Wall -Wextra -I src/common \
   tests/host/test_protocol.c src/common/protocol.c \
   -o /tmp/test_protocol -lm
/tmp/test_protocol
# expected: "test_protocol: 124 checks, 0 failures"
```

## Host loopback client (no device, no NDK)

Exercises the whole client: HELLO + memfd via SCM_RIGHTS, PRESENT
pacing, FRAME_DONE latency, synthetic touches, INFO mapping: against
an in-process fake server over a socketpair.

```sh
cd ~/workspace/ioswine/surface
cc -std=c11 -Wall -Wextra -I src/common -DIOSWINE_HOST_TEST \
   src/client/ioswine-client.c src/common/protocol.c \
   -o /tmp/ioswine-client-host -lm -lpthread
IOSWINE_LOOPBACK=1 /tmp/ioswine-client-host --width 640 --height 480 --fps 30 --seconds 3
# expected: "ready: server protocol v1", "info: screen 1080x2400 ...",
# synthetic "touch:" lines, "fps=~30 ..." lines, "client exiting after ~90 frames"
```

Note: at 1080x2400 the loopback client only reaches ~18 fps on this
VM: that is the test pattern's own per-pixel cost (a `sin`/`fmod`
per pixel over 2.6 M pixels), not a protocol limit; the
PRESENT→FRAME_DONE latency stays ~0.1 ms in loopback.

## Building the APK + client binary (needs NDK r28)

One script does everything (from the `surface/` directory):

```sh
cd ~/workspace/ioswine/surface
./src/server/package.sh            # builds build/ioswine-surface.apk + build/ioswine-client
./src/server/package.sh apk        # APK only
./src/server/package.sh client     # client binary only
./src/server/package.sh install    # build + install APK + push client
```

What `package.sh` runs, step by step (so you can run it by hand):

```sh
export ANDROID_NDK_HOME=$HOME/workspace/tools/android-ndk-r28c
export ANDROID_SDK_ROOT=$HOME/Android/Sdk   # or wherever build-tools live

# 1. Server .so (CMake + NDK clang, arm64-v8a, API 34)
cmake -S src/server -B build/server-cmake \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-34 \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/server-cmake --parallel
# -> build/server-cmake/libioswine-surface.so

# 2. APK packaging, gradle-free (aapt2 link + zip + zipalign + apksigner)
mkdir -p build/apk/lib/arm64-v8a
cp build/server-cmake/libioswine-surface.so build/apk/lib/arm64-v8a/
$ANDROID_SDK_ROOT/build-tools/34.0.0/aapt2 link -o build/ioswine-surface.unaligned.apk \
  -I $ANDROID_SDK_ROOT/platforms/android-34/android.jar \
  --manifest src/server/AndroidManifest.xml \
  --min-sdk-version 34 --target-sdk-version 34 \
  --version-code 1 --version-name 1.0
(cd build/apk && zip -0 -r ../ioswine-surface.unaligned.apk lib/)
$ANDROID_SDK_ROOT/build-tools/34.0.0/zipalign -f -p 4 \
  build/ioswine-surface.unaligned.apk build/ioswine-surface.apk

# 3. Debug keystore (first run only) + sign
keytool -genkeypair -keystore debug.keystore -storepass android \
  -keypass android -alias ioswine-debug -keyalg RSA -keysize 2048 \
  -validity 10000 -dname "CN=ioswine debug, OU=ioswine, O=ioswine, C=HU"
$ANDROID_SDK_ROOT/build-tools/34.0.0/apksigner sign --ks debug.keystore \
  --ks-pass pass:android --key-pass pass:android --ks-key-alias ioswine-debug \
  --out build/ioswine-surface.apk build/ioswine-surface.apk
$ANDROID_SDK_ROOT/build-tools/34.0.0/apksigner verify --verbose build/ioswine-surface.apk

# 4. Test client binary (NDK clang, plain C, no Java)
$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/clang \
  --target=aarch64-linux-android34 -std=c11 -O2 -Wall -Wextra \
  -I src/common src/client/ioswine-client.c src/common/protocol.c \
  -o build/ioswine-client -lm
```
No `-lpthread` on the Android build: bionic carries pthread in libc and
the NDK linker has no `libpthread.a` (adding it fails the link).
`-lpthread` is only for the host builds above, where it is correct.
`package.sh` also exports `ANDROID_NDK_HOME` before running CMake,
because `src/server/CMakeLists.txt` reads `$ENV{ANDROID_NDK_HOME}`
for the `native_app_glue` source: without the export the .so build
fails to find it.

### Experimental EGL path

Off by default. To try it (see the warning in `src/server/server_egl.c`
: it is written but not compiled or tested on device):

```sh
cmake -S src/server -B build/server-cmake -DIOSWINE_USE_EGL=ON ...
```

Note: enabling the flag only compiles `server_egl.c` into the .so;
the call sites in `server.c` are intentionally not wired yet: see the
header comment in `server_egl.c` for what to hook up.

## Install & run on the phone

```sh
adb install -r build/ioswine-surface.apk
adb push build/ioswine-client /data/local/tmp/ioswine-client
adb shell chmod 755 /data/local/tmp/ioswine-client
```

Launch the app (tap its icon, or `adb shell monkey -p org.ioswine.surface -c android.intent.category.LAUNCHER 1`),
then in a second shell:

```sh
adb shell /data/local/tmp/ioswine-client --width 1080 --height 2400 --fps 60
```

You should see the animated colour bars fullscreen, a dot following
your finger, and once a second a line like:

```
fps=59.8 frames=1234 acked=1233 latency_ms avg=2.31 min=0.42 max=8.10 (n=1233)
```

Stop with Ctrl-C in the adb shell, or press the Android back button
(the client exits on back-key up).

Client options:

```
--width W --height H    client frame size (default 1080x2400)
--fps F                 target frame rate (default 60, max 240)
--frames N              exit after N frames (default: run forever)
--seconds N             exit after N seconds
```

## Reading logcat (scoped to the app PID only)

Do not dump the whole logcat. Get the app's PID and filter to it:

```sh
PID=$(adb shell pidof org.ioswine.surface)
adb logcat --pid=$PID
# or, if --pid is unavailable on the host adb:
adb logcat -s ioswine-surface
```

What to look for:

```
ioswine-surface: listening on abstract socket @ioswine-surface
ioswine-surface: window 1080x2400, format RGBA_8888, CPU swizzle BGRA->RGBA active
ioswine-surface: client connected: 1080x2400 stride=4320 memfd=20736000 bytes
ioswine-surface: stats: presented=1234 dropped=0 copied=123456789 bytes
```

Which presentation path is active: the `window ... CPU swizzle
BGRA->RGBA active` line means the primary `ANativeWindow_lock` path.
There is no other path in the default build: if you enabled the EGL
path you will see `egl: initialised ...` instead (and only after
wiring the call sites, see above).

If the client prints `connect @ioswine-surface failed`, the presenter
app is not running (or was killed). If the server logs
`client connected` but no frames appear, check `presented=` in the
stats line and that the client is not stuck waiting for READY.

## Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| `aapt2: command not found` | build-tools not on PATH; set `ANDROID_SDK_ROOT` (see §1) |
| `android.jar` missing | `sdkmanager "platforms;android-34"` |
| APK installs but crashes on launch | check `adb logcat --pid=$(adb shell pidof org.ioswine.surface)` for a `dlopen` failure: usually a missing `lib_name` meta-data or an ABI mismatch (must be `arm64-v8a`) |
| Client: `connect failed` | presenter app not in the foreground; Android may have killed it: relaunch it |
| Client: `no READY from server` | server rejected the HELLO (see logcat for the reason) or the socket thread died |
| Frames present but touch does nothing | touch coordinates are mapped with the latest INFO; check the client printed an `info:` line |
| `dropped=` climbing in server stats | client is presenting faster than the server acks (ignoring max-in-flight): the client paces on acks, so this should stay at 0 with the test client |
