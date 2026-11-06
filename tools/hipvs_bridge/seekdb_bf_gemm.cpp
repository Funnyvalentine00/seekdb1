/* seekdb <-> hipVS bridge, GEMM part: exact-kNN distances via rocBLAS.
 *
 * The 128-D exact-kNN graph build spends its GPU time inside cuVS brute-force
 * search: a plain map_kernel for the distances (15-20% SM efficiency) plus
 * warpsort for the top-k (~56% of the whole build). This file replaces that
 * path with
 *   - one fp16 GEMM per (query-tile x base-tile) block computing
 *     dist = -2<a,b> + ||b||^2 (the query norm is a per-row constant and does
 *     not affect the ordering, so it is skipped), accumulated in fp32 by
 *     rocBLAS on tensor cores, and
 *   - cuVS select_k (the same radix-select family cuVS brute force uses) to
 *     keep the top-gd neighbours per row, merged into a running device-side
 *     best buffer so the host never sees more than one final D2H copy.
 * Symmetrize and cuvsCagraIndexFromArgs stay in the plain-C bridge; the
 * extern "C" entry point below is the only interface between them.
 *
 * raft/cuVS C++ headers are included through a small vendor tree plus the
 * shim/ directory (cuda_runtime.h maps onto HIP and the two logger headers
 * are stubbed). The upstream select_k is NOT included; its prototype is
 * re-declared here with types from the raft headers, which mangles to the
 * exact symbol shipped in libcuvs.so. cuvsResources_t is (per the hipVS C
 * API source) a `new raft::resources` pointer, so the bridge's resource
 * handle is reinterpreted directly. */
#include <hip/hip_runtime.h>
#include <hip/hip_fp16.h>
#include <rocblas/rocblas.h>
#include <raft/core/device_mdspan.hpp>
#include <raft/matrix/select_k_types.hpp>
#include <optional>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <pthread.h>

#include <rocwmma/rocwmma.hpp>
using namespace rocwmma;

namespace raft { class resources; }

namespace cuvs { namespace selection {
void select_k(const raft::resources& handle,
  raft::device_matrix_view<const float, int64_t, raft::row_major> in_val,
  std::optional<raft::device_matrix_view<const int, int64_t, raft::row_major>> in_idx,
  raft::device_matrix_view<float, int64_t, raft::row_major> out_val,
  raft::device_matrix_view<int, int64_t, raft::row_major> out_idx,
  bool select_min, bool sorted, raft::matrix::SelectAlgo algo,
  std::optional<raft::device_vector_view<const int, int64_t>> len_i);
}}

#define SK_GEMM_MAX_GD 128   /* merge kernel keeps candidates in registers */

#define SK_HIP_CHECK(call)                                                   \
  do {                                                                       \
    hipError_t e_ = (call);                                                  \
    if (e_ != hipSuccess) {                                                  \
      fprintf(stderr, "[seekdb_bf_gemm] hip error %d (%s) at %s:%d\n",       \
              (int)e_, hipGetErrorString(e_), __FILE__, __LINE__);           \
      return 1;                                                              \
    }                                                                        \
  } while (0)

static pthread_mutex_t g_blas_mu = PTHREAD_MUTEX_INITIALIZER;
static rocblas_handle g_blas = NULL;

static rocblas_handle sk_get_blas(void)
{
  pthread_mutex_lock(&g_blas_mu);
  if (g_blas == NULL) {
    if (rocblas_create_handle(&g_blas) != rocblas_status_success) {
      g_blas = NULL;
    }
  }
  pthread_mutex_unlock(&g_blas_mu);
  return g_blas;
}

/* ||b||^2 for every base row (fp16 input, fp32 accumulate). */
__global__ static void sk_row_norm2_kernel(const unsigned short* x, float* out,
                                           long n, long dim)
{
  const long i = blockIdx.x;
  __shared__ float sh[256];
  float s = 0.f;
  const __half* row = (const __half*)(x + (size_t)i * dim);
  for (long c = threadIdx.x; c < dim; c += blockDim.x) {
    const float v = __half2float(row[c]);
    s += v * v;
  }
  sh[threadIdx.x] = s;
  __syncthreads();
  for (int t = blockDim.x / 2; t > 0; t >>= 1) {
    if (threadIdx.x < t) { sh[threadIdx.x] += sh[threadIdx.x + t]; }
    __syncthreads();
  }
  if (threadIdx.x == 0) { out[i] = sh[0]; }
}

/* Epilogue: dist[i][j] = ||b_j||^2 + s2 * ab[i][j] (row-major [m x tb], ldc = tb).
 * rocBLAS already applies alpha=-2 (s2=1); the WMMA kernel emits +ab (s2=-2). */
__global__ static void sk_add_normb_kernel(float* c, const float* nb,
                                           long total, long tb, int s2)
{
  long idx = (long)blockIdx.x * blockDim.x + threadIdx.x;
  const long stride = (long)gridDim.x * blockDim.x;
  for (; idx < total; idx += stride) {
    const float v = nb[idx % tb] + (float)s2 * c[idx];
    c[idx] = v;
  }
}

/* WMMA GEMM: dist[i][j] = -2 * <a_i, b_j>, row-major [m x tb] (ldc = tb).
 * One 64x64 output tile per block (16 warps, 16x16 per warp), fp16 inputs,
 * fp32 accumulate on tensor cores. Tail tiles (not multiples of 64) fall
 * back to rocBLAS in the caller. */
__global__ __launch_bounds__(512) static void sk_wmma_gemm_kernel(
    const unsigned short* base, const unsigned short* q, float* out,
    long m, long tb, long dim)
{
  const int wx = (threadIdx.x / 32) & 3;
  const int wy = (threadIdx.x / 32) >> 2;
  const long j0 = (long)blockIdx.x * 64 + wy * 16;   /* base row   */
  const long i0 = (long)blockIdx.y * 64 + wx * 16;   /* query row  */
  fragment<matrix_a, 16, 16, 16, float16_t, row_major> fa;
  fragment<matrix_b, 16, 16, 16, float16_t, col_major> fb;
  fragment<accumulator, 16, 16, 16, float32_t, col_major> fc;
  fill_fragment(fc, 0.0f);
  for (long k = 0; k < dim; k += 16) {
    load_matrix_sync(fa, (const float16_t*)(base + j0 * dim + k), (unsigned)dim);
    load_matrix_sync(fb, (const float16_t*)(q + i0 * dim + k), (unsigned)dim);
    mma_sync(fc, fa, fb, fc);
  }
  store_matrix_sync((float32_t*)(out + i0 * tb + j0), fc, (unsigned)tb, mem_col_major);
}

__global__ static void sk_merge_tile_kernel(const float* td, const int* ti,
                                            long tb_off, float* bd,
                                            unsigned* bid, long m, int gd)
{
  const long r = (long)blockIdx.x * blockDim.x + threadIdx.x;
  if (r >= m) { return; }
  float* a_d = bd + r * gd;
  unsigned* a_i = bid + r * gd;
  const float* b_d = td + r * gd;
  const int* b_i = ti + r * gd;
  float od[SK_GEMM_MAX_GD];
  unsigned oi[SK_GEMM_MAX_GD];
  int ia = 0, ib = 0;
  for (int k = 0; k < gd; k++) {
    if (ia < gd && (ib >= gd || a_d[ia] <= b_d[ib])) {
      od[k] = a_d[ia]; oi[k] = a_i[ia]; ia++;
    } else {
      od[k] = b_d[ib]; oi[k] = (unsigned)(b_i[ib] + tb_off); ib++;
    }
  }
  for (int k = 0; k < gd; k++) { a_d[k] = od[k]; a_i[k] = oi[k]; }
}

/* First tile: just copy. */
__global__ static void sk_copy_tile_kernel(const float* td, const int* ti,
                                           long tb_off, float* bd,
                                           unsigned* bid, long m, int gd)
{
  const long r = (long)blockIdx.x * blockDim.x + threadIdx.x;
  if (r >= m) { return; }
  for (int k = 0; k < gd; k++) {
    bd[r * gd + k] = td[r * gd + k];
    bid[r * gd + k] = (unsigned)(ti[r * gd + k] + tb_off);
  }
}

static int sk_select_topk(raft::resources& res, const float* in, long m,
                          long cols, long k, float* out_v, int* out_i,
                          raft::matrix::SelectAlgo algo)
{
  try {
    auto in_val = raft::make_device_matrix_view<const float, int64_t>(
        in, (int64_t)m, (int64_t)cols);
    auto ov = raft::make_device_matrix_view<float, int64_t>(
        out_v, (int64_t)m, (int64_t)k);
    auto oi = raft::make_device_matrix_view<int, int64_t>(
        out_i, (int64_t)m, (int64_t)k);
    cuvs::selection::select_k(res, in_val, std::nullopt, ov, oi, true, true,
                              algo, std::nullopt);
  } catch (...) {
    return 1;
  }
  return 0;
}

/* SEEKDB_CUVS_GEMM_SELALGO: 0=kAuto 1=radix8 2=radix11 3=radix11+extra 4=warpAuto */
static int env_sel_algo = 0;
static int g_use_wmma = 1;

static raft::matrix::SelectAlgo sk_env_algo(void)
{
  switch (env_sel_algo) {
    case 1: return raft::matrix::SelectAlgo::kRadix8bits;
    case 2: return raft::matrix::SelectAlgo::kRadix11bits;
    case 3: return raft::matrix::SelectAlgo::kRadix11bitsExtraPass;
    case 4: return raft::matrix::SelectAlgo::kWarpAuto;
    default: return raft::matrix::SelectAlgo::kAuto;
  }
}

extern "C" int sk_bf_gemm_topk128(void* res_p, const void* d_base, long n,
                                  long dim, long gd, float* host_d,
                                  unsigned* host_id, long tile_q, long tile_b)
{
  if (gd <= 0 || gd > SK_GEMM_MAX_GD) { return 2; }
  if (tile_q <= 0 || tile_b <= 0) { return 2; }
  raft::resources& res = *reinterpret_cast<raft::resources*>(res_p);
  rocblas_handle blas = sk_get_blas();
  if (blas == NULL) { return 1; }

  /* GEMM distances carry ~1e-7 relative noise, enough to shuffle the last few
   * candidates around the k boundary. Selecting a slightly larger pool and
   * handing the bridge the top-gd of the merged list restores the exact-kNN
   * recall at negligible cost. */
  const long ksel = (gd + 32 <= SK_GEMM_MAX_GD) ? (gd + 32) : gd;

  {
    const char* sa = getenv("SEEKDB_CUVS_GEMM_SELALGO");
    if (sa != NULL && sa[0] != '\0') { env_sel_algo = atoi(sa); }
    const char* wm = getenv("SEEKDB_CUVS_GEMM_WMA");
    g_use_wmma = (wm == NULL || wm[0] != '0');
  }

  const int do_time = (getenv("SEEKDB_CUVS_TIME") != NULL);
  hipEvent_t ev_g0 = NULL, ev_g1 = NULL, ev_s1 = NULL;
  if (do_time) {
    hipEventCreate(&ev_g0); hipEventCreate(&ev_g1); hipEventCreate(&ev_s1);
  }
  float* d_normb = NULL;
  float* d_scratch = NULL;
  float* d_best_d = NULL;
  unsigned* d_best_i = NULL;
  float* d_tdist = NULL;
  int* d_tidx = NULL;
  int rc = 1;

  SK_HIP_CHECK(hipMalloc(&d_normb, sizeof(float) * (size_t)n));
  sk_row_norm2_kernel<<<dim3((unsigned)n), 256, 0, 0>>>(
      (const unsigned short*)d_base, d_normb, n, dim);
  SK_HIP_CHECK(hipGetLastError());

  SK_HIP_CHECK(hipMalloc(&d_scratch, sizeof(float) * (size_t)tile_q * tile_b));
  SK_HIP_CHECK(hipMalloc(&d_best_d, sizeof(float) * (size_t)tile_q * ksel));
  SK_HIP_CHECK(hipMalloc(&d_best_i, sizeof(unsigned) * (size_t)tile_q * ksel));
  SK_HIP_CHECK(hipMalloc(&d_tdist, sizeof(float) * (size_t)tile_q * ksel));
  SK_HIP_CHECK(hipMalloc(&d_tidx, sizeof(int) * (size_t)tile_q * ksel));

  const float alpha = -2.0f;
  const float beta = 0.0f;
  if (do_time) { hipEventRecord(ev_g0, 0); }

  for (long qo = 0; qo < n; qo += tile_q) {
    const long m = (n - qo < tile_q) ? (n - qo) : tile_q;
    for (long bo = 0; bo < n; bo += tile_b) {
      const long tb = (n - bo < tile_b) ? (n - bo) : tile_b;
      /* dist = -2 * base^T @ query, C stored row-major [m x tb] */
      if (g_use_wmma && (tb % 64) == 0 && (m % 64) == 0) {
        dim3 g((unsigned)(tb / 64), (unsigned)(m / 64));
        sk_wmma_gemm_kernel<<<g, 512, 0, 0>>>(
            (const unsigned short*)d_base + bo * dim,
            (const unsigned short*)d_base + qo * dim,
            d_scratch, m, tb, dim);
        SK_HIP_CHECK(hipGetLastError());
        sk_add_normb_kernel<<<4096, 256, 0, 0>>>(d_scratch, d_normb,
                                                 m * tb, tb, -2);
      } else {
        const rocblas_status gs = rocblas_gemm_ex(
            blas, rocblas_operation_transpose, rocblas_operation_none,
            (rocblas_int)tb, (rocblas_int)m, (rocblas_int)dim, &alpha,
            (const unsigned short*)d_base + (size_t)bo * dim,
            rocblas_datatype_f16_r, (rocblas_int)dim,
            (const unsigned short*)d_base + (size_t)qo * dim,
            rocblas_datatype_f16_r, (rocblas_int)dim, &beta,
            d_scratch, rocblas_datatype_f32_r, (rocblas_int)tb,
            d_scratch, rocblas_datatype_f32_r, (rocblas_int)tb,
            rocblas_datatype_f32_r, rocblas_gemm_algo_standard, 0, 0);
        if (gs != rocblas_status_success) {
          fprintf(stderr, "[seekdb_bf_gemm] rocblas_gemm_ex failed: %d\n", (int)gs);
          goto done;
        }
        sk_add_normb_kernel<<<4096, 256, 0, 0>>>(d_scratch, d_normb,
                                                 m * tb, tb, 1);
      }
      SK_HIP_CHECK(hipGetLastError());
      if (sk_select_topk(res, d_scratch, m, tb, ksel, d_tdist, d_tidx,
                         sk_env_algo()) != 0) {
        fprintf(stderr, "[seekdb_bf_gemm] select_k threw\n");
        goto done;
      }
      if (bo == 0) {
        sk_copy_tile_kernel<<<(unsigned)((m + 255) / 256), 256, 0, 0>>>(
            d_tdist, d_tidx, bo, d_best_d, d_best_i, m, ksel);
      } else {
        sk_merge_tile_kernel<<<(unsigned)((m + 255) / 256), 256, 0, 0>>>(
            d_tdist, d_tidx, bo, d_best_d, d_best_i, m, ksel);
      }
      SK_HIP_CHECK(hipGetLastError());
    }
    SK_HIP_CHECK(hipMemcpy2D(host_d + (size_t)qo * gd, sizeof(float) * gd,
                             d_best_d, sizeof(float) * ksel,
                             sizeof(float) * gd, (size_t)m, hipMemcpyDeviceToHost));
    SK_HIP_CHECK(hipMemcpy2D(host_id + (size_t)qo * gd, sizeof(unsigned) * gd,
                             d_best_i, sizeof(unsigned) * ksel,
                             sizeof(unsigned) * gd, (size_t)m, hipMemcpyDeviceToHost));
  }
  rc = 0;
done:
  if (do_time) {
    hipEventRecord(ev_g1, 0);
    hipEventSynchronize(ev_g1);
    float gms = 0.f, sms = 0.f, wall = 0.f;
    hipEventElapsedTime(&gms, ev_g0, ev_g1);
    hipEventRecord(ev_s1, 0);
    hipEventSynchronize(ev_s1);
    hipEventElapsedTime(&sms, ev_g1, ev_s1);
    fprintf(stderr, "[seekdb_bf_gemm] n=%ld dim=%ld gd=%ld ksel=%ld tiles %ldx%ld "
                    "algo=%d wma=%d gemm+norm=%.0fms select+merge=%.0fms rc=%d\n",
            n, dim, gd, ksel, tile_q, tile_b, env_sel_algo, g_use_wmma, gms, sms, rc);
  }
  if (ev_g0 != NULL) { hipEventDestroy(ev_g0); }
  if (ev_g1 != NULL) { hipEventDestroy(ev_g1); }
  if (ev_s1 != NULL) { hipEventDestroy(ev_s1); }
  if (d_normb != NULL) { hipFree(d_normb); }
  if (d_scratch != NULL) { hipFree(d_scratch); }
  if (d_best_d != NULL) { hipFree(d_best_d); }
  if (d_best_i != NULL) { hipFree(d_best_i); }
  if (d_tdist != NULL) { hipFree(d_tdist); }
  if (d_tidx != NULL) { hipFree(d_tidx); }
  return rc;
}
