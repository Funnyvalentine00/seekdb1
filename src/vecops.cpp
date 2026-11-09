#include "vecops.h"

#include <immintrin.h>

#include <algorithm>
#include <cstring>

namespace mvd {

float l2DistanceScalar(const float *a, const float *b, size_t dim) {
  float s = 0.0f;
  for (size_t i = 0; i < dim; ++i) {
    float d = a[i] - b[i];
    s += d * d;
  }
  return s;
}

#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2,fma"))) float l2DistanceAvx2(const float *a, const float *b,
                                                         size_t dim) {
  __m256 s0 = _mm256_setzero_ps();
  __m256 s1 = _mm256_setzero_ps();
  __m256 s2 = _mm256_setzero_ps();
  __m256 s3 = _mm256_setzero_ps();
  size_t i = 0;
  for (; i + 32 <= dim; i += 32) {
    __m256 a0 = _mm256_loadu_ps(a + i);
    __m256 b0 = _mm256_loadu_ps(b + i);
    __m256 a1 = _mm256_loadu_ps(a + i + 8);
    __m256 b1 = _mm256_loadu_ps(b + i + 8);
    __m256 a2 = _mm256_loadu_ps(a + i + 16);
    __m256 b2 = _mm256_loadu_ps(b + i + 16);
    __m256 a3 = _mm256_loadu_ps(a + i + 24);
    __m256 b3 = _mm256_loadu_ps(b + i + 24);
    __m256 d0 = _mm256_sub_ps(a0, b0);
    __m256 d1 = _mm256_sub_ps(a1, b1);
    __m256 d2 = _mm256_sub_ps(a2, b2);
    __m256 d3 = _mm256_sub_ps(a3, b3);
    s0 = _mm256_fmadd_ps(d0, d0, s0);
    s1 = _mm256_fmadd_ps(d1, d1, s1);
    s2 = _mm256_fmadd_ps(d2, d2, s2);
    s3 = _mm256_fmadd_ps(d3, d3, s3);
  }
  for (; i + 8 <= dim; i += 8) {
    __m256 d = _mm256_sub_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i));
    s0 = _mm256_fmadd_ps(d, d, s0);
  }
  __m256 s = _mm256_add_ps(_mm256_add_ps(s0, s1), _mm256_add_ps(s2, s3));
  __m128 lo = _mm256_castps256_ps128(s);
  __m128 hi = _mm256_extractf128_ps(s, 1);
  __m128 sum = _mm_add_ps(lo, hi);
  sum = _mm_hadd_ps(sum, sum);
  sum = _mm_hadd_ps(sum, sum);
  float r = _mm_cvtss_f32(sum);
  for (; i < dim; ++i) {
    float d = a[i] - b[i];
    r += d * d;
  }
  return r;
}
namespace {

struct HeapCmp {
  bool operator()(const TopKEntry &a, const TopKEntry &b) const {
    if (a.dist != b.dist) return a.dist < b.dist;
    return a.id < b.id;
  }
};

inline void heapPush(std::vector<TopKEntry> &heap, int k, const TopKEntry &e) {
  if (static_cast<int>(heap.size()) < k) {
    heap.push_back(e);
    std::push_heap(heap.begin(), heap.end(), HeapCmp());
  } else if (e.dist < heap.front().dist ||
             (e.dist == heap.front().dist && e.id < heap.front().id)) {
    std::pop_heap(heap.begin(), heap.end(), HeapCmp());
    heap.back() = e;
    std::push_heap(heap.begin(), heap.end(), HeapCmp());
  }
}

}  // namespace

void scanTopK(const uint8_t *base, uint32_t slot_count, uint32_t slot_size, uint32_t vec_off,
              int dim, const float *query, int k, std::vector<TopKEntry> &out) {
  out.clear();
  if (k <= 0) return;
  std::vector<TopKEntry> heap;
  heap.reserve(static_cast<size_t>(k) + 1);
#if defined(__x86_64__) || defined(__i386__)
  static const bool has_avx2 = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
  if (has_avx2) {
    for (uint32_t s = 0; s < slot_count; ++s) {
      const uint8_t *p = base + static_cast<size_t>(s) * slot_size;
      if (p[0] == 0) continue;
      int64_t id;
      std::memcpy(&id, p + 8, sizeof(id));
      float d = l2DistanceAvx2(reinterpret_cast<const float *>(p + vec_off), query,
                               static_cast<size_t>(dim));
      heapPush(heap, k, TopKEntry{d, id, s});
    }
    std::sort(heap.begin(), heap.end(), HeapCmp());
    out = std::move(heap);
    return;
  }
#endif
  for (uint32_t s = 0; s < slot_count; ++s) {
    const uint8_t *p = base + static_cast<size_t>(s) * slot_size;
    if (p[0] == 0) continue;
    int64_t id;
    std::memcpy(&id, p + 8, sizeof(id));
    float d = l2DistanceScalar(reinterpret_cast<const float *>(p + vec_off), query,
                               static_cast<size_t>(dim));
    heapPush(heap, k, TopKEntry{d, id, s});
  }
  std::sort(heap.begin(), heap.end(), HeapCmp());
  out = std::move(heap);
}

#endif

}  // namespace mvd
