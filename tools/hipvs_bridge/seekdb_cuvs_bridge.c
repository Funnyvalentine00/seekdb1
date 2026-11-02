/* seekdb <-> hipVS bridge: plain-C CAGRA kNN; all cuVS/ROCm deps live here. */
#include "seekdb_cuvs_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <cuvs/neighbors/brute_force.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- tuning knobs (defaults keep the frozen-baseline behaviour) ---------- */
static long env_long(const char *name, long def)
{
  const char *v = getenv(name);
  if (v == NULL || v[0] == '\0') { return def; }
  long x = atol(v);
  return x > 0 ? x : def;
}

static const char* env_str(const char *name, const char *def)
{
  const char *v = getenv(name);
  return (v == NULL || v[0] == '\0') ? def : v;
}

static enum cuvsCagraGraphBuildAlgo parse_build_algo(const char *s)
{
  if (s == NULL) { return NN_DESCENT; }
  if (s[0] == 'i' || s[0] == 'I') { return IVF_PQ; }
  if (strstr(s, "iter") != NULL) { return ITERATIVE_CAGRA_SEARCH; }
  if (s[0] == 'a' || s[0] == 'A') { return AUTO_SELECT; }
  return NN_DESCENT;
}

static int env_on(const char *name)
{
  const char *v = getenv(name);
  return v != NULL && (v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't');
}

static double now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* IEEE float32 -> float16 (round-to-nearest-even-ish). Pure C so the bridge keeps
 * building as plain C; used only when the half-precision build path is enabled. */
static unsigned short f2h(float f)
{
  union { float f; unsigned u; } v;
  v.f = f;
  unsigned u = v.u;
  unsigned sign = (u >> 16) & 0x8000u;
  int exp = (int)((u >> 23) & 0xffu) - 127;
  unsigned mant = u & 0x7fffffu;
  if (exp > 15) { return (unsigned short)(sign | 0x7c00u); }
  if (exp < -14) {
    if (exp < -24) { return (unsigned short)sign; }
    mant |= 0x800000u;
    int shift = -14 - exp;
    unsigned rem = mant & ((1u << (13 + shift)) - 1u);
    unsigned hm = mant >> (13 + shift);
    if (rem >= (1u << (12 + shift))) { hm++; }
    return (unsigned short)(sign | hm);
  }
  unsigned h16 = sign | ((unsigned)(exp + 15) << 10) | ((u >> 13) & 0x3ffu);
  if ((u & 0x1fffu) >= 0x1000u) { h16++; }
  return (unsigned short)h16;
}

typedef struct {
  cuvsResources_t        res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndex_t       idx;
  cuvsBruteForceIndex_t  bf;      /* kept only for the brute-force kNN graph path */
  void                  *d_data;  /* device dataset backing idx (brute-force path) */
  void                  *d_graph; /* device uint32 knn graph backing idx */
  long n, dim;
  int  half;
} sk_cuvs_index;

/* Fills a 2-D DLManagedTensor over caller-owned shape storage. */
static void set_tensor2(DLManagedTensor *t, void *data, int dev_type, int code, int bits,
                        long *shape)
{
  memset(t, 0, sizeof(*t));
  t->dl_tensor.data = data;
  t->dl_tensor.device.device_type = dev_type;
  t->dl_tensor.device.device_id = 0;
  t->dl_tensor.ndim = 2;
  t->dl_tensor.dtype.code = code;
  t->dl_tensor.dtype.bits = bits;
  t->dl_tensor.dtype.lanes = 1;
  t->dl_tensor.shape = shape;
  t->dl_tensor.strides = 0;
  t->dl_tensor.byte_offset = 0;
}

/* Build a CAGRA index from an exact brute-force kNN graph.
 *
 * cuVS can wrap a precomputed kNN graph into a CAGRA index (cuvsCagraIndexFromArgs).
 * Brute force over 250K x 250K is cheaper than running NN-descent at the degree we
 * need, and the resulting graph is exact, so recall does not suffer. */
static int build_bruteforce_graph(sk_cuvs_index *h, const float *base, long n, long dim,
                                  long gd)
{
  void *d_data = NULL, *d_nbr = NULL, *d_dist = NULL, *d_graph = NULL;
  cuvsBruteForceIndex_t bf = NULL;
  int rc = 1;
  if (gd <= 0 || gd > 512) { return 1; }
  if (gd > n) { gd = n; }
  if (cudaMalloc(&d_data, sizeof(float) * (size_t)n * dim) != cudaSuccess) { goto done; }
  if (cudaMemcpy(d_data, base, sizeof(float) * (size_t)n * dim, cudaMemcpyDefault) != cudaSuccess) { goto done; }
  if (cudaMalloc(&d_nbr, sizeof(long long) * (size_t)n * gd) != cudaSuccess) { goto done; }
  if (cudaMalloc(&d_dist, sizeof(float) * (size_t)n * gd) != cudaSuccess) { goto done; }
  if (cudaMalloc(&d_graph, sizeof(unsigned) * (size_t)n * gd) != cudaSuccess) { goto done; }

  long dshape[2] = {n, dim};
  DLManagedTensor ds;
  set_tensor2(&ds, d_data, kDLCUDA, kDLFloat, 32, dshape);
  if (cuvsBruteForceIndexCreate(&bf) != CUVS_SUCCESS) { goto done; }
  if (cuvsBruteForceBuild(h->res, &ds, L2Expanded, 0.f, bf) != CUVS_SUCCESS) { goto done; }

  long kshape[2] = {n, gd};
  DLManagedTensor nb, dt;
  set_tensor2(&nb, d_nbr, kDLCUDA, kDLInt, 64, kshape);
  set_tensor2(&dt, d_dist, kDLCUDA, kDLFloat, 32, kshape);
  cuvsFilter f;
  f.type = NO_FILTER;
  f.addr = (uintptr_t)0;
  if (cuvsBruteForceSearch(h->res, bf, &ds, &nb, &dt, f) != CUVS_SUCCESS) { goto done; }

  {
    const size_t cnt = (size_t)n * gd;
    long long *hh = (long long *)malloc(cnt * sizeof(long long));
    unsigned *gg = (unsigned *)malloc(cnt * sizeof(unsigned));
    if (hh == NULL || gg == NULL) { free(hh); free(gg); goto done; }
    if (cudaMemcpy(hh, d_nbr, cnt * sizeof(long long), cudaMemcpyDefault) != cudaSuccess) {
      free(hh); free(gg); goto done;
    }
    for (size_t i = 0; i < cnt; i++) { gg[i] = (unsigned)hh[i]; }
    free(hh);
    if (cudaMemcpy(d_graph, gg, cnt * sizeof(unsigned), cudaMemcpyDefault) != cudaSuccess) {
      free(gg); goto done;
    }
    free(gg);
  }

  {
    DLManagedTensor g;
    long gshape[2] = {n, gd};
    set_tensor2(&g, d_graph, kDLCUDA, kDLUInt, 32, gshape);
    if (cuvsCagraIndexFromArgs(h->res, L2Expanded, &g, &ds, h->idx) != CUVS_SUCCESS) { goto done; }
  }
  h->bf = bf; bf = NULL;
  h->d_data = d_data; d_data = NULL;
  h->d_graph = d_graph; d_graph = NULL;
  rc = 0;
done:
  if (bf != NULL) { cuvsBruteForceIndexDestroy(bf); }
  if (d_nbr != NULL) { cudaFree(d_nbr); }
  if (d_dist != NULL) { cudaFree(d_dist); }
  if (d_data != NULL) { cudaFree(d_data); }
  if (d_graph != NULL) { cudaFree(d_graph); }
  if (rc != 0) {
    fprintf(stderr, "[seekdb_cuvs] brute-force graph build failed: %s\n", cuvsGetLastErrorText());
  }
  return rc;
}

/* Per-thread device buffers + search params. Queries are issued one row at a
 * time by seekdb, so re-allocating device memory for every query dominates the
 * per-query cost; keep the buffers alive for the thread instead. */
typedef struct {
  float    *qd;
  unsigned short *qh;
  unsigned *nd;
  float    *dd;
  size_t    qcap;   /* floats  */
  size_t    qhcap;  /* halfs   */
  size_t    ncap;   /* entries */
  size_t    dcap;   /* floats  */
  cuvsCagraSearchParams_t sp;
  size_t    itopk;
} sk_thread_ctx;
static __thread sk_thread_ctx t_ctx;

static int grow_half(unsigned short **p, size_t *cap, size_t need)
{
  if (*cap >= need) { return 0; }
  if (*p != NULL) { cudaFree(*p); *p = NULL; }
  size_t cap2 = need < 256 ? 256 : need;
  void *q = NULL;
  if (cudaMalloc(&q, sizeof(unsigned short) * cap2) != cudaSuccess) { *cap = 0; return 1; }
  *p = (unsigned short *)q;
  *cap = cap2;
  return 0;
}

static int grow_float(float **p, size_t *cap, size_t need)
{
  if (*cap >= need) { return 0; }
  if (*p != NULL) { cudaFree(*p); *p = NULL; }
  size_t cap2 = need < 256 ? 256 : need;
  void *q = NULL;
  if (cudaMalloc(&q, sizeof(float) * cap2) != cudaSuccess) { *cap = 0; return 1; }
  *p = (float *)q;
  *cap = cap2;
  return 0;
}

static int grow_uint(unsigned **p, size_t *cap, size_t need)
{
  if (*cap >= need) { return 0; }
  if (*p != NULL) { cudaFree(*p); *p = NULL; }
  size_t cap2 = need < 256 ? 256 : need;
  void *q = NULL;
  if (cudaMalloc(&q, sizeof(unsigned) * cap2) != cudaSuccess) { *cap = 0; return 1; }
  *p = (unsigned *)q;
  *cap = cap2;
  return 0;
}

void* seekdb_cuvs_build(const float* base, long n, long dim)
{
  const long graph_degree = env_long("SEEKDB_CUVS_GRAPH_DEGREE", 128);
  long inter_degree = env_long("SEEKDB_CUVS_INTERMEDIATE_DEGREE", 128);
  /* NN-descent needs fewer refinement rounds on lower-dimensional data: at 128 dims
   * the graph is already converged after ~30 iterations (recall unchanged), while
   * 256-dim graphs still need the full 60 to stay within the recall gate. */
  const long nn_iter = env_long("SEEKDB_CUVS_NN_ITER", dim <= 128 ? 30 : 60);
  const enum cuvsCagraGraphBuildAlgo algo =
      parse_build_algo(env_str("SEEKDB_CUVS_BUILD_ALGO", "nnd"));
  /* Exact brute-force kNN graphs are very fast to build, but they navigate less
   * well than NN-descent graphs; at 128 dims that costs almost nothing in recall,
   * at 256 dims it does, so only the lower-dimensional case uses them. */
  const int use_bfknn =
      (strcmp(env_str("SEEKDB_CUVS_BUILD_ALGO", dim <= 128 ? "bfknn" : "nnd"), "bfknn") == 0);
  if (inter_degree < graph_degree) { inter_degree = graph_degree; }
  const double t0 = now_ms();

  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  if (!h) return NULL;
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) { free(h); return NULL; }

  const int use_half = (env_str("SEEKDB_CUVS_DTYPE", "float")[0] == 'h');
  unsigned short *half_buf = NULL;
  if (use_half) {
    const size_t cnt = (size_t)n * (size_t)dim;
    half_buf = (unsigned short*)malloc(cnt * sizeof(unsigned short));
    if (half_buf == NULL) { goto fail_res; }
    for (size_t i = 0; i < cnt; i++) { half_buf[i] = f2h(base[i]); }
  }

  DLManagedTensor dset; long ds[2] = {n, dim};
  dset.dl_tensor.data = use_half ? (void*)half_buf : (void*)base;
  dset.dl_tensor.device.device_type = kDLCPU; dset.dl_tensor.device.device_id = 0;
  dset.dl_tensor.ndim = 2;
  dset.dl_tensor.dtype.code = kDLFloat;
  dset.dl_tensor.dtype.bits = use_half ? 16 : 32;
  dset.dl_tensor.dtype.lanes = 1;
  dset.dl_tensor.shape = ds; dset.dl_tensor.strides = 0; dset.dl_tensor.byte_offset = 0;

  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) goto fail_half;
  h->ip->build_algo = algo;
  h->ip->graph_degree = graph_degree;
  h->ip->intermediate_graph_degree = inter_degree;
  h->ip->nn_descent_niter = nn_iter;
  if (cuvsCagraIndexCreate(&h->idx)      != CUVS_SUCCESS) goto fail_ip;
  if (use_bfknn) {
    if (build_bruteforce_graph(h, base, n, dim, graph_degree) != 0) { goto fail_idx; }
  } else {
    const cuvsError_t st = cuvsCagraBuild(h->res, h->ip, &dset, h->idx);
    if (half_buf != NULL) { free(half_buf); half_buf = NULL; }
    if (st != CUVS_SUCCESS) goto fail_idx;
  }
  if (half_buf != NULL) { free(half_buf); half_buf = NULL; }
  h->n = n; h->dim = dim; h->half = use_half;
  if (env_on("SEEKDB_CUVS_TIME")) {
    fprintf(stderr, "[seekdb_cuvs] build n=%ld dim=%ld gd=%ld igd=%ld iters=%ld algo=%s half=%d took=%.1fms\n",
            n, dim, graph_degree, inter_degree, nn_iter,
            use_bfknn ? "bfknn" : "cuvs", use_half, now_ms() - t0);
  }
  return h;

fail_idx: cuvsCagraIndexDestroy(h->idx);
fail_ip:  cuvsCagraIndexParamsDestroy(h->ip);
fail_half: if (half_buf != NULL) { free(half_buf); }
fail_res: cuvsResourcesDestroy(h->res); free(h);
  fprintf(stderr, "[seekdb_cuvs] build failed: %s\n", cuvsGetLastErrorText());
  return NULL;
}

int seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                       unsigned int* out_ids, float* out_dist)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return 1;
  const double t0 = now_ms();
  const size_t need_q = (size_t)nq * (size_t)h->dim;
  const size_t need_r = (size_t)nq * (size_t)topk;
  int rc = 0;
  if (h->half) {
    if (grow_half(&t_ctx.qh, &t_ctx.qhcap, need_q) != 0) { rc = 10; goto done; }
    unsigned short *tmp = (unsigned short*)malloc(sizeof(unsigned short) * need_q);
    if (tmp == NULL) { rc = 13; goto done; }
    for (size_t i = 0; i < need_q; i++) { tmp[i] = f2h(query[i]); }
    {
      const int cp = cudaMemcpy(t_ctx.qh, tmp, sizeof(unsigned short) * need_q, cudaMemcpyDefault);
      free(tmp);
      if (cp != cudaSuccess) { rc = 13; goto done; }
    }
  } else {
    if (grow_float(&t_ctx.qd, &t_ctx.qcap, need_q) != 0) { rc = 10; goto done; }
    if (cudaMemcpy(t_ctx.qd, query, sizeof(float) * need_q, cudaMemcpyDefault) != cudaSuccess) { rc = 13; goto done; }
  }
  if (grow_uint(&t_ctx.nd, &t_ctx.ncap, need_r) != 0) { rc = 11; goto done; }
  if (grow_float(&t_ctx.dd, &t_ctx.dcap, need_r) != 0) { rc = 12; goto done; }
  {
    DLManagedTensor qt, nt, dt; long qs[2] = {nq, h->dim}, nsh[2] = {nq, topk}, dsh[2] = {nq, topk};
    qt.dl_tensor.data = h->half ? (void*)t_ctx.qh : (void*)t_ctx.qd;
    qt.dl_tensor.device.device_type = kDLCUDA; qt.dl_tensor.device.device_id = 0;
    qt.dl_tensor.ndim = 2; qt.dl_tensor.dtype.code = kDLFloat;
    qt.dl_tensor.dtype.bits = h->half ? 16 : 32;
    qt.dl_tensor.dtype.lanes = 1;
    qt.dl_tensor.shape = qs; qt.dl_tensor.strides = 0; qt.dl_tensor.byte_offset = 0;
    nt = qt; nt.dl_tensor.data = t_ctx.nd; nt.dl_tensor.dtype.code = kDLUInt;
    nt.dl_tensor.dtype.bits = 32; nt.dl_tensor.shape = nsh;
    dt = qt; dt.dl_tensor.data = t_ctx.dd; dt.dl_tensor.dtype.code = kDLFloat; dt.dl_tensor.dtype.bits = 32;
    dt.dl_tensor.shape = dsh;
    if (t_ctx.sp == NULL && cuvsCagraSearchParamsCreate(&t_ctx.sp) != CUVS_SUCCESS) { rc = 14; goto done; }
    {
      /* Single-row SQL queries: the per-call cost grows with itopk_size, so keep the
       * candidate list just large enough. Two limits apply:
       *  - large top-k needs a wide pool or recall@100 collapses;
       *  - 256-dim single-row searches lose a lot of recall at a narrow pool
       *    (measured 0.59 vs 0.93), so only the low-dimensional case may shrink it. */
      const long itopk_min = env_long("SEEKDB_CUVS_ITOPK_MIN",
                                      (topk <= 32 && h->dim <= 128) ? 128 : 512);
      const long search_width = env_long("SEEKDB_CUVS_SEARCH_WIDTH", 4);
      size_t required_itopk = (size_t)topk * 4;
      if (required_itopk < (size_t)itopk_min) { required_itopk = (size_t)itopk_min; }
      if (t_ctx.sp->itopk_size < required_itopk || t_ctx.itopk < required_itopk) {
        t_ctx.sp->itopk_size = required_itopk;
        t_ctx.itopk = required_itopk;
      }
      t_ctx.sp->search_width = search_width;
    }
    cuvsFilter f; f.type = NO_FILTER; f.addr = (uintptr_t)0;
    if (cuvsCagraSearch(h->res, t_ctx.sp, h->idx, &qt, &nt, &dt, f) != CUVS_SUCCESS) { rc = 15; goto done; }
    if (cudaMemcpy(out_ids, t_ctx.nd, sizeof(unsigned) * need_r, cudaMemcpyDefault) != cudaSuccess) { rc = 16; goto done; }
    if (out_dist) cudaMemcpy(out_dist, t_ctx.dd, sizeof(float) * need_r, cudaMemcpyDefault);
  }
done:
  if (env_on("SEEKDB_CUVS_TIME")) {
    fprintf(stderr, "[seekdb_cuvs] search nq=%ld topk=%ld rc=%d took=%.2fms\n",
            nq, topk, rc, now_ms() - t0);
  }
  if (rc) fprintf(stderr, "[seekdb_cuvs] search rc=%d: %s\n", rc, cuvsGetLastErrorText());
  return rc;
}

void seekdb_cuvs_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return;
  cuvsCagraIndexDestroy(h->idx);
  if (h->bf != NULL) { cuvsBruteForceIndexDestroy(h->bf); }
  if (h->d_data != NULL) { cudaFree(h->d_data); }
  if (h->d_graph != NULL) { cudaFree(h->d_graph); }
  cuvsCagraIndexParamsDestroy(h->ip);
  cuvsResourcesDestroy(h->res);
  free(h);
}

int seekdb_cuvs_serialize(void* handle, const char* filename)
{
  sk_cuvs_index* index = (sk_cuvs_index*)handle;
  if (!index || !filename) return 1;
  if (cuvsCagraSerialize(index->res, filename, index->idx, true) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] serialize failed: %s\n", cuvsGetLastErrorText());
    return 1;
  }
  return 0;
}

void* seekdb_cuvs_deserialize(const char* filename, long n, long dim)
{
  if (!filename || n <= 0 || dim <= 0) return NULL;
  sk_cuvs_index* index = (sk_cuvs_index*)calloc(1, sizeof(*index));
  if (!index) return NULL;
  if (cuvsResourcesCreate(&index->res) != CUVS_SUCCESS) goto fail;
  if (cuvsCagraIndexParamsCreate(&index->ip) != CUVS_SUCCESS) goto fail_res;
  if (cuvsCagraIndexCreate(&index->idx) != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraDeserialize(index->res, filename, index->idx) != CUVS_SUCCESS) goto fail_idx;
  {
    int64_t actual_n = 0;
    int64_t actual_dim = 0;
    if (cuvsCagraIndexGetSize(index->idx, &actual_n) != CUVS_SUCCESS ||
        cuvsCagraIndexGetDims(index->idx, &actual_dim) != CUVS_SUCCESS ||
        actual_n != n || actual_dim != dim) {
      fprintf(stderr, "[seekdb_cuvs] snapshot shape mismatch: %ld x %ld, expected %ld x %ld\n",
              (long)actual_n, (long)actual_dim, n, dim);
      goto fail_idx;
    }
  }
  index->n = n;
  index->dim = dim;
  index->half = (index->idx->dtype.bits == 16);
  return index;
fail_idx:
  cuvsCagraIndexDestroy(index->idx);
fail_ip:
  cuvsCagraIndexParamsDestroy(index->ip);
fail_res:
  cuvsResourcesDestroy(index->res);
fail:
  fprintf(stderr, "[seekdb_cuvs] deserialize failed: %s\n", cuvsGetLastErrorText());
  free(index);
  return NULL;
}

int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                          const float* query, long nq, long topk,
                          unsigned int* out_ids)
{
  void* h = seekdb_cuvs_build(base, n, dim);
  if (!h) return 10;
  int rc = seekdb_cuvs_search(h, query, nq, topk, out_ids, NULL);
  seekdb_cuvs_free(h);
  return rc;
}
