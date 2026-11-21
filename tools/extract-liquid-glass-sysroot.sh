#!/usr/bin/env bash
# tools/extract-liquid-glass-sysroot.sh - Extract the iOS FeatureFlags
# plist directory (and any other plist/data dirs the Solarium / Liquid
# Glass decision needs) from a user-supplied IPSW into the ioswine
# sysroot.
#
# Why this exists: Aidoku (built with SDK 27.1,
# no UIDesignRequiresCompatibility) renders the iOS 18 legacy tab
# bar instead of the iOS 26 floating Liquid Glass one when the runtime's
# sysroot has no /System/Library/FeatureFlags (mksysroot.sh never
# extracted it), so whatever reads the Solarium feature flag finds
# nothing.
#
# What it extracts: ONLY plist/data files under the listed directories.
# No Mach-O binaries, no dyld cache, no frameworks are ever copied:
# every extracted file is checked to be a plist (bplist00 or XML
# <plist>) and anything else is deleted with a loud warning. No Apple
# files are shipped in the ioswine zip; this script runs on the user's
# own IPSW on their own machine.
#
# Usage:
#   tools/extract-liquid-glass-sysroot.sh <IPSW> [--dest DIR]
#       [--skip-sha1] [--keep-dmg] [--force]
#
#   IPSW   path to e.g. iPhone17,3_26.6.2_23G90_Restore.ipsw
#   --dest where to install (default: build/sysroot-<BUILD> next to
#          the mksysroot.sh output, so it merges into the sysroot)
#   --skip-sha1  do not verify the IPSW sha1 (default: verify when the
#          build is a known one, see EXPECTED_SHA1 below)
#   --keep-dmg   keep the decrypted filesystem dmg in build/tmp
#   --force      re-extract even if the target dir already exists
#
# Required host tools: ipsw, 7z or 7zz, unzip, python3, sha1sum.
# Idempotent, rootless host tool.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# Directories the Liquid Glass decision reads (plist/data only).
# FeatureFlags is the Solarium flag store.
FEATURE_DIRS=(
    "System/Library/FeatureFlags"
)

# Known IPSW sha1 values (ipsw.me). Only used to verify the file the
# user points at; unknown builds warn and continue.
declare -A EXPECTED_SHA1=(
    ["23G90"]="05442731f7be2b3910731766bb4f4e91b3858c95"
)

IPSW=""
DEST=""
SKIP_SHA1=0
KEEP_DMG=0
FORCE=0

while [ $# -gt 0 ]; do
    case "$1" in
        --dest) DEST="$2"; shift 2 ;;
        --dest=*) DEST="${1#--dest=}"; shift ;;
        --skip-sha1) SKIP_SHA1=1; shift ;;
        --keep-dmg) KEEP_DMG=1; shift ;;
        --force) FORCE=1; shift ;;
        -h|--help)
            sed -n '2,30p' "${BASH_SOURCE[0]}"
            exit 0 ;;
        -*)
            echo "ERROR: unknown option $1" >&2; exit 1 ;;
        *)
            if [ -n "$IPSW" ]; then
                echo "ERROR: only one IPSW argument is allowed" >&2; exit 1
            fi
            IPSW="$1"; shift ;;
    esac
done

if [ -z "$IPSW" ]; then
    echo "ERROR: no IPSW given. Usage: $0 <IPSW> [--dest DIR] [--skip-sha1] [--keep-dmg] [--force]" >&2
    exit 1
fi
if [ ! -f "$IPSW" ]; then
    echo "ERROR: IPSW not found at $IPSW" >&2
    exit 1
fi

for tool in ipsw unzip python3 sha1sum; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERROR: Required tool '$tool' not found in PATH" >&2
        exit 1
    fi
done

# 7-Zip ships as `7z` (p7zip) or `7zz` (7-Zip 24.09+ Linux binary,
# which is the one that reads APFS dmgs). Accept either.
if command -v 7z >/dev/null 2>&1; then
    SEVEN_Z=7z
elif command -v 7zz >/dev/null 2>&1; then
    SEVEN_Z=7zz
else
    echo "ERROR: Required tool '7z' (or '7zz') not found in PATH" >&2
    exit 1
fi

# Detect build tag (e.g. 23G90) from the IPSW filename or BuildManifest.
BUILD_TAG="unknown"
if [[ "$IPSW" =~ _([0-9]+[A-Z][0-9]+[a-z]?)_ ]]; then
    BUILD_TAG="${BASH_REMATCH[1]}"
elif [[ "$IPSW" =~ ([0-9]+[A-Z][0-9]+[a-z]?) ]]; then
    BUILD_TAG="${BASH_REMATCH[1]}"
fi

if [ -z "$DEST" ]; then
    DEST="build/sysroot-${BUILD_TAG}"
fi
TMP_DIR="build/tmp"
# Per-build dmg dir: never reuse another build's decrypted dmg if two
# IPSWs happen to use the same image filename.
DMG_DIR="${TMP_DIR}/dmg_out_${BUILD_TAG}"
mkdir -p "$TMP_DIR" "$DEST"

echo "=== Liquid Glass sysroot extractor ==="
echo "IPSW:        $IPSW"
echo "Build Tag:   $BUILD_TAG"
echo "Dest:        $DEST"
echo "Dirs:        ${FEATURE_DIRS[*]}"

# Step 0: verify the IPSW sha1 when the build is known.
if [ "$SKIP_SHA1" -eq 0 ] && [ -n "${EXPECTED_SHA1[$BUILD_TAG]:-}" ]; then
    echo "Step 0: Verifying IPSW sha1..."
    GOT="$(sha1sum "$IPSW" | awk '{print $1}')"
    WANT="${EXPECTED_SHA1[$BUILD_TAG]}"
    if [ "$GOT" != "$WANT" ]; then
        echo "ERROR: sha1 mismatch for build $BUILD_TAG" >&2
        echo "  got:      $GOT" >&2
        echo "  expected: $WANT" >&2
        echo "Re-download the IPSW, or pass --skip-sha1 to proceed anyway." >&2
        exit 1
    fi
    echo "  sha1 OK: $GOT"
elif [ "$SKIP_SHA1" -eq 0 ]; then
    echo "Step 0: Build $BUILD_TAG has no known sha1; skipping verification."
    echo "  (pass --skip-sha1 to silence this, or add the hash to EXPECTED_SHA1)"
else
    echo "Step 0: sha1 verification skipped (--skip-sha1)."
fi

# Step 1: fast path - already extracted.
ALREADY=1
for d in "${FEATURE_DIRS[@]}"; do
    if [ ! -d "$DEST/$d" ]; then ALREADY=0; fi
done
if [ "$FORCE" -eq 0 ] && [ "$ALREADY" -eq 1 ]; then
    echo "FeatureFlags already present under $DEST; nothing to do."
    echo "Pass --force to re-extract."
    exit 0
fi

# Step 2: obtain the decrypted filesystem dmg (same flow as
# tools/mksysroot.sh step 1: AEA-encrypted image first, plain dmg
# fallback).
echo "Step 1: Locating filesystem image in IPSW..."
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

# Step 3: extract only the listed dirs with 7z.
echo "Step 2: Extracting ${FEATURE_DIRS[*]} with 7z..."
EXTRACT_LOG="${TMP_DIR}/7z_liquid_glass.log"
set +e
"$SEVEN_Z" x -y -o"$DEST" "$DMG_PATH" "${FEATURE_DIRS[@]}" >"${EXTRACT_LOG}.stdout" 2>"${EXTRACT_LOG}"
EXT_RET=$?
set -e
if [ $EXT_RET -ne 0 ] && [ $EXT_RET -ne 2 ]; then
    echo "ERROR: 7z extraction failed with code $EXT_RET" >&2
    cat "${EXTRACT_LOG}" >&2
    exit 1
fi

# Step 4: plist-only validation. Every extracted file must be a plist
# (bplist00 magic or XML <plist>); anything else is deleted and
# reported. Mach-O binaries are a hard error.
echo "Step 3: Validating extracted files are plists only..."
python3 - "$DEST" "${FEATURE_DIRS[@]}" <<'EOF'
import os, sys

dest = sys.argv[1]
dirs = sys.argv[2:]
removed = 0
kept = 0
macho = 0

MACHO_MAGICS = {
    b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe",
    b"\xfe\xed\xfa\xcf", b"\xcf\xfa\xed\xfe",
    b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca",
    b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca",
}

for d in dirs:
    root = os.path.join(dest, d)
    if not os.path.isdir(root):
        print(f"WARNING: {d} not present after extraction")
        continue
    for dirpath, _dirnames, filenames in os.walk(root):
        for f in filenames:
            p = os.path.join(dirpath, f)
            if os.path.islink(p):
                continue
            with open(p, "rb") as fp:
                head = fp.read(512)
            if head[:4] in MACHO_MAGICS:
                print(f"ERROR: Mach-O binary in extracted tree: {p}")
                os.unlink(p)
                macho += 1
                continue
            is_bplist = head.startswith(b"bplist00")
            is_xml = head.lstrip().startswith(b"<?xml") and b"<plist" in head
            if is_bplist or is_xml:
                kept += 1
            else:
                print(f"WARNING: not a plist, removing: {p}")
                os.unlink(p)
                removed += 1

print(f"Kept {kept} plist files, removed {removed} non-plist files.")
if macho:
    print(f"ERROR: {macho} Mach-O files found; refusing to ship.")
    sys.exit(1)
EOF

# Step 5: report what was installed.
echo "--- Installed ---"
for d in "${FEATURE_DIRS[@]}"; do
    if [ -d "$DEST/$d" ]; then
        find "$DEST/$d" -type f | sort | while read -r f; do
            echo "  ${f#$DEST/} ($(stat -c%s "$f") bytes)"
        done
    fi
done
echo "Total: $(find "$DEST" -path "$DEST/System/Library/FeatureFlags*" -type f 2>/dev/null | wc -l) plist files under $DEST/System/Library/FeatureFlags"

# Step 6: clean up the decrypted dmg unless asked to keep it.
if [ "$KEEP_DMG" -eq 0 ]; then
    echo "Cleaning up decrypted DMG..."
    rm -rf "$DMG_DIR"
fi

echo "=== Liquid Glass sysroot extraction complete ==="
echo "Next: rebuild/push the sysroot (tools/mksysroot.sh documents the"
echo "adb push command) and relaunch the app on the device."
