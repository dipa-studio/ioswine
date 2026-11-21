#!/usr/bin/env bash
# tools/mksysroot.sh - Extract non-binary iOS sysroot from user-supplied IPSW
#
# Context:
#   The app process loads code from dyld shared cache, but frameworks need their
#   bundle directories on disk (Info.plist, *.strings, *.car, *.plist, data files).
#   Remaps /System, /Library, /usr/share to /data/local/tmp/ioswine/root on device.
#   /System/Library/EventTimingProfiles is also extracted, so the
#   real D47.Default.plist (UIKit event timing calibration) ships in the sysroot
#   instead of needing a hand-made stand-in.
#   /System/Library/FeatureFlags is also extracted, so the
#   Solarium (Liquid Glass) feature flag is readable in the sysroot (see
#   tools/extract-liquid-glass-sysroot.sh).
#
# Device push command (run from host, requires adb):
#   adb push build/sysroot-23G90.tar.zst /data/local/tmp/ioswine/root/
#   adb shell "mkdir -p /data/local/tmp/ioswine/root && tar --zstd -xf /data/local/tmp/ioswine/root/sysroot-23G90.tar.zst -C /data/local/tmp/ioswine/root && rm -f /data/local/tmp/ioswine/root/sysroot-23G90.tar.zst"
#   (or if plain tar: tar -xf ... /sysroot-23G90.tar ...)
#
# Idempotent, rootless host tool.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

IPSW="${1:-vendor-local/iPhone17,3_26.6.2_23G90_Restore.ipsw}"
FORCE="${FORCE:-0}"

if [ ! -f "$IPSW" ]; then
    echo "ERROR: IPSW not found at $IPSW" >&2
    exit 1
fi

# Detect build tag (e.g. 23G90) from IPSW filename or BuildManifest
BUILD_TAG="23G90"
if [[ "$IPSW" =~ _([0-9]+[A-Z][0-9]+[a-z]?)_ ]]; then
    BUILD_TAG="${BASH_REMATCH[1]}"
elif [[ "$IPSW" =~ ([0-9]+[A-Z][0-9]+[a-z]?) ]]; then
    BUILD_TAG="${BASH_REMATCH[1]}"
fi

SYSROOT_DIR="build/sysroot-${BUILD_TAG}"
TAR_ZST="build/sysroot-${BUILD_TAG}.tar.zst"
TAR_PLAIN="build/sysroot-${BUILD_TAG}.tar"
TMP_DIR="build/tmp"
DMG_DIR="${TMP_DIR}/dmg_out"

mkdir -p "$TMP_DIR"

echo "=== iOS Sysroot Extractor ==="
echo "IPSW:        $IPSW"
echo "Build Tag:   $BUILD_TAG"
echo "Sysroot Dir: $SYSROOT_DIR"

# Check required host utilities
for tool in ipsw 7z unzip python3; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERROR: Required tool '$tool' not found in PATH" >&2
        exit 1
    fi
done

# Python helper for Mach-O filtering
filter_macho() {
    local target_dir="$1"
    python3 - <<EOF
import os, sys

MACHO_MAGICS = {
    b"\xfe\xed\xfa\xce",  # MH_MAGIC
    b"\xce\xfa\xed\xfe",  # MH_CIGAM
    b"\xfe\xed\xfa\xcf",  # MH_MAGIC_64
    b"\xcf\xfa\xed\xfe",  # MH_CIGAM_64
    b"\xca\xfe\xba\xbe",  # FAT_MAGIC
    b"\xbe\xba\xfe\xca",  # FAT_CIGAM
    b"\xca\xfe\xba\xbf",  # FAT_MAGIC_64
    b"\xbf\xba\xfe\xca",  # FAT_CIGAM_64
}

target_dir = "$target_dir"
removed_count = 0
removed_bytes = 0

for root, dirs, files in os.walk(target_dir):
    for f in files:
        p = os.path.join(root, f)
        if os.path.islink(p):
            continue
        try:
            with open(p, "rb") as fp:
                hdr = fp.read(4)
                if hdr in MACHO_MAGICS:
                    sz = os.path.getsize(p)
                    os.unlink(p)
                    removed_count += 1
                    removed_bytes += sz
        except Exception as e:
            pass

print(f"Removed {removed_count} Mach-O binaries ({removed_bytes / (1024 * 1024):.2f} MB)")
EOF
}

# Helper to restore symlinks ignored by 7z due to '..' path traversal
restore_symlinks() {
    local log_file="$1"
    local target_dir="$2"
    python3 - <<EOF
import os, re, sys

log_file = "$log_file"
target_dir = "$target_dir"
count = 0

if os.path.exists(log_file):
    with open(log_file, "r", errors="ignore") as f:
        for line in f:
            m = re.search(r"Dangerous link path was ignored\s*:\s*(.*?)\s*:\s*(.*)", line)
            if m:
                rel_path = m.group(1).strip()
                link_target = m.group(2).strip()
                full_path = os.path.join(target_dir, rel_path)
                os.makedirs(os.path.dirname(full_path), exist_ok=True)
                if os.path.islink(full_path) or os.path.exists(full_path):
                    try:
                        os.unlink(full_path)
                    except OSError:
                        pass
                try:
                    os.symlink(link_target, full_path)
                    count += 1
                except OSError as e:
                    pass

# Ensure Library/Preferences symlink if missing
pref_path = os.path.join(target_dir, "Library/Preferences")
os.makedirs(os.path.dirname(pref_path), exist_ok=True)
if not os.path.exists(pref_path) and not os.path.islink(pref_path):
    try:
        os.symlink("../private/var/preferences", pref_path)
        count += 1
    except OSError:
        pass

print(f"Restored {count} symlinks")
EOF
}

# Helper to report size and file count
report_sysroot() {
    local target_dir="$1"
    local file_count
    file_count="$(find "$target_dir" -type f | wc -l)"
    local symlink_count
    symlink_count="$(find "$target_dir" -type l | wc -l)"
    local total_size
    total_size="$(du -sh "$target_dir" | cut -f1)"

    echo "--- Sysroot Summary ---"
    echo "Directory:     $target_dir"
    echo "Regular Files: $file_count"
    echo "Symlinks:      $symlink_count"
    echo "Disk Usage:    $total_size"

    # Verify 0 Mach-O
    local macho_count
    macho_count="$(find "$target_dir" -type f | xargs file | grep -c 'Mach-O' || true)"
    echo "Mach-O Files:  $macho_count"
    if [ "$macho_count" -ne 0 ]; then
        echo "ERROR: Found $macho_count Mach-O files in sysroot!" >&2
        exit 1
    fi
}

# Helper to create tarball
create_archive() {
    local target_dir="$1"
    local archive_target
    if command -v zstd >/dev/null 2>&1; then
        archive_target="$TAR_ZST"
        if [ ! -f "$archive_target" ] || [ "$FORCE" = "1" ]; then
            echo "Creating zstd-compressed archive: $archive_target..."
            tar -I 'zstd -T0 -3' -cf "$archive_target" -C "$(dirname "$target_dir")" "$(basename "$target_dir")"
        else
            echo "Archive $archive_target already exists."
        fi
    else
        archive_target="$TAR_PLAIN"
        if [ ! -f "$archive_target" ] || [ "$FORCE" = "1" ]; then
            echo "Creating plain tar archive: $archive_target..."
            tar -cf "$archive_target" -C "$(dirname "$target_dir")" "$(basename "$target_dir")"
        else
            echo "Archive $archive_target already exists."
        fi
    fi
    echo "Archive size: $(ls -lh "$archive_target" | awk '{print $5}') ($archive_target)"
}

# Fast path: Idempotent check
if [ "$FORCE" != "1" ] && [ -f "${SYSROOT_DIR}/System/Library/PrivateFrameworks/CoreEmoji.framework/Info.plist" ] && [ -d "${SYSROOT_DIR}/System/Library/EventTimingProfiles" ] && [ -d "${SYSROOT_DIR}/System/Library/FeatureFlags" ]; then
    echo "Sysroot already exists at ${SYSROOT_DIR} (CoreEmoji Info.plist present)."
    echo "Verifying Mach-O filtering and archive..."
    filter_macho "$SYSROOT_DIR"
    report_sysroot "$SYSROOT_DIR"
    create_archive "$SYSROOT_DIR"
    echo "=== Sysroot ready ==="
    exit 0
elif [ "$FORCE" = "1" ]; then
    echo "FORCE=1 set: removing existing sysroot and archives to reclaim disk space..."
    rm -rf "$SYSROOT_DIR" "$TAR_ZST" "$TAR_PLAIN"
fi

# Step 1: Identify and extract the FileSystem DMG from IPSW
echo "Step 1: Locating FileSystem DMG in IPSW..."
FS_AEA="$(unzip -l "$IPSW" | grep -oE '[0-9A-Za-z_-]+\.dmg\.aea' | grep -v 'Firmware' | head -n 1 || true)"
FS_DMG=""

mkdir -p "$DMG_DIR"

if [ -n "$FS_AEA" ]; then
    FS_DMG="${FS_AEA%.aea}"
    DMG_PATH="${DMG_DIR}/${FS_DMG}"
    if [ ! -f "$DMG_PATH" ]; then
        echo "Extracting AEA image $FS_AEA..."
        unzip -q -o "$IPSW" "$FS_AEA" -d "$TMP_DIR"
        echo "Decrypting $FS_AEA via ipsw fw aea..."
        ipsw fw aea "${TMP_DIR}/${FS_AEA}" -o "$DMG_DIR"
        rm -f "${TMP_DIR}/${FS_AEA}"
    else
        echo "Reusing existing decrypted DMG: $DMG_PATH"
    fi
else
    # Fallback to unencrypted DMG if present
    FS_DMG="$(unzip -l "$IPSW" | grep -oE '[0-9A-Za-z_-]+\.dmg' | grep -v 'Firmware' | head -n 1 || true)"
    if [ -z "$FS_DMG" ]; then
        echo "ERROR: Could not find filesystem DMG in $IPSW" >&2
        exit 1
    fi
    DMG_PATH="${DMG_DIR}/${FS_DMG}"
    if [ ! -f "$DMG_PATH" ]; then
        echo "Extracting DMG $FS_DMG..."
        unzip -q -o "$IPSW" "$FS_DMG" -d "$DMG_DIR"
    fi
fi

if [ ! -f "$DMG_PATH" ]; then
    echo "ERROR: Failed to obtain decrypted filesystem DMG at $DMG_PATH" >&2
    exit 1
fi

echo "Decrypted DMG: $DMG_PATH ($(ls -lh "$DMG_PATH" | awk '{print $5}'))"

# Step 2: Survey sizes of candidate trees and report threshold checks
echo "Step 2: Checking tree sizes..."
EXTRA_TREES=()
python3 - <<EOF
import subprocess, sys

dmg_path = "$DMG_PATH"
proc = subprocess.Popen(["7z", "l", dmg_path], stdout=subprocess.PIPE, text=True)

targets = {
    "System/Library/Fonts": 0,
    "System/Library/LinguisticData": 0,
}

for line in proc.stdout:
    if len(line) < 54 or line[20:21] == "D":
        continue
    try:
        size = int(line[26:38].strip())
        name = line[53:].strip()
    except Exception:
        continue
    for t in targets:
        if name == t or name.startswith(t + "/"):
            targets[t] += size
            break
proc.wait()

threshold = 500 * 1024 * 1024
with open("${TMP_DIR}/selected_trees.txt", "w") as out:
    for t, sz in targets.items():
        mb = sz / (1024 * 1024)
        if sz > threshold:
            print(f"  [SKIP] {t}: {mb:.2f} MB (> 500 MB threshold)")
        else:
            print(f"  [KEEP] {t}: {mb:.2f} MB (<= 500 MB threshold)")
            out.write(t + "\n")
EOF

# Collect trees to extract
TREES=(
    "System/Library/Frameworks"
    "System/Library/PrivateFrameworks"
    "System/Library/CoreServices"
    "System/Library/EventTimingProfiles"
    "System/Library/FeatureFlags"
    "usr/share"
)
if [ -f "${TMP_DIR}/selected_trees.txt" ]; then
    while IFS= read -r line; do
        [ -n "$line" ] && TREES+=("$line")
    done < "${TMP_DIR}/selected_trees.txt"
fi

echo "Trees to extract: ${TREES[*]}"

# Step 3: Extract directory trees into sysroot
echo "Step 3: Extracting sysroot trees with 7z..."
mkdir -p "$SYSROOT_DIR"
EXTRACT_LOG="${TMP_DIR}/7z_extract.log"
# 7z may return 2 when ignoring dangerous symlinks, which we handle afterwards
set +e
7z x -y -o"${SYSROOT_DIR}" "${DMG_PATH}" "${TREES[@]}" >"${EXTRACT_LOG}.stdout" 2>"${EXTRACT_LOG}"
EXT_RET=$?
set -e
if [ $EXT_RET -ne 0 ] && [ $EXT_RET -ne 2 ]; then
    echo "ERROR: 7z extraction failed with code $EXT_RET" >&2
    cat "$EXTRACT_LOG" >&2
    exit 1
fi

# Step 4: Restore skipped symlinks & create Library/Preferences
echo "Step 4: Restoring symlinks..."
restore_symlinks "$EXTRACT_LOG" "$SYSROOT_DIR"

# Step 5: Filter Mach-O binaries
echo "Step 5: Filtering Mach-O binaries..."
filter_macho "$SYSROOT_DIR"

# Step 6: Clean up intermediate decrypted DMG to conserve host disk space
echo "Step 6: Cleaning up decrypted DMG..."
rm -rf "$DMG_DIR"

# Step 7: Report sysroot summary
report_sysroot "$SYSROOT_DIR"

# Step 8: Create push archive
create_archive "$SYSROOT_DIR"

echo "=== Sysroot extraction completed successfully ==="
