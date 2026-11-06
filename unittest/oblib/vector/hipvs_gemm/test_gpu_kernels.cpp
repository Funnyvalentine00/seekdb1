// Standalone validation of the production GPU kernels, without cuVS selection.
// Link with -ffunction-sections -Wl,--gc-sections to discard the unused cuVS entry.
#include "seekdb_bf_gemm.cpp"
#include <vector>
#include <algorithm>
#include <numeric>

#define CHECK(call) do { auto e = (call); if (e != hipSuccess) { \
  std::fprintf(stderr, "%s: %s\n", #call, hipGetErrorString(e)); return 1; } } while (0)

int main()
{
  constexpr long n = 256, dim = 128, m = 64, tb = 128, bo = 128, qo = 64;
  std::vector<__half> input(n * dim);
  for (long i = 0; i < n; ++i) {
    for (long c = 0; c < dim; ++c) {
      input[i * dim + c] = __float2half(float((i * 7 + c * 11) % 19 - 9));
    }
    input[i * dim] = __float2half(float(i));
  }
  __half* data = nullptr;
  float *norm = nullptr, *dist = nullptr;
  CHECK(hipMalloc(&data, input.size() * sizeof(__half)));
  CHECK(hipMalloc(&norm, n * sizeof(float)));
  CHECK(hipMalloc(&dist, m * tb * sizeof(float)));
  CHECK(hipMemcpy(data, input.data(), input.size() * sizeof(__half), hipMemcpyHostToDevice));
  sk_row_norm2_kernel<<<n, 256>>>(reinterpret_cast<unsigned short*>(data), norm, n, dim);
  CHECK(hipGetLastError());
  sk_wmma_gemm_kernel<<<dim3(tb / 64, m / 64), 512>>>(
      reinterpret_cast<unsigned short*>(data) + bo * dim,
      reinterpret_cast<unsigned short*>(data) + qo * dim, dist, m, tb, dim);
  CHECK(hipGetLastError());
  sk_add_normb_kernel<<<16, 256>>>(dist, norm + bo, m * tb, tb, -2);
  CHECK(hipGetLastError());
  sk_restore_norm_kernel<<<1, 256>>>(dist, norm + qo, m, tb);
  CHECK(hipGetLastError());
  CHECK(hipDeviceSynchronize());
  std::vector<float> result(m * tb);
  CHECK(hipMemcpy(result.data(), dist, result.size() * sizeof(float), hipMemcpyDeviceToHost));
  sk_wmma_gemm_kernel<true><<<dim3(tb / 64, m / 64), 512>>>(
      reinterpret_cast<unsigned short*>(data) + bo * dim,
      reinterpret_cast<unsigned short*>(data) + qo * dim, dist, m, tb, dim, norm + bo);
  CHECK(hipGetLastError());
  sk_restore_norm_kernel<<<1, 256>>>(dist, norm + qo, m, tb);
  CHECK(hipGetLastError());
  CHECK(hipDeviceSynchronize());
  std::vector<float> fused(m * tb);
  CHECK(hipMemcpy(fused.data(), dist, fused.size() * sizeof(float), hipMemcpyDeviceToHost));
  for (long k = 0; k < m * tb; ++k) {
    if (fused[k] != result[k]) {
      std::fprintf(stderr, "fused/unfused distance differs at %ld: %.9g / %.9g\n", k, fused[k], result[k]);
      return 5;
    }
  }
  std::puts("PASS: fused WMMA epilogue matches separate epilogue for 8192 distances");
  std::vector<float> norms(n);
  CHECK(hipMemcpy(norms.data(), norm, norms.size() * sizeof(float), hipMemcpyDeviceToHost));
  double max_norm_error = 0, max_distance_error = 0;
  for (long row = 0; row < n; ++row) {
    float expected = 0;
    for (long c = 0; c < dim; ++c) {
      float x = __half2float(input[row * dim + c]); expected += x * x;
    }
    max_norm_error = std::fmax(max_norm_error, std::fabs(norms[row] - expected));
  }
  int mismatches = 0, outside_tolerance = 0, rounding_bound_failures = 0;
  std::vector<float> exact(m * tb);
  // Bound the expanded-L2 calculation by the magnitudes of its operands,
  // rather than the final distance, which can be small after cancellation.
  const double unit_roundoff = 0x1p-24;
  const double gamma = (2 * dim + 3) * unit_roundoff /
                       (1 - (2 * dim + 3) * unit_roundoff);
  for (long q = 0; q < m; ++q) {
    for (long b = 0; b < tb; ++b) {
      float expected = 0;
      double magnitude = 0;
      for (long c = 0; c < dim; ++c) {
        const double x = __half2float(input[(q + qo) * dim + c]);
        const double y = __half2float(input[(b + bo) * dim + c]);
        const float delta = x - y;
        expected += delta * delta;
        magnitude += x * x + y * y + 2 * std::fabs(x * y);
      }
      exact[q * tb + b] = expected;
      if (result[q * tb + b] != expected) {
        if (mismatches < 3) std::fprintf(stderr, "WMMA distance mismatch q=%ld b=%ld got=%f expected=%f\n",
                                        q, b, result[q * tb + b], expected);
        ++mismatches;
        max_distance_error = std::fmax(max_distance_error, std::fabs(result[q * tb + b] - expected));
      }
      const float tolerance = 4 * 1.1920928955078125e-7f * std::fmax(1.f, expected);
      if (!std::isfinite(result[q * tb + b]) || std::fabs(result[q * tb + b] - expected) > tolerance) {
        if (outside_tolerance < 3) std::printf("4eps outlier q=%ld b=%ld got=%.9g expected=%.9g bound=%.9g\n",
                                              q, b, result[q * tb + b], expected, gamma * magnitude);
        ++outside_tolerance;
      }
      if (!std::isfinite(result[q * tb + b]) || std::fabs(result[q * tb + b] - expected) > gamma * magnitude)
        ++rounding_bound_failures;
    }
  }
  std::printf("norm_max_abs_error=%.9g distance_max_abs_error=%.9g mismatches=%d/8192\n",
               max_norm_error, max_distance_error, mismatches);
  std::printf("outside_4eps_relative_tolerance=%d/8192\n", outside_tolerance);
  int rank_inversions = 0;
  for (long q = 0; q < m; ++q) {
    std::vector<int> order(tb);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
      return result[q * tb + a] < result[q * tb + b];
    });
    for (long k = 1; k < tb; ++k)
      if (exact[q * tb + order[k - 1]] > exact[q * tb + order[k]]) ++rank_inversions;
  }
  std::printf("operand_rounding_bound_failures=%d rank_inversions=%d\n", rounding_bound_failures, rank_inversions);
  // The library path is an independent hardware comparison for the WMMA path.
  rocblas_handle blas = nullptr;
  if (rocblas_create_handle(&blas) != rocblas_status_success) return 4;
  const float alpha = -2, beta = 0;
  if (rocblas_gemm_ex(blas, rocblas_operation_transpose, rocblas_operation_none,
      tb, m, dim, &alpha, data + bo * dim, rocblas_datatype_f16_r, dim,
      data + qo * dim, rocblas_datatype_f16_r, dim, &beta,
      dist, rocblas_datatype_f32_r, tb, dist, rocblas_datatype_f32_r, tb,
      rocblas_datatype_f32_r, rocblas_gemm_algo_standard, 0, 0) != rocblas_status_success) return 4;
  sk_add_normb_kernel<<<16, 256>>>(dist, norm + bo, m * tb, tb, 1);
  CHECK(hipGetLastError());
  sk_restore_norm_kernel<<<1, 256>>>(dist, norm + qo, m, tb);
  CHECK(hipGetLastError());
  CHECK(hipDeviceSynchronize());
  std::vector<float> library(m * tb);
  CHECK(hipMemcpy(library.data(), dist, library.size() * sizeof(float), hipMemcpyDeviceToHost));
  double library_error = 0, wmma_library_error = 0;
  int library_bound_errors = 0;
  for (long k = 0; k < m * tb; ++k) {
    library_error = std::fmax(library_error, std::fabs(library[k] - exact[k]));
    wmma_library_error = std::fmax(wmma_library_error, std::fabs(library[k] - result[k]));
    const double nq = norms[qo + k / tb], nb = norms[bo + k % tb];
    const double magnitude_bound = nq + nb + 2 * std::sqrt(nq * nb);
    if (!std::isfinite(library[k]) || std::fabs(library[k] - exact[k]) > gamma * magnitude_bound)
      ++library_bound_errors;
  }
  std::printf("rocblas_max_abs_error=%.9g wmma_vs_rocblas_max_abs_error=%.9g library_bound_errors=%d\n",
               library_error, wmma_library_error, library_bound_errors);
  if (rocblas_destroy_handle(blas) != rocblas_status_success) return 4;
  CHECK(hipFree(data));
  CHECK(hipFree(norm));
  CHECK(hipFree(dist));

  // A short first tile is padded, then a short tail is merged into its top-k.
  const float first[] = {1, 4, 2, 7};
  const int first_ids[] = {0, 1, 1, 0};
  const float second[] = {-2, 3, -1, 8};
  const int second_ids[] = {0, 1, 0, 1};
  float *td = nullptr, *bd = nullptr;
  int* ti = nullptr;
  unsigned* bi = nullptr;
  CHECK(hipMalloc(&td, sizeof(first)));
  CHECK(hipMalloc(&ti, sizeof(first_ids)));
  CHECK(hipMalloc(&bd, 8 * sizeof(float)));
  CHECK(hipMalloc(&bi, 8 * sizeof(unsigned)));
  CHECK(hipMemcpy(td, first, sizeof(first), hipMemcpyHostToDevice));
  CHECK(hipMemcpy(ti, first_ids, sizeof(first_ids), hipMemcpyHostToDevice));
  sk_copy_tile_kernel<<<1, 256>>>(td, ti, 0, bd, bi, 2, 4, 2);
  CHECK(hipGetLastError());
  CHECK(hipMemcpy(td, second, sizeof(second), hipMemcpyHostToDevice));
  CHECK(hipMemcpy(ti, second_ids, sizeof(second_ids), hipMemcpyHostToDevice));
  sk_merge_tile_kernel<<<1, 256>>>(td, ti, 2, bd, bi, 2, 4, 2);
  CHECK(hipGetLastError());
  CHECK(hipDeviceSynchronize());
  float got_d[8]; unsigned got_i[8];
  CHECK(hipMemcpy(got_d, bd, sizeof(got_d), hipMemcpyDeviceToHost));
  CHECK(hipMemcpy(got_i, bi, sizeof(got_i), hipMemcpyDeviceToHost));
  const float expected_d[] = {-2, 1, 3, 4, -1, 2, 7, 8};
  const unsigned expected_i[] = {2, 0, 3, 1, 2, 1, 0, 3};
  for (int k = 0; k < 8; ++k) {
    if (got_d[k] != expected_d[k] || got_i[k] != expected_i[k]) { return 3; }
  }
  CHECK(hipFree(td)); CHECK(hipFree(ti)); CHECK(hipFree(bd)); CHECK(hipFree(bi));
  std::puts("PASS: real GPU short-tile copy/merge kernels");
  if (max_norm_error || rounding_bound_failures || rank_inversions || library_bound_errors) return 2;
  std::puts("PASS: 8192 WMMA L2 distances within operand-based FP32 bound; exact-distance ordering preserved");
  return 0;
}
