#!/usr/bin/env bash
# Real GPU checks. Requires an already built bridge and the matching hipVS libs.
set -Eeuo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../../../.." && pwd)
BRIDGE_DIR="$ROOT/tools/hipvs_bridge"
BRIDGE_LIB=${1:-$BRIDGE_DIR/libseekdb_cuvs_bridge.so}
HIPVS_ROOT=${HIPVS_ROOT:-/opt/hipvs}
ROCM_ROOT=${ROCM_ROOT:-/opt/rocm}
HIPCC=${HIPCC:-$ROCM_ROOT/bin/hipcc}
GPU_ARCH=${GPU_ARCH:-$("$ROCM_ROOT/bin/rocminfo" 2>/dev/null | grep -om1 'gfx[0-9a-f]*' || true)}
[[ -n "$GPU_ARCH" ]] || { echo "Set GPU_ARCH to the target GPU architecture" >&2; exit 2; }
[[ -f "$BRIDGE_LIB" ]] || { echo "Build the bridge first: $BRIDGE_LIB" >&2; exit 2; }

GPU_TEST_TMP=$(mktemp -d -t seekdb-gpu-tests.XXXXXXXX)
trap 'rm -rf -- "$GPU_TEST_TMP"' EXIT
COMMON=(-std=c++17 -O2 -D__HIP_PLATFORM_AMD__ "--offload-arch=$GPU_ARCH")
# Every candidate has the production SONAME. Resolve it to the requested file,
# rather than a baseline with that SONAME in the candidate's parent directory.
ln -s "$(realpath "$BRIDGE_LIB")" "$GPU_TEST_TMP/libseekdb_cuvs_bridge.so"

if [[ -f "$ROCM_ROOT/include/rocwmma/rocwmma.hpp" ]]; then
  "$HIPCC" "${COMMON[@]}" -DSK_USE_ROCWMA -ffunction-sections -fdata-sections \
    -Wl,--gc-sections -I"$BRIDGE_DIR" -I"$BRIDGE_DIR/raft/include" -I"$BRIDGE_DIR/shim" \
    "$HERE/test_gpu_kernels.cpp" -lrocblas -o "$GPU_TEST_TMP/kernels"
  "$GPU_TEST_TMP/kernels"
else
  echo "SKIP standalone WMMA kernels: rocWMMA headers are unavailable"
fi

"$HIPCC" "${COMMON[@]}" -I"$HIPVS_ROOT/include" "$HERE/test_gpu_entry.cpp" \
  -L"$(dirname "$BRIDGE_LIB")" "-l:$(basename "$BRIDGE_LIB")" -L"$HIPVS_ROOT/lib" -lcuvs_c \
  -Wl,-rpath,"$GPU_TEST_TMP:$HIPVS_ROOT/lib:$ROCM_ROOT/lib" \
  -o "$GPU_TEST_TMP/entry"
LD_LIBRARY_PATH="$GPU_TEST_TMP:$HIPVS_ROOT/lib:$ROCM_ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  "$GPU_TEST_TMP/entry" 2>&1 | tee "$GPU_TEST_TMP/entry.log"
if [[ -f "$ROCM_ROOT/include/rocwmma/rocwmma.hpp" ]]; then
  grep -Eq 'wma=1 fused_tiles=[1-9][0-9]*' "$GPU_TEST_TMP/entry.log"
  grep -Eq 'wma=1 fused_tiles=0 wmma_tiles=[1-9][0-9]*' "$GPU_TEST_TMP/entry.log"
  echo "PASS: linked bridge executed fused and unfused WMMA paths"
else
  echo "SKIP WMMA execution coverage: rocWMMA headers unavailable"
fi
grep -Eq 'wma=0 fused_tiles=0 wmma_tiles=0 blas_tiles=[1-9][0-9]*' "$GPU_TEST_TMP/entry.log"
echo "PASS: linked bridge executed rocBLAS fallback"
