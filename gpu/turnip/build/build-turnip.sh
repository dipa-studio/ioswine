#!/usr/bin/env bash
# Build Mesa 26.2.7 Turnip (freedreno, KGSL-only) for Android arm64.
# Produces build-android-aarch64/src/freedreno/vulkan/libvulkan_freedreno.so
# and copies it to ./libvulkan_freedreno.so.
#
# Runnable from a clean checkout: it downloads the Mesa tarball, the
# host tools (meson/ninja via a venv, glslang/bison/flex/m4 as .debs
# extracted into ./tools), writes the cross file and the ICD-export
# patch, then runs meson setup + compile. The only input is the NDK.
#
# Usage:
#   NDK=/path/to/android-ndk-r28c ./build-turnip.sh
#   ./build-turnip.sh            (uses the default NDK below)
# Options: -j N sets the ninja job count (default 2; the VM is RAM-tight).
#
# Requirements: curl, xz, tar, python3, apt-get (only for pkg-config;
# the build itself does not depend on system packages because the
# apt-installed glslang/bison/flex did not survive a VM reset here,
# so they are fetched as .debs and extracted into ./tools instead).
set -euo pipefail

NDK="${NDK:-$HOME/workspace/tools/android-ndk-r28c}"
MESA_VERSION="26.2.7"
MESA_TARBALL="mesa-${MESA_VERSION}.tar.xz"
MESA_URL="https://archive.mesa3d.org/${MESA_TARBALL}"
# Official SHA256 from https://docs.mesa3d.org/relnotes/26.2.7.html
MESA_SHA256="04294f75d22e9f61cd1e050206bc48b3d39366e788f1803a3368e3d5a872ce18"
JOBS="2"
DEB_BASE="http://azure.archive.ubuntu.com/ubuntu"

while getopts "j:" opt; do
  case "$opt" in
    j) JOBS="$OPTARG" ;;
  esac
done

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

echo "== NDK: $NDK"
[ -d "$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin" ] || {
  echo "NDK not found at $NDK (set NDK=...)" >&2; exit 1; }

# ---------------------------------------------------------------- venv + meson
if [ ! -x venv/bin/meson ]; then
  echo "== creating venv, installing meson/ninja/mako/pyyaml/packaging"
  python3 -m venv venv
  venv/bin/pip install -q meson ninja mako pyyaml packaging
fi
echo "== meson $(venv/bin/meson --version), ninja $(venv/bin/ninja --version)"

# ---------------------------------------------------------------- host tools
# glslangValidator, bison, flex, m4 are needed at configure AND build time
# (glslang compiles .comp shaders to .spv.h; bison/flex generate the ir3
# parser). They are fetched as Ubuntu .debs and extracted under ./tools so
# the build does not depend on apt-installed packages surviving.
TOOLS_DIR="$HERE/tools/extracted"
if [ ! -x "$TOOLS_DIR/usr/bin/glslangValidator" ] || \
   [ ! -x "$TOOLS_DIR/usr/bin/bison" ] || \
   [ ! -x "$TOOLS_DIR/usr/bin/flex" ] || \
   [ ! -x "$TOOLS_DIR/usr/bin/m4" ]; then
  echo "== downloading host-tool .debs"
  mkdir -p "$HERE/tools"
  cd "$HERE/tools"
  for deb in \
    "pool/universe/g/glslang/glslang-tools_15.1.0-2~ubuntu0.24.04.2_amd64.deb" \
    "pool/main/b/bison/bison_3.8.2+dfsg-1build1_amd64.deb" \
    "pool/main/f/flex/flex_2.6.4-8.2build1_amd64.deb" \
    "pool/main/f/flex/libfl-dev_2.6.4-8.2build1_amd64.deb" \
    "pool/main/m/m4/m4_1.4.19-4build1_amd64.deb"; do
    f="$(basename "$deb")"
    [ -f "$f" ] || curl -fsSL -o "$f" "$DEB_BASE/$deb"
  done
  mkdir -p extracted
  for deb in *.deb; do dpkg-deb -x "$deb" extracted/; done
  cd "$HERE"
fi
export PATH="$TOOLS_DIR/usr/bin:$HERE/venv/bin:$PATH"
export BISON_PKGDATADIR="$TOOLS_DIR/usr/share/bison"
export M4="$TOOLS_DIR/usr/bin/m4"
echo "== glslangValidator $(glslangValidator --version | head -1)"
echo "== bison $(bison --version | head -1), flex $(flex --version | head -1), m4 $(m4 --version | head -1)"

# pkg-config is convenient but not required for this configuration.
command -v pkg-config >/dev/null 2>&1 || {
  echo "== installing pkg-config (optional, best effort)"
  apt-get install -y pkg-config || true; }

# ---------------------------------------------------------------- Mesa source
if [ ! -d "mesa-${MESA_VERSION}" ]; then
  echo "== downloading Mesa ${MESA_VERSION}"
  [ -f "$MESA_TARBALL" ] || curl -fL -o "$MESA_TARBALL" "$MESA_URL"
  echo "$MESA_SHA256  $MESA_TARBALL" | sha256sum -c -
  tar --no-same-owner -xf "$MESA_TARBALL"
fi
# The stock mesa/android-aarch64 cross file does NOT ship in the release
# tarball, so we write our own (see mesa-android-aarch64-cross.txt).
cat > mesa-android-aarch64-cross.txt <<EOF
[constants]
ndk_path = '$NDK'

[binaries]
ar = ndk_path / 'toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-ar'
c = [ndk_path / 'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android34-clang', '-fno-exceptions', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables']
cpp = [ndk_path / 'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android34-clang++', '-fno-exceptions', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '--start-no-unused-arguments', '-static-libstdc++', '--end-no-unused-arguments']
c_ld = ndk_path / 'toolchains/llvm/prebuilt/linux-x86_64/bin/ld.lld'
cpp_ld = ndk_path / 'toolchains/llvm/prebuilt/linux-x86_64/bin/ld.lld'
strip = ndk_path / 'toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip'

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8'
endian = 'little'

[properties]
needs_exe_wrapper = true
EOF

# ---------------------------------------------------------------- ICD export patch
# Stock Mesa Android exports only HMI (the HAL module) from the driver,
# which makes dlsym("vk_icdGetInstanceProcAddr") fail. Export the three
# ICD entry points as well. See vulkan-android-sym.patch.
cat > "mesa-${MESA_VERSION}/src/vulkan/vulkan-android.sym" <<'EOF'
{
	global:
		# Andoid looks for this global in HAL modules. In the source it occurs
		# as HAL_MODULE_INFO_SYM (which is just a #define for HMI) and it's an
		# instance of struct hwvulkan_module_t.
		HMI;

		# ioswine: also export the ICD entry points so the driver can be
		# loaded by direct dlopen + vk_icdGetInstanceProcAddr (the GPU
		# helper process path). Stock Android Mesa exports only HMI,
		# which makes dlsym("vk_icdGetInstanceProcAddr") fail.
		vk_icdGetInstanceProcAddr;
		vk_icdGetPhysicalDeviceProcAddr;
		vk_icdNegotiateLoaderICDInterfaceVersion;

	local:
		# When static linking LLVM, all its symbols are public API.
		# That may cause symbol collision, so explicitly demote everything.
		*;
};
EOF

# ---------------------------------------------------------------- meson setup
# -Dfreedreno-kmds=kgsl (not msm,kgsl): msm forces system_has_kms_drm and
# requires libdrm at configure time, which is not available for Android
# aarch64 here (no pkg-config cross libdrm, wrapdb downloads time out).
# The runtime path is KGSL either way; the known kgsl-only SIGBUS was at
# first present, and this build is offscreen-first.
if [ ! -d build-android-aarch64 ]; then
  echo "== meson setup"
  venv/bin/meson setup build-android-aarch64 "mesa-${MESA_VERSION}" \
    --cross-file mesa-android-aarch64-cross.txt \
    -Dplatforms=android -Dplatform-sdk-version=34 \
    -Dandroid-stub=true -Dandroid-libbacktrace=disabled \
    -Dgallium-drivers= -Dvulkan-drivers=freedreno \
    -Dfreedreno-kmds=kgsl -Degl=disabled -Dbuildtype=release
else
  echo "== build dir exists, reconfiguring in place"
  venv/bin/meson setup --reconfigure build-android-aarch64 "mesa-${MESA_VERSION}"
fi

# ---------------------------------------------------------------- compile
echo "== meson compile -j $JOBS"
venv/bin/ninja -C build-android-aarch64 -j "$JOBS"

SO="build-android-aarch64/src/freedreno/vulkan/libvulkan_freedreno.so"
cp "$SO" libvulkan_freedreno.so
echo "== built libvulkan_freedreno.so"
ls -la libvulkan_freedreno.so
sha256sum libvulkan_freedreno.so
file libvulkan_freedreno.so

if [ -f gen-symbols.py ]; then
  echo "== symbols.txt"
  python3 gen-symbols.py libvulkan_freedreno.so
fi
echo "== done"
