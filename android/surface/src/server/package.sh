#!/bin/bash
# ioswine-surface packaging: gradle-free APK build + test client binary.
#
# Prerequisites (see BUILD.md):
#   ANDROID_NDK_HOME = ~/workspace/tools/android-ndk-r28c
#   ANDROID_SDK_ROOT with build-tools 34.0.0+ (aapt2, zipalign, apksigner)
#   A debug keystore is generated on first run (debug.keystore, "android").
#
# Usage:
#   ./package.sh            # builds APK + client binary into build/
#   ./package.sh install    # also installs the APK on the device ($DEV_SERIAL)
#   ./package.sh client     # only the client binary
set -euo pipefail
# package.sh lives in src/server/; the tree root is two levels up.
cd "$(dirname "$0")/../.."

# Locate NDK: env -> ~/workspace/tools/android-ndk-r28c -> ~/Android/ndk/*
if [ -n "${ANDROID_NDK_HOME:-}" ] && [ -d "$ANDROID_NDK_HOME" ]; then
    NDK="$ANDROID_NDK_HOME"
elif [ -d "$HOME/workspace/tools/android-ndk-r28c" ]; then
    NDK="$HOME/workspace/tools/android-ndk-r28c"
elif [ -d "$HOME/workspace/tools/android-ndk-r28c" ]; then
    NDK="$HOME/workspace/tools/android-ndk-r28c"
else
    NDK="$(ls -d $HOME/Android/ndk/* "$HOME"/Android/ndk/* 2>/dev/null | sort -V | tail -n 1 || true)"
fi
[ -n "${NDK:-}" ] && [ -d "$NDK" ] || { echo "error: Android NDK not found (checked ANDROID_NDK_HOME, ~/workspace/tools/android-ndk-r28c, ~/Android/ndk)" >&2; exit 1; }
# CMakeLists.txt reads $ENV{ANDROID_NDK_HOME} for native_app_glue, so
# export it even when the caller only set the default above.
export ANDROID_NDK_HOME="$NDK"

OUT="${SURFACE_OUT_DIR:-build}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
KEYSTORE="${SURFACE_KEYSTORE:-$OUT/debug.keystore}"

find_sdk_tools() {
    local candidates=()
    [ -d "$HOME/workspace/tools/android-sdk" ] && candidates+=("$HOME/workspace/tools/android-sdk")
    [ -d "$HOME/workspace/tools/android-sdk" ] && [ "$HOME/workspace/tools/android-sdk" != "$HOME/workspace/tools/android-sdk" ] && candidates+=("$HOME/workspace/tools/android-sdk")
    [ -n "${ANDROID_SDK_ROOT:-}" ] && [ -d "$ANDROID_SDK_ROOT" ] && candidates+=("$ANDROID_SDK_ROOT")
    [ -d "$HOME/Android/Sdk" ] && candidates+=("$HOME/Android/Sdk")
    [ -d "$HOME/Android/Sdk" ] && [ "$HOME/Android/Sdk" != "$HOME/Android/Sdk" ] && candidates+=("$HOME/Android/Sdk")

    SDK=""
    BT=""
    ANDROID_JAR=""
    for cand in "${candidates[@]+"${candidates[@]}"}"; do
        local bt="" jar=""
        if [ -d "$cand/build-tools/34.0.0" ]; then
            bt="$cand/build-tools/34.0.0"
        else
            bt="$(ls -d "$cand"/build-tools/*/ 2>/dev/null | sort -V | tail -1 || true)"
            bt="${bt%/}"
        fi
        if [ -f "$cand/platforms/android-34/android.jar" ]; then
            jar="$cand/platforms/android-34/android.jar"
        else
            jar="$(ls "$cand"/platforms/android-*/android.jar 2>/dev/null | sort -V | tail -1 || true)"
        fi
        if [ -n "$bt" ] && [ -n "$jar" ]; then
            SDK="$cand"; BT="$bt"; ANDROID_JAR="$jar"; break
        fi
        [ -z "$SDK" ] && SDK="$cand"
        [ -z "$BT" ] && [ -n "$bt" ] && BT="$bt"
        [ -z "$ANDROID_JAR" ] && [ -n "$jar" ] && ANDROID_JAR="$jar"
    done

    local missing=()
    if [ -z "$BT" ] || [ ! -x "$BT/aapt2" ]; then missing+=("build-tools/<ver>/aapt2"); fi
    if [ -z "$BT" ] || [ ! -x "$BT/zipalign" ]; then missing+=("build-tools/<ver>/zipalign"); fi
    if [ -z "$BT" ] || [ ! -x "$BT/apksigner" ]; then missing+=("build-tools/<ver>/apksigner"); fi
    if [ -z "$ANDROID_JAR" ] || [ ! -f "$ANDROID_JAR" ]; then missing+=("platforms/android-34/android.jar"); fi

    if [ ${#missing[@]} -gt 0 ]; then
        echo "error: Android SDK components missing for gradle-free APK build:" >&2
        for m in "${missing[@]}"; do echo "  - missing: $m" >&2; done
        echo "  searched SDK roots: ${candidates[*]:-(none found)}" >&2
        echo "  install build-tools;34.0.0 and platforms;android-34 under \$ANDROID_SDK_ROOT or ~/workspace/tools/android-sdk" >&2
        exit 1
    fi

    AAPT2="$BT/aapt2"; ZIPALIGN="$BT/zipalign"; APKSIGNER="$BT/apksigner"

    # keytool: prefer PATH, then JAVA_HOME, then the Temurin JRE kept in
    # ~/workspace/tools (a JRE ships keytool; a full JDK is not required).
    if command -v keytool >/dev/null 2>&1; then
        KEYTOOL="keytool"
    elif [ -x "${JAVA_HOME:-}/bin/keytool" ]; then
        KEYTOOL="$JAVA_HOME/bin/keytool"
    else
        KEYTOOL="$(ls -d "$HOME"/workspace/tools/jdk-*/bin/keytool $HOME/workspace/tools/jdk-*/bin/keytool 2>/dev/null | head -1 || true)"
    fi
    [ -n "${KEYTOOL:-}" ] || { echo "error: keytool not found (need a JDK/JRE with keytool)" >&2; exit 1; }
}

build_client() {
    echo "== building test client (arm64, API 34) =="
    # NOTE: no -lpthread here: bionic has pthread in libc and the NDK
    # linker has no libpthread.a; -lpthread is host-only (see BUILD.md).
    "$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" \
        --target=aarch64-linux-android34 \
        -std=c11 -O2 -Wall -Wextra \
        -I src/common \
        src/client/ioswine-client.c src/common/protocol.c \
        -o "$OUT/ioswine-client" -lm
    "$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" "$OUT/ioswine-client" || true
    ls -l "$OUT/ioswine-client"
}

build_apk() {
    find_sdk_tools

    echo "== building server .so (NDK r28) =="
    if command -v cmake >/dev/null 2>&1 && cmake --version >/dev/null 2>&1; then
        cmake -S src/server -B "$OUT/server-cmake" \
            -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
            -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-34 \
            -DCMAKE_BUILD_TYPE=Release
        cmake --build "$OUT/server-cmake" --parallel
    else
        echo "note: cmake unavailable; compiling libioswine-surface.so directly with NDK clang"
        mkdir -p "$OUT/server-cmake"
        "$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" \
            --target=aarch64-linux-android34 \
            -shared -fPIC -std=c11 -O2 -Wall -Wextra -fvisibility=hidden \
            -Wl,-soname,libioswine-surface.so \
            -I src/common \
            -I "$NDK/sources/android/native_app_glue" \
            src/server/server.c \
            src/server/sysinfo.c \
            src/server/ui.c \
            src/common/protocol.c \
            src/common/touch_input.c \
            "$NDK/sources/android/native_app_glue/android_native_app_glue.c" \
            -o "$OUT/server-cmake/libioswine-surface.so" \
            -landroid -llog -lEGL -lGLESv2
    fi

    echo "== packaging APK (aapt2, no gradle) =="
    rm -rf "$OUT/apk"
    mkdir -p "$OUT/apk/lib/arm64-v8a"
    cp "$OUT/server-cmake/libioswine-surface.so" "$OUT/apk/lib/arm64-v8a/"
    # No resources of our own: link the manifest directly.
    "$AAPT2" link -o "$OUT/ioswine-surface.unaligned.apk" \
        -I "$ANDROID_JAR" \
        --manifest src/server/AndroidManifest.xml \
        --min-sdk-version 34 --target-sdk-version 34 \
        --version-code 1 --version-name 1.0
    # Add the native library (uncompressed, page-aligned for the loader).
    (cd "$OUT/apk" && zip -0 -r "$OUT/ioswine-surface.unaligned.apk" lib/ >/dev/null)
    "$ZIPALIGN" -f -p 4 "$OUT/ioswine-surface.unaligned.apk" "$OUT/ioswine-surface.aligned.apk"

    if [ ! -f "$KEYSTORE" ]; then
        echo "== generating debug keystore =="
        "$KEYTOOL" -genkeypair -keystore "$KEYSTORE" -storepass android \
            -keypass android -alias ioswine-debug -keyalg RSA -keysize 2048 \
            -validity 10000 -dname "CN=ioswine debug, OU=ioswine, O=ioswine, C=HU"
    fi
    "$APKSIGNER" sign --ks "$KEYSTORE" --ks-pass pass:android \
        --key-pass pass:android --ks-key-alias ioswine-debug \
        --out "$OUT/ioswine-surface.apk" "$OUT/ioswine-surface.aligned.apk"
    "$APKSIGNER" verify --verbose "$OUT/ioswine-surface.apk"
    ls -l "$OUT/ioswine-surface.apk"
}

case "${1:-all}" in
    client) build_client ;;
    apk)    build_apk ;;
    all)    build_client; build_apk ;;
    install)
        build_client; build_apk
        adb ${DEV_SERIAL:+-s "$DEV_SERIAL"} install -r "$OUT/ioswine-surface.apk"
        adb ${DEV_SERIAL:+-s "$DEV_SERIAL"} push "$OUT/ioswine-client" /data/local/tmp/ioswine-client
        adb ${DEV_SERIAL:+-s "$DEV_SERIAL"} shell chmod 755 /data/local/tmp/ioswine-client
        echo "installed. Launch the app, then run:"
        echo "  adb shell /data/local/tmp/ioswine-client"
        ;;
    *) echo "usage: $0 [all|apk|client|install]"; exit 2 ;;
esac
