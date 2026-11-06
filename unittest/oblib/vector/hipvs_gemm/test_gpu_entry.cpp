// Exercise the exported production GEMM helper with real HIP/rocBLAS/cuVS calls.
#include <hip/hip_runtime.h>
#include <hip/hip_fp16.h>
#include <cuvs/core/c_api.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

extern "C" int sk_bf_gemm_topk128(void*, const void*, long, long, long,
                                  float*, unsigned*, long, long);

#define HIP_CHECK(call) do { auto status = (call); if (status != hipSuccess) { \
  std::fprintf(stderr, "%s: %s\n", #call, hipGetErrorString(status)); return 1; } } while (0)

static int check_shape(long n, long dim, long k, long tq, long tb, int wmma)
{
  std::vector<__half> input(n * dim);
  for (long i = 0; i < n; ++i) {
    for (long c = 0; c < dim; ++c)
      input[i * dim + c] = __float2half(float((i * 7 + c * 11) % 19 - 9));
    input[i * dim] = __float2half(float(i));
  }
  __half* data = nullptr;
  HIP_CHECK(hipMalloc(&data, input.size() * sizeof(__half)));
  HIP_CHECK(hipMemcpy(data, input.data(), input.size() * sizeof(__half), hipMemcpyHostToDevice));
  cuvsResources_t resource = 0;
  if (cuvsResourcesCreate(&resource) != CUVS_SUCCESS) return 2;
  setenv("SEEKDB_CUVS_GEMM_WMA", wmma ? "1" : "0", 1);
  std::vector<float> distances(n * k);
  std::vector<unsigned> ids(n * k);
  int rc = sk_bf_gemm_topk128(reinterpret_cast<void*>(resource), data, n, dim, k,
                             distances.data(), ids.data(), tq, tb);
  HIP_CHECK(hipDeviceSynchronize());
  if (rc) {
    const char* error = cuvsGetLastErrorText();
    std::fprintf(stderr, "GEMM rc=%d n=%ld dim=%ld k=%ld wmma=%d: %s\n", rc, n, dim, k, wmma,
                 error ? error : "no C API error text");
    cuvsResourcesDestroy(resource); hipFree(data); return 3;
  }
  double max_error = 0;
  const double u = 0x1p-24;
  const double gamma = (2 * dim + 3) * u / (1 - (2 * dim + 3) * u);
  int bad_ids = 0, bound_errors = 0, wrong_topk = 0, inversions = 0;
  for (long q = 0; q < n; ++q) {
    std::vector<double> exact(n), magnitudes(n);
    for (long b = 0; b < n; ++b) {
      for (long c = 0; c < dim; ++c) {
        const double x = __half2float(input[q * dim + c]);
        const double y = __half2float(input[b * dim + c]);
        exact[b] += (x - y) * (x - y);
        magnitudes[b] += x * x + y * y + 2 * std::fabs(x * y);
      }
    }
    auto sorted = exact;
    std::sort(sorted.begin(), sorted.end());
    const double boundary = sorted[k - 1];
    std::vector<bool> seen(n);
    double previous = -1;
    for (long j = 0; j < k; ++j) {
      const unsigned id = ids[q * k + j];
      if (id >= unsigned(n) || seen[id]) { ++bad_ids; continue; }
      seen[id] = true;
      const float d = distances[q * k + j];
      const double error = std::fabs(d - exact[id]);
      max_error = std::fmax(max_error, error);
      if (!std::isfinite(d) || error > gamma * magnitudes[id]) ++bound_errors;
      // Integer inputs give exact integer squared distances. Equal-distance
      // IDs can vary; an ID strictly beyond the CPU kth distance cannot pass.
      if (exact[id] > boundary) ++wrong_topk;
      if (exact[id] < previous) ++inversions;
      previous = exact[id];
    }
  }
  if (cuvsResourcesDestroy(resource) != CUVS_SUCCESS) return 2;
  HIP_CHECK(hipFree(data));
  std::printf("n=%ld dim=%ld k=%ld tiles=%ldx%ld wmma=%d max_abs_error=%.9g "
               "bad_ids=%d bound_errors=%d wrong_topk=%d inversions=%d\n",
               n, dim, k, tq, tb, wmma, max_error, bad_ids, bound_errors, wrong_topk, inversions);
  return bad_ids || bound_errors || wrong_topk || inversions;
}

int main()
{
  setenv("SEEKDB_CUVS_TIME", "1", 1);
  for (int fused : {0, 1}) {
    setenv("SEEKDB_CUVS_GEMM_FUSED_NORM", fused ? "1" : "0", 1);
    std::printf("fused_norm=%d\n", fused);
    for (int wmma : {0, 1}) {
    if (check_shape(256, 128, 96, 64, 128, wmma) ||
        check_shape(257, 128, 96, 64, 128, wmma) ||
        check_shape(65, 128, 48, 8, 8, wmma) ||
        check_shape(129, 130, 96, 64, 64, wmma) ||
        check_shape(128, 256, 128, 64, 64, wmma)) return 1;
    }
  }
  std::puts("PASS: full GEMM entry and CPU top-k oracle; runner verifies actual path counters");
  return 0;
}
