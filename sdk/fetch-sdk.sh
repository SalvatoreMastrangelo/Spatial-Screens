#!/usr/bin/env bash
#
# fetch-sdk.sh — download the VITURE XR Glasses SDK v2.0.0 (Linux x86_64) into
# this directory so the bridge and spatial-screens can build.
#
# The SDK binaries are © VITURE Inc. (all rights reserved) and are NOT
# redistributed in this repository. This script fetches them, unmodified, from
# the pinned upstream commit of wheaney/XRLinuxDriver (which redistributes the
# official VITURE SDK for its VITURE support) and verifies every file by
# sha256. See ./README.md for provenance.
#
# Usage:  ./sdk/fetch-sdk.sh   (idempotent — re-run any time; skips valid files)

set -euo pipefail

COMMIT=08b3bc93d1dff5f8843be30fbc262fb5364f7fa3
REPO=wheaney/XRLinuxDriver
RAW="https://raw.githubusercontent.com/${REPO}/${COMMIT}"

SDK_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INC_DIR="${SDK_DIR}/include"
LIB_DIR="${SDK_DIR}/lib/x86_64"

# --- sha256 manifest: "sha  path-relative-to-sdk-dir  upstream-subpath" -------
# Headers (include/sdks/*.h upstream) and the real (non-symlink) libraries.
HEADERS=(
  viture_device.h
  viture_device_carina.h
  viture_glasses_constants.h
  viture_glasses_provider.h
  viture_macros.h
  viture_protocol.h
  viture_version.h
)

# real library files -> sha256 (upstream lib/x86_64/viture/<name>)
declare -A LIB_SHA=(
  [libglasses.so]=473a40aaa05d70817ba586f2c2f03221887a6497411b874b86be0c9d6dfb64b5
  [libcarina_vio.so]=3e99f6d62fe0381ef943af60111a6b61d5a2adde58ab0984244009cd09ff6788
  [libopencv_calib3d.so.4.2.0]=f8c9031754ca502f3434e91e642be18a95899ea7c300e102d0fc2d62ba28aa26
  [libopencv_core.so.4.2.0]=8416508befee50af0f3c201f4d3e3fc539f012cb9392518e4559d0a91ff7ccec
  [libopencv_features2d.so.4.2.0]=c485906598ce178fc7dd861a580e1a9471d70567dac29e82628c900c37f8928b
  [libopencv_flann.so.4.2.0]=3a08a5479c8d21708fe9500a54a68010de21bd8c1ac9fa26a29c50a19a5134f1
  [libopencv_highgui.so.4.2.0]=23d89ed34394f4753ba0ea32111bf41201ad5edf9b4c81a82def4cc6668746c4
  [libopencv_imgcodecs.so.4.2.0]=a6f3485a65785e9e4378f47eb324ede164e052cd4590872b9e7325971a46c367
  [libopencv_imgproc.so.4.2.0]=704e30a31b25a1734410d9af0425dd5fe211c80c3dd66aaee94ebc2abf6fc555
  [libopencv_videoio.so.4.2.0]=899443baaa1d37abe7b380af664a0865ff0cb561340f53a141671dfd28f1695c
  [libopencv_video.so.4.2.0]=da9b6e067f34aab5a817239785534054e461df99dced2f4727dc70e1ad0dee3d
)

# OpenCV libs that need the classic 3-name symlink chain (.so -> .so.4.2 -> .so.4.2.0)
OPENCV=(calib3d core features2d flann highgui imgcodecs imgproc video videoio)

have() { command -v "$1" >/dev/null 2>&1; }
have curl   || { echo "error: curl is required" >&2; exit 1; }
have sha256sum || { echo "error: sha256sum is required" >&2; exit 1; }

# download <url> <dest> — atomic, with retry
dl() {
  local url="$1" dest="$2" tmp
  tmp="$(mktemp "${dest}.XXXXXX")"
  if curl -fsSL --retry 3 --retry-delay 2 "$url" -o "$tmp"; then
    mv -f "$tmp" "$dest"
  else
    rm -f "$tmp"; echo "error: failed to download $url" >&2; exit 1
  fi
}

# fetch_verified <dest-abs> <upstream-url> <sha256> — skip if already valid
fetch_verified() {
  local dest="$1" url="$2" want="$3" got
  if [ -f "$dest" ] && [ ! -L "$dest" ]; then
    got="$(sha256sum "$dest" | cut -d' ' -f1)"
    [ "$got" = "$want" ] && { echo "  ok (cached)  ${dest#"$SDK_DIR"/}"; return; }
  fi
  echo "  fetch        ${dest#"$SDK_DIR"/}"
  dl "$url" "$dest"
  got="$(sha256sum "$dest" | cut -d' ' -f1)"
  [ "$got" = "$want" ] || { echo "error: checksum mismatch for $dest" >&2; \
      echo "  want $want" >&2; echo "  got  $got" >&2; exit 1; }
}

echo "Fetching VITURE XR Glasses SDK v2.0.0 (x86_64) from ${REPO}@${COMMIT:0:12}"
mkdir -p "$INC_DIR" "$LIB_DIR"

echo "headers:"
for h in "${HEADERS[@]}"; do
  # headers are small text — download then just confirm non-empty
  dl "${RAW}/include/sdks/${h}" "${INC_DIR}/${h}"
  [ -s "${INC_DIR}/${h}" ] || { echo "error: empty header ${h}" >&2; exit 1; }
  echo "  ok           include/${h}"
done

echo "libraries:"
for name in "${!LIB_SHA[@]}"; do
  fetch_verified "${LIB_DIR}/${name}" "${RAW}/lib/x86_64/viture/${name}" "${LIB_SHA[$name]}"
done

echo "symlinks:"
for base in "${OPENCV[@]}"; do
  ( cd "$LIB_DIR"
    ln -sf "libopencv_${base}.so.4.2.0" "libopencv_${base}.so.4.2"
    ln -sf "libopencv_${base}.so.4.2"   "libopencv_${base}.so" )
  echo "  ok           lib/x86_64/libopencv_${base}.so{,.4.2} -> .so.4.2.0"
done

echo
echo "Done. SDK is ready in ${SDK_DIR#"$PWD"/}"
echo "Next: build the bridge with 'cd bridge && make', or spatial-screens per its README."
