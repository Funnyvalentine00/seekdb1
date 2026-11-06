#pragma once

// Test-only external library doubles. Production host control flow, epilogue,
// row norm, copy, and merge kernels are compiled from seekdb_bf_gemm.cpp.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <vector>

#define __global__
#define __shared__
#define __syncthreads() ((void)0)
// Fixtures use small integers exactly representable in fp16. Store their
// numerical values in 16-bit cells; this model does not test fp16 encoding.
using __half = int16_t;
inline float __half2float(__half x) { return static_cast<float>(x); }
struct dim3 {
  unsigned x, y, z;
  dim3(unsigned a = 1, unsigned b = 1, unsigned c = 1) : x(a), y(b), z(c) {}
};
static dim3 blockIdx, blockDim, threadIdx, gridDim;

template <class F, class... Args>
void cpu_launch(F fn, dim3 grid, dim3 block, Args... args)
{
  gridDim = grid;
  blockDim = block;
  for (blockIdx.x = 0; blockIdx.x < grid.x; ++blockIdx.x) {
    for (threadIdx.x = 0; threadIdx.x < block.x; ++threadIdx.x) { fn(args...); }
  }
}

// The reduction kernel also supports a one-thread block: its strided input
// loop sums the whole row. No fake shared-memory barriers are needed.
inline void cpu_launch(void (*fn)(const unsigned short*, float*, long, long),
                       dim3 grid, dim3, const unsigned short* x, float* out,
                       long n, long dim)
{
  gridDim = grid;
  blockDim = dim3(1);
  threadIdx = dim3(0);
  for (blockIdx.x = 0; blockIdx.x < grid.x; ++blockIdx.x) { fn(x, out, n, dim); }
}

using hipError_t = int;
constexpr hipError_t hipSuccess = 0;
constexpr int hipMemcpyDeviceToHost = 1;
static std::unordered_set<void*> allocations;
static int allocation_calls = 0, fail_allocation = 0, live_handles = 0, live_events = 0;
static bool fail_copy = false, fail_selection = false;
inline const char* hipGetErrorString(int) { return "injected failure"; }
inline int hipGetLastError() { return 0; }
template <class T> int hipMalloc(T** out, size_t bytes)
{
  if (++allocation_calls == fail_allocation) { return 1; }
  *out = static_cast<T*>(std::malloc(bytes));
  if (!*out) { return 1; }
  allocations.insert(*out);
  return 0;
}
inline int hipFree(void* p)
{
  if (p && allocations.erase(p) != 1) { std::abort(); }
  std::free(p);
  return 0;
}
inline int hipMemcpy2D(void* dst, size_t dp, const void* src, size_t sp,
                       size_t width, size_t height, int)
{
  if (fail_copy) { return 1; }
  for (size_t i = 0; i < height; ++i) {
    std::memcpy(static_cast<char*>(dst) + i * dp,
                static_cast<const char*>(src) + i * sp, width);
  }
  return 0;
}
using hipEvent_t = int*;
inline int hipEventCreate(hipEvent_t* e) { *e = new int(0); ++live_events; return 0; }
inline int hipEventDestroy(hipEvent_t e) { delete e; --live_events; return 0; }
inline int hipEventRecord(hipEvent_t, int) { return 0; }
inline int hipEventSynchronize(hipEvent_t) { return 0; }
inline int hipEventElapsedTime(float* ms, hipEvent_t, hipEvent_t) { *ms = 0; return 0; }

using rocblas_handle = int*;
using rocblas_status = int;
using rocblas_int = int;
constexpr int rocblas_status_success = 0, rocblas_operation_transpose = 1,
  rocblas_operation_none = 2, rocblas_datatype_f16_r = 3,
  rocblas_datatype_f32_r = 4, rocblas_gemm_algo_standard = 5;
inline int rocblas_create_handle(rocblas_handle* h)
{ *h = new int(0); ++live_handles; return 0; }
inline int rocblas_destroy_handle(rocblas_handle h)
{ delete h; --live_handles; return 0; }
inline int rocblas_gemm_ex(rocblas_handle, int, int, int rows, int cols, int dim,
                          const float* alpha, const unsigned short* av, int, int lda,
                          const unsigned short* bv, int, int ldb, const float*,
                          const float*, int, int, float* dst, int, int ldd, int,
                          int, int, int)
{
  const auto* a = reinterpret_cast<const __half*>(av);
  const auto* b = reinterpret_cast<const __half*>(bv);
  for (int j = 0; j < cols; ++j) {
    for (int i = 0; i < rows; ++i) {
      float dot = 0;
      for (int c = 0; c < dim; ++c) { dot += float(a[i * lda + c]) * float(b[j * ldb + c]); }
      dst[j * ldd + i] = *alpha * dot;
    }
  }
  return 0;
}

namespace raft {
class resources {};
struct row_major {};
template <class T, class I, class Layout = row_major> struct device_matrix_view {
  T* data; I rows; I cols;
};
template <class T, class I> struct device_vector_view {};
template <class T, class I>
device_matrix_view<T, I> make_device_matrix_view(T* p, I rows, I cols)
{ return {p, rows, cols}; }
namespace matrix {
enum class SelectAlgo { kAuto, kRadix8bits, kRadix11bits, kRadix11bitsExtraPass, kWarpAuto };
}
}

namespace cuvs { namespace selection {
inline void select_k(const raft::resources&,
    raft::device_matrix_view<const float, int64_t> in,
    std::optional<raft::device_matrix_view<const int, int64_t>>,
    raft::device_matrix_view<float, int64_t> val,
    raft::device_matrix_view<int, int64_t> idx, bool, bool,
    raft::matrix::SelectAlgo,
    std::optional<raft::device_vector_view<const int, int64_t>>)
{
  if (fail_selection || val.cols > in.cols) { throw std::runtime_error("invalid select_k"); }
  for (int64_t r = 0; r < in.rows; ++r) {
    std::vector<std::pair<float, int>> row;
    for (int c = 0; c < in.cols; ++c) { row.emplace_back(in.data[r * in.cols + c], c); }
    std::sort(row.begin(), row.end());
    for (int64_t c = 0; c < val.cols; ++c) {
      val.data[r * val.cols + c] = row[c].first;
      idx.data[r * idx.cols + c] = row[c].second;
    }
  }
}
}}
