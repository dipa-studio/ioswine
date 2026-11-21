#!/usr/bin/env bash
# Build ioswine + presenter APK and deploy to /data/local/tmp/ioswine on the device.
# All device interaction goes through tools/dev.sh (never bare adb).
set -euo pipefail
cd "$(dirname "$0")/.."

mkdir -p build/tmp

echo "== building loader, dylib, tls-sites, surface-apk =="
make -j16
make tls-sites
make surface-apk

for f in build/ioswine-loader build/libsystem_kernel.dylib build/dsc/tls_sites.bin build/surface/ioswine-surface.apk; do
    if [ ! -f "$f" ]; then
        echo "error: expected build artifact missing: $f" >&2
        exit 1
    fi
done

GIT_DESC=$(git describe --always --dirty --tags 2>/dev/null || git rev-parse --short HEAD 2>/dev/null || echo unknown)
BUILD_DATE=$(date -Is)
SHA_LOADER=$(sha256sum build/ioswine-loader | cut -d' ' -f1)
SHA_DYLIB=$(sha256sum build/libsystem_kernel.dylib | cut -d' ' -f1)
SHA_TLS=$(sha256sum build/dsc/tls_sites.bin | cut -d' ' -f1)
SHA_APK=$(sha256sum build/surface/ioswine-surface.apk | cut -d' ' -f1)

cat > build/tmp/release.txt <<EOF
version: $GIT_DESC
date: $BUILD_DATE
ioswine-loader: $SHA_LOADER
libsystem_kernel.dylib: $SHA_DYLIB
tls_sites.bin: $SHA_TLS
ioswine-surface.apk: $SHA_APK
EOF

cat > build/tmp/launch.sh <<'EOF'
#!/system/bin/sh
DIR=/data/local/tmp/ioswine
LOG="$DIR/last-run.log"
cd "$DIR" || exit 1
mkdir -p "$DIR/home/aidoku/tmp"

restore_selinux() {
    # A relaunch kills this run's loader; never flip back to Enforcing
    # while a newer run's loader is alive (presenter then cannot map the
    # memfd and the app reconnects forever).
    sleep 1
    pidof ioswine-loader >/dev/null 2>&1 && return
    setenforce 1 2>/dev/null || true
}
trap restore_selinux EXIT INT TERM HUP QUIT

# Ensure no stale loader holds @ioswine-surface or fights over files.
PIDS=$(pidof ioswine-loader 2>/dev/null || true)
if [ -n "$PIDS" ]; then
    kill -TERM $PIDS 2>/dev/null || true
    sleep 0.3
    PIDS2=$(pidof ioswine-loader 2>/dev/null || true)
    [ -n "$PIDS2" ] && kill -KILL $PIDS2 2>/dev/null || true
    sleep 1.5  # let the old run's trap finish before we go Permissive
fi

setenforce 0
START_TS=$(date +%s 2>/dev/null || echo 0)
printf '=== ioswine launch start=%s ===\n' "$START_TS" > "$LOG"

env \
    IOSWINE_RENDERS_LOCALLY=1 \
    HOME=/data/local/tmp/ioswine/home/aidoku \
    CFFIXED_USER_HOME=/data/local/tmp/ioswine/home/aidoku \
    TMPDIR=/data/local/tmp/ioswine/home/aidoku/tmp/ \
    IOSWINE_TOUCH=1 \
    IOSWINE_TOUCH_TRACE=1 \
    IOSWINE_TOUCH_DIRECT=1 \
    IOSWINE_TOUCH_TAP_HACK=1 \
    PTHREAD_MUTEX_USE_ULOCK=1 \
    IOSWINE_IOKIT=1 \
    IOSWINE_SNAPSHOT=1 \
    IOSWINE_SNAPSHOT_MS=50 \
    IOSWINE_SNAPSHOT_SCALE=2 \
    IOSWINE_NO_ANIM=1 \
    IOSWINE_SNAPSHOT_PNG=/data/local/tmp/ioswine/snap.png \
    IOSWINE_FAKEBOARD=1 \
    IOSWINE_DSC=/data/local/tmp/ioswine/dsc/dyld_shared_cache_arm64e \
    ./ioswine-loader Aidoku.app/Aidoku >> "$LOG" 2>&1
RC=$?

END_TS=$(date +%s 2>/dev/null || echo 0)
DUR=0
if [ "$START_TS" -gt 0 ] 2>/dev/null && [ "$END_TS" -ge "$START_TS" ] 2>/dev/null; then
    DUR=$((END_TS - START_TS))
fi
printf '=== ioswine launch exit=%d duration=%ds ===\n' "$RC" "$DUR" >> "$LOG"
mkdir -p "$DIR/runs"
cp "$LOG" "$DIR/runs/run-$START_TS.log" 2>/dev/null
ls -1t "$DIR/runs"/run-*.log 2>/dev/null | tail -n +501 | while read f; do rm -f "$f"; done
exit "$RC"
EOF

echo "== pushing binaries, release.txt, launch.sh, and APK via tools/dev.sh =="
tools/dev.sh push build/ioswine-loader ioswine-loader
tools/dev.sh push build/libsystem_kernel.dylib libsystem_kernel.dylib
tools/dev.sh push build/dsc/tls_sites.bin tls_sites.bin
tools/dev.sh push build/tmp/release.txt release.txt
tools/dev.sh push build/tmp/launch.sh launch.sh
tools/dev.sh push build/surface/ioswine-surface.apk ioswine-surface.apk

echo "== installing APK via pm install -r =="
INSTALL_OUT=$(tools/dev.sh root 'pm install -r /data/local/tmp/ioswine/ioswine-surface.apk')
printf '%s\n' "$INSTALL_OUT"
echo "$INSTALL_OUT" | grep -q '\[exit 0\]' || {
    echo "error: pm install failed" >&2
    exit 1
}

echo "== release complete ($GIT_DESC) =="
