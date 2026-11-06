#!/usr/bin/env bash
# Build the seekdb <-> hipVS bridge as one shared object.
#
# Two translation units:
#   seekdb_cuvs_bridge.c  - plain C, cuVS C API side (index lifecycle, search)
#   seekdb_bf_gemm.cpp    - HIP C++, rocBLAS + cuVS select_k exact-kNN engine
#
# The GEMM unit compiles against a vendored raft header tree (raft/) and the
# shim/ directory (CUDA->HIP mapping + stubbed logger headers), so no extra
# system packages are needed beyond what /opt/hipvs and /opt/rocm ship.

set -Eeuo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
HIPVS_ROOT=${HIPVS_ROOT:-/opt/hipvs}
ROCM_ROOT=${ROCM_ROOT:-/opt/rocm}
HIPCC=${HIPCC:-$ROCM_ROOT/bin/hipcc}
OUTPUT=${1:-$SCRIPT_DIR/libseekdb_cuvs_bridge.so}

[[ -x "$HIPCC" ]] || { echo "hipcc is not executable: $HIPCC" >&2; exit 2; }
[[ -f "$HIPVS_ROOT/include/cuvs/core/c_api.h" ]] || {
  echo "hipVS headers are missing under $HIPVS_ROOT/include" >&2
  exit 2
}
[[ -f "$HIPVS_ROOT/lib/libcuvs_c.so" ]] || {
  echo "libcuvs_c.so is missing under $HIPVS_ROOT/lib" >&2
  exit 2
}
[[ -f "$SCRIPT_DIR/raft/include/raft/core/device_mdspan.hpp" ]] || {
  echo "vendored raft headers are missing under $SCRIPT_DIR/raft/include" >&2
  exit 2
}

mkdir -p "$(dirname "$OUTPUT")"

COMMON=(-shared -fPIC -O2 -D__HIP_PLATFORM_AMD__
        -I"$SCRIPT_DIR" -I"$SCRIPT_DIR/raft/include" -I"$SCRIPT_DIR/shim"
        -I"$HIPVS_ROOT/include" -I"$ROCM_ROOT/include")

# The WMMA kernel needs the rocwmma headers; skip it (rocBLAS fallback) when
# the ROCm install does not ship them, so the official build never breaks.
WMA_FLAGS=""
if [[ -f "$ROCM_ROOT/include/rocwmma/rocwmma.hpp" ]]; then
  WMA_FLAGS="-DSK_USE_ROCWMA"
fi

"$HIPCC" -c "${COMMON[@]}" $WMA_FLAGS "$SCRIPT_DIR/seekdb_cuvs_bridge.c" \
  -o "$SCRIPT_DIR/.bridge_c.o"
"$HIPCC" -c "${COMMON[@]}" $WMA_FLAGS "$SCRIPT_DIR/seekdb_bf_gemm.cpp" \
  -o "$SCRIPT_DIR/.bridge_cpp.o"

"$HIPCC" -shared \
  "$SCRIPT_DIR/.bridge_c.o" "$SCRIPT_DIR/.bridge_cpp.o" \
  -L"$HIPVS_ROOT/lib" \
  -Wl,-soname,libseekdb_cuvs_bridge.so \
  -Wl,-rpath,"$HIPVS_ROOT/lib:$ROCM_ROOT/lib" \
  -lcuvs_c -lcuvs -lrocblas \
  -o "$OUTPUT"

rm -f "$SCRIPT_DIR/.bridge_c.o" "$SCRIPT_DIR/.bridge_cpp.o"
echo "built $OUTPUT"
