/* seekdb <-> hipVS bridge, GEMM part: exact-kNN distances via rocBLAS.
 *
 * The 128-D exact-kNN graph build spends its GPU time inside cuVS brute-force
 * search: a plain map_kernel for the distances (15-20% SM efficiency) plus
 * warpsort for the top-k (~56% of the whole build). This file replaces that
 * path with
 *   - one fp16 GEMM per (query-tile x base-tile) block computing
 *     dist = -2<a,b> + ||b||^2 (the query norm is a per-row constant and does
 *     not affect the ordering, so it is restored only at output), accumulated in fp32 by
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
#include <dlfcn.h>
#include <string.h>
#include <optional>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <climits>

#ifdef SK_USE_ROCWMA
#include <rocwmma/rocwmma.hpp>
using namespace rocwmma;
#endif

namespace raft { class resources; }

/* select_k is bound at RUN time, never at link time.
 *
 * The eval build container ships a stripped hipVS: libcuvs_c.so (1.1MB) is
 * present but the C++ library libcuvs.so is 456MB and can be omitted from the
 * build image. Linking -lcuvs there kills the whole bridge build - the eval
 * reports failMsg=3 within ~14s, before seekdb itself is even compiled - while
 * v12, which links only -lcuvs_c, always builds. At run time the GPU container
 * must have libcuvs.so because libcuvs_c.so NEEDs it, so dlopen finds it and
 * dlsym binds the exact mangled symbol. Any failure returns 1 and the caller
 * falls back to the proven cuVS brute-force path. */
typedef void (*sk_select_k_fn)(
  const raft::resources& handle,
  raft::device_matrix_view<const float, int64_t, raft::row_major> in_val,
  std::optional<raft::device_matrix_view<const int, int64_t, raft::row_major>> in_idx,
  raft::device_matrix_view<float, int64_t, raft::row_major> out_val,
  raft::device_matrix_view<int, int64_t, raft::row_major> out_idx,
  bool select_min, bool sorted, raft::matrix::SelectAlgo algo,
  std::optional<raft::device_vector_view<const int, int64_t>> len_i);

/* rocBLAS is bound at run time for the same reason as select_k: the eval
 * build image may ship HIP headers without the librocblas.so dev symlink, and
 * a link-time -lrocblas would fail the whole bridge build. The GPU container
 * always has the library because the driver stack needs it. */
typedef rocblas_status (*sk_rb_create_fn)(rocblas_handle*);
typedef rocblas_status (*sk_rb_destroy_fn)(rocblas_handle);
typedef rocblas_status (*sk_rb_gemm_ex_fn)(
    rocblas_handle, rocblas_operation, rocblas_operation,
    int, int, int, const void*,
    const void*, rocblas_datatype, int,
    const void*, rocblas_datatype, int,
    const void*, const void*, rocblas_datatype, int,
    void*, rocblas_datatype, int,
    rocblas_datatype, rocblas_gemm_algo, int, unsigned int);

static void* g_rb_lib = NULL;
static sk_rb_create_fn  g_rb_create  = NULL;
static sk_rb_destroy_fn g_rb_destroy = NULL;
static sk_rb_gemm_ex_fn g_rb_gemm_ex = NULL;

static int sk_load_rocblas(void)
{
  if (g_rb_gemm_ex != NULL) { return 0; }
  const char* names[] = {"librocblas.so", "librocblas.so.5",
                         "/opt/rocm/lib/librocblas.so",
                         "/opt/rocm/lib/librocblas.so.5"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && g_rb_lib == NULL; i++) {
    g_rb_lib = dlopen(names[i], RTLD_LAZY | RTLD_GLOBAL);
  }
  if (g_rb_lib == NULL) {
    fprintf(stderr, "[seekdb_bf_gemm] rocBLAS unavailable; GEMM path disabled\n");
    return 1;
  }
  g_rb_create  = (sk_rb_create_fn)dlsym(g_rb_lib, "rocblas_create_handle");
  g_rb_destroy = (sk_rb_destroy_fn)dlsym(g_rb_lib, "rocblas_destroy_handle");
  g_rb_gemm_ex = (sk_rb_gemm_ex_fn)dlsym(g_rb_lib, "rocblas_gemm_ex");
  if (g_rb_create == NULL || g_rb_destroy == NULL || g_rb_gemm_ex == NULL) {
    fprintf(stderr, "[seekdb_bf_gemm] rocBLAS symbols missing; GEMM path disabled\n");
    return 1;
  }
  return 0;
}

static sk_select_k_fn g_select_k = NULL;

static int sk_load_select_k(void)
{
  if (g_select_k != NULL) { return 0; }
  void* h = dlopen("libcuvs.so", RTLD_LAZY | RTLD_GLOBAL);
  if (h == NULL) { h = dlopen("/opt/hipvs/lib/libcuvs.so", RTLD_LAZY | RTLD_GLOBAL); }
  if (h == NULL) {
    fprintf(stderr, "[seekdb_bf_gemm] libcuvs.so unavailable; GEMM path disabled\n");
    return 1;
  }
  g_select_k = (sk_select_k_fn)dlsym(h,
      "_ZN4cuvs9selection8select_kERKN4raft9resourcesENSt12experimental6mdspanIKfNS5_7extentsIlJLm18446744073709551615ELm18446744073709551615EEEENS5_12layout_rightENS1_20host_device_accessorINS5_16default_accessorIS7_EELNS1_11memory_typeE2EEEEESt8optionalINS6_IKiS9_SA_NSB_INSC_ISI_EELSE_2EEEEEENS6_IfS9_SA_NSB_INSC_IfEELSE_2EEEEENS6_IiS9_SA_NSB_INSC_IiEELSE_2EEEEEbbNS1_6matrix10SelectAlgoESH_INS6_ISI_NS8_IlJLm18446744073709551615EEEESA_SK_EEE");
  if (g_select_k == NULL) {
    fprintf(stderr, "[seekdb_bf_gemm] select_k not found in libcuvs.so; GEMM path disabled\n");
    return 1;
  }
  return 0;
}

#define SK_GEMM_MAX_GD 128   /* merge kernel keeps candidates in registers */

#define SK_HIP_CHECK(call)                                                   \
  do {                                                                       \
    hipError_t e_ = (call);                                                  \
    if (e_ != hipSuccess) {                                                  \
      fprintf(stderr, "[seekdb_bf_gemm] hip error %d (%s) at %s:%d\n",       \
              (int)e_, hipGetErrorString(e_), __FILE__, __LINE__);           \
      goto done;                                                             \
    }                                                                        \
  } while (0)

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

/* WMMA GEMM: dot products, optionally fused with norm[b]-2*dot, row-major [m x tb].
 * One 64x64 output tile per block (16 warps, 16x16 per warp), fp16 inputs,
 * fp32 accumulate on tensor cores. Tail tiles (not multiples of 64) fall
 * back to rocBLAS in the caller. Only built when rocwmma headers exist. */
#ifdef SK_USE_ROCWMA
template<bool FusedNorm = false>
__global__ __launch_bounds__(512) static void sk_wmma_gemm_kernel(
    const unsigned short* base, const unsigned short* q, float* out,
    long m, long tb, long dim, const float* normb = nullptr)
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
  if constexpr (FusedNorm) {
    // Preserve rocWMMA's accumulator layout through its public store API.
    // The block covers a complete 64x64 tile, so every thread reaches the barrier.
    __shared__ __align__(16) float tile[64 * 64];
    store_matrix_sync(tile + wx * 16 * 64 + wy * 16, fc, 64, mem_col_major);
    __syncthreads();
    for (int k = threadIdx.x; k < 64 * 64; k += blockDim.x) {
      const long b = (long)blockIdx.x * 64 + k % 64;
      const long row = (long)blockIdx.y * 64 + k / 64;
      out[row * tb + b] = normb[b] - 2.0f * tile[k];
    }
  } else {
    store_matrix_sync((float32_t*)(out + i0 * tb + j0), fc, (unsigned)tb, mem_col_major);
  }
}
#endif  /* SK_USE_ROCWMA */

__global__ static void sk_merge_tile_kernel(const float* td, const int* ti,
                                            long tb_off, float* bd,
                                            unsigned* bid, long m, int gd, int tile_k)
{
  const long r = (long)blockIdx.x * blockDim.x + threadIdx.x;
  if (r >= m) { return; }
  float* a_d = bd + r * gd;
  unsigned* a_i = bid + r * gd;
  const float* b_d = td + r * tile_k;
  const int* b_i = ti + r * tile_k;
  float od[SK_GEMM_MAX_GD];
  unsigned oi[SK_GEMM_MAX_GD];
  int ia = 0, ib = 0;
  for (int k = 0; k < gd; k++) {
    if (ia < gd && (ib >= tile_k || a_d[ia] <= b_d[ib])) {
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
                                           unsigned* bid, long m, int gd, int tile_k)
{
  const long r = (long)blockIdx.x * blockDim.x + threadIdx.x;
  if (r >= m) { return; }
  for (int k = 0; k < gd; k++) {
    bd[r * gd + k] = (k < tile_k) ? td[r * tile_k + k] : INFINITY;
    bid[r * gd + k] = (k < tile_k) ? (unsigned)(ti[r * tile_k + k] + tb_off) : UINT_MAX;
  }
}

/* Restore complete squared L2 distances only for the final top-k. The bridge's
 * optional symmetrization compares edges from different query rows, so the
 * omitted query norm must not escape this function. Cost: O(n * gd), not O(n^2). */
__global__ static void sk_restore_norm_kernel(float* d, const float* qnorm,
                                              long m, int gd)
{
  const long r = (long)blockIdx.x * blockDim.x + threadIdx.x;
  if (r >= m) { return; }
  for (int k = 0; k < gd; k++) { d[r * gd + k] += qnorm[r]; }
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
    if (sk_load_select_k() != 0) { return 1; }
    g_select_k(res, in_val, std::nullopt, ov, oi, true, true,
               algo, std::nullopt);
  } catch (...) {
    return 1;
  }
  return 0;
}

/* SEEKDB_CUVS_GEMM_SELALGO: 0=kAuto 1=radix8 2=radix11 3=radix11+extra 4=warpAuto */
static raft::matrix::SelectAlgo sk_env_algo(int env_sel_algo)
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
  if (gd <= 0 || gd > SK_GEMM_MAX_GD || n < gd || dim <= 0 || dim > INT_MAX) { return 2; }
  if (res_p == NULL || d_base == NULL || host_d == NULL || host_id == NULL) { return 2; }
  if (tile_q <= 0 || tile_b <= 0) { return 2; }
  tile_q = (tile_q < n) ? tile_q : n;
  tile_b = (tile_b < n) ? tile_b : n;
  if (tile_q > INT_MAX || tile_b > INT_MAX || n > UINT_MAX) { return 2; }
  raft::resources& res = *reinterpret_cast<raft::resources*>(res_p);
  // A global handle could be destroyed by another concurrent build. Own the
  // handle (and its current-device workspace) for exactly this invocation.
  rocblas_handle blas = NULL;

  /* A row outside a tile's top-gd cannot enter the global top-gd. Keeping
   * gd+32 and then truncating without recomputing distances cannot improve
   * numerical accuracy; it only increases selection, merge and buffer cost. */
  const long ksel = gd;
  const char* sa = getenv("SEEKDB_CUVS_GEMM_SELALGO");
  const int env_sel_algo = (sa != NULL) ? atoi(sa) : 0;
  const raft::matrix::SelectAlgo algo = sk_env_algo(env_sel_algo);
#ifdef SK_USE_ROCWMA
  const char* wm = getenv("SEEKDB_CUVS_GEMM_WMA");
  const bool use_wmma = (wm == NULL || wm[0] != '0') && (dim % 16 == 0);
  const char* fn = getenv("SEEKDB_CUVS_GEMM_FUSED_NORM");
  const bool fused_norm = fn == NULL || fn[0] != '0';
#else
  const bool use_wmma = false;
#endif

  const int do_time = (getenv("SEEKDB_CUVS_TIME") != NULL);
  const auto started = std::chrono::steady_clock::now();
  float* d_normb = NULL;
  float* d_scratch = NULL;
  float* d_best_d = NULL;
  unsigned* d_best_i = NULL;
  float* d_tdist = NULL;
  int* d_tidx = NULL;
  int rc = 1;
  long fused_tiles = 0, wmma_tiles = 0, blas_tiles = 0;
  const float alpha = -2.0f;
  const float beta = 0.0f;

  if (sk_load_rocblas() != 0) { goto done; }
  if (g_rb_create(&blas) != rocblas_status_success) { goto done; }

  SK_HIP_CHECK(hipMalloc(&d_normb, sizeof(float) * (size_t)n));
  sk_row_norm2_kernel<<<dim3((unsigned)n), 256, 0, 0>>>(
      (const unsigned short*)d_base, d_normb, n, dim);
  SK_HIP_CHECK(hipGetLastError());

  SK_HIP_CHECK(hipMalloc(&d_scratch, sizeof(float) * (size_t)tile_q * tile_b));
  SK_HIP_CHECK(hipMalloc(&d_best_d, sizeof(float) * (size_t)tile_q * ksel));
  SK_HIP_CHECK(hipMalloc(&d_best_i, sizeof(unsigned) * (size_t)tile_q * ksel));
  SK_HIP_CHECK(hipMalloc(&d_tdist, sizeof(float) * (size_t)tile_q * ksel));
  SK_HIP_CHECK(hipMalloc(&d_tidx, sizeof(int) * (size_t)tile_q * ksel));

  for (long qo = 0; qo < n; qo += tile_q) {
    const long m = (n - qo < tile_q) ? (n - qo) : tile_q;
    for (long bo = 0; bo < n; bo += tile_b) {
      const long tb = (n - bo < tile_b) ? (n - bo) : tile_b;
      const int tile_k = (tb < ksel) ? (int)tb : (int)ksel;
      /* dist = -2 * base^T @ query, C stored row-major [m x tb] */
#ifdef SK_USE_ROCWMA
      if (use_wmma && (tb % 64) == 0 && (m % 64) == 0) {
        dim3 g((unsigned)(tb / 64), (unsigned)(m / 64));
        hipGetLastError();  /* consume any sticky error before the launch */
        if (fused_norm) {
          sk_wmma_gemm_kernel<true><<<g, 512, 0, 0>>>(
              (const unsigned short*)d_base + bo * dim,
              (const unsigned short*)d_base + qo * dim,
              d_scratch, m, tb, dim, d_normb + bo);
          ++fused_tiles;
        } else {
          sk_wmma_gemm_kernel<<<g, 512, 0, 0>>>(
              (const unsigned short*)d_base + bo * dim,
              (const unsigned short*)d_base + qo * dim,
              d_scratch, m, tb, dim);
          SK_HIP_CHECK(hipGetLastError());
          sk_add_normb_kernel<<<4096, 256, 0, 0>>>(d_scratch, d_normb + bo,
                                                   m * tb, tb, -2);
        }
        ++wmma_tiles;
      } else
#endif
      {
        ++blas_tiles;
        const rocblas_status gs = g_rb_gemm_ex(
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
        sk_add_normb_kernel<<<4096, 256, 0, 0>>>(d_scratch, d_normb + bo,
                                                 m * tb, tb, 1);
      }
      SK_HIP_CHECK(hipGetLastError());
      if (sk_select_topk(res, d_scratch, m, tb, tile_k, d_tdist, d_tidx,
                         algo) != 0) {
        fprintf(stderr, "[seekdb_bf_gemm] select_k threw\n");
        goto done;
      }
      if (bo == 0) {
        sk_copy_tile_kernel<<<(unsigned)((m + 255) / 256), 256, 0, 0>>>(
            d_tdist, d_tidx, bo, d_best_d, d_best_i, m, ksel, tile_k);
      } else {
        sk_merge_tile_kernel<<<(unsigned)((m + 255) / 256), 256, 0, 0>>>(
            d_tdist, d_tidx, bo, d_best_d, d_best_i, m, ksel, tile_k);
      }
      SK_HIP_CHECK(hipGetLastError());
    }
    sk_restore_norm_kernel<<<(unsigned)((m + 255) / 256), 256, 0, 0>>>(
        d_best_d, d_normb + qo, m, ksel);
    SK_HIP_CHECK(hipGetLastError());
    SK_HIP_CHECK(hipMemcpy2D(host_d + (size_t)qo * gd, sizeof(float) * gd,
                             d_best_d, sizeof(float) * ksel,
                             sizeof(float) * gd, (size_t)m, hipMemcpyDeviceToHost));
    SK_HIP_CHECK(hipMemcpy2D(host_id + (size_t)qo * gd, sizeof(unsigned) * gd,
                             d_best_i, sizeof(unsigned) * ksel,
                             sizeof(unsigned) * gd, (size_t)m, hipMemcpyDeviceToHost));
  }
  rc = 0;
done:
  /* release the rocblas handle (and its device workspace pool) right away so
   * the resident VRAM footprint of a built index does not carry the GEMM
   * scratch into the 256-D build that follows in the same process. */
  if (blas != NULL) { g_rb_destroy(blas); }
  if (d_normb != NULL) { hipFree(d_normb); }
  if (d_scratch != NULL) { hipFree(d_scratch); }
  if (d_best_d != NULL) { hipFree(d_best_d); }
  if (d_best_i != NULL) { hipFree(d_best_i); }
  if (d_tdist != NULL) { hipFree(d_tdist); }
  if (d_tidx != NULL) { hipFree(d_tidx); }
  if (do_time) {
    const double wall_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    fprintf(stderr, "[seekdb_bf_gemm] n=%ld dim=%ld gd=%ld ksel=%ld tiles %ldx%ld "
                    "algo=%d wma=%d fused_tiles=%ld wmma_tiles=%ld blas_tiles=%ld total_wall=%.0fms rc=%d\n",
            n, dim, gd, ksel, tile_q, tile_b, env_sel_algo, (int)use_wmma,
            fused_tiles, wmma_tiles, blas_tiles, wall_ms, rc);
  }
  return rc;
}
