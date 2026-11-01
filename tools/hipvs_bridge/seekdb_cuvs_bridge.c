/* seekdb <-> hipVS bridge: plain-C CAGRA kNN; all cuVS/ROCm deps live here. */
#include "seekdb_cuvs_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
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

typedef struct {
  cuvsResources_t        res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndex_t       idx;
  long n, dim;
} sk_cuvs_index;

/* Per-thread device buffers + search params. Queries are issued one row at a
 * time by seekdb, so re-allocating device memory for every query dominates the
 * per-query cost; keep the buffers alive for the thread instead. */
typedef struct {
  float    *qd;
  unsigned *nd;
  float    *dd;
  size_t    qcap;   /* floats  */
  size_t    ncap;   /* entries */
  size_t    dcap;   /* floats  */
  cuvsCagraSearchParams_t sp;
  size_t    itopk;
} sk_thread_ctx;
static __thread sk_thread_ctx t_ctx;

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
  const long nn_iter = env_long("SEEKDB_CUVS_NN_ITER", 60);
  if (inter_degree < graph_degree) { inter_degree = graph_degree; }
  const double t0 = now_ms();

  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  if (!h) return NULL;
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) { free(h); return NULL; }

  DLManagedTensor dset; long ds[2] = {n, dim};
  dset.dl_tensor.data = (void*)base;
  dset.dl_tensor.device.device_type = kDLCPU; dset.dl_tensor.device.device_id = 0;
  dset.dl_tensor.ndim = 2;
  dset.dl_tensor.dtype.code = kDLFloat; dset.dl_tensor.dtype.bits = 32; dset.dl_tensor.dtype.lanes = 1;
  dset.dl_tensor.shape = ds; dset.dl_tensor.strides = 0; dset.dl_tensor.byte_offset = 0;

  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) goto fail_res;
  h->ip->build_algo = NN_DESCENT;
  h->ip->graph_degree = graph_degree;
  h->ip->intermediate_graph_degree = inter_degree;
  h->ip->nn_descent_niter = nn_iter;
  if (cuvsCagraIndexCreate(&h->idx)      != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraBuild(h->res, h->ip, &dset, h->idx) != CUVS_SUCCESS) goto fail_idx;
  h->n = n; h->dim = dim;
  if (env_on("SEEKDB_CUVS_TIME")) {
    fprintf(stderr, "[seekdb_cuvs] build n=%ld dim=%ld gd=%ld igd=%ld iters=%ld took=%.1fms\n",
            n, dim, graph_degree, inter_degree, nn_iter, now_ms() - t0);
  }
  return h;

fail_idx: cuvsCagraIndexDestroy(h->idx);
fail_ip:  cuvsCagraIndexParamsDestroy(h->ip);
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
  if (grow_float(&t_ctx.qd, &t_ctx.qcap, need_q) != 0) { rc = 10; goto done; }
  if (grow_uint(&t_ctx.nd, &t_ctx.ncap, need_r) != 0) { rc = 11; goto done; }
  if (grow_float(&t_ctx.dd, &t_ctx.dcap, need_r) != 0) { rc = 12; goto done; }
  if (cudaMemcpy(t_ctx.qd, query, sizeof(float) * need_q, cudaMemcpyDefault) != cudaSuccess) { rc = 13; goto done; }
  {
    DLManagedTensor qt, nt, dt; long qs[2] = {nq, h->dim}, nsh[2] = {nq, topk}, dsh[2] = {nq, topk};
    qt.dl_tensor.data = t_ctx.qd; qt.dl_tensor.device.device_type = kDLCUDA; qt.dl_tensor.device.device_id = 0;
    qt.dl_tensor.ndim = 2; qt.dl_tensor.dtype.code = kDLFloat; qt.dl_tensor.dtype.bits = 32; qt.dl_tensor.dtype.lanes = 1;
    qt.dl_tensor.shape = qs; qt.dl_tensor.strides = 0; qt.dl_tensor.byte_offset = 0;
    nt = qt; nt.dl_tensor.data = t_ctx.nd; nt.dl_tensor.dtype.code = kDLUInt; nt.dl_tensor.shape = nsh;
    dt = qt; dt.dl_tensor.data = t_ctx.dd; dt.dl_tensor.dtype.code = kDLFloat; dt.dl_tensor.shape = dsh;
    if (t_ctx.sp == NULL && cuvsCagraSearchParamsCreate(&t_ctx.sp) != CUVS_SUCCESS) { rc = 14; goto done; }
    {
      const long itopk_min = env_long("SEEKDB_CUVS_ITOPK_MIN", 512);
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
