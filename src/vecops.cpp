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

// 以下全部函数以 AVX2/FMA 编译（pragma GCC target 对模板也生效）。
// 入口 scanTopK 在运行时检查 CPU 特性后才派发进来。
#pragma GCC push_options
#pragma GCC target("avx2,fma")

float l2DistanceAvx2(const float *a, const float *b, size_t dim) {
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

inline void heapPush(std::vector<TopKEntry> &heap, size_t k, const TopKEntry &e) {
  if (heap.size() < k) {
    heap.push_back(e);
    std::push_heap(heap.begin(), heap.end(), HeapCmp());
  } else if (e.dist < heap.front().dist ||
             (e.dist == heap.front().dist && e.id < heap.front().id)) {
    std::pop_heap(heap.begin(), heap.end(), HeapCmp());
    heap.back() = e;
    std::push_heap(heap.begin(), heap.end(), HeapCmp());
  }
}

inline float hsum256(__m256 s) {
  __m128 lo = _mm256_castps256_ps128(s);
  __m128 hi = _mm256_extractf128_ps(s, 1);
  __m128 sum = _mm_add_ps(lo, hi);
  sum = _mm_hadd_ps(sum, sum);
  sum = _mm_hadd_ps(sum, sum);
  return _mm_cvtss_f32(sum);
}

// 两行各 4 累加器的独立归约；两条链路互不依赖，可被乱序执行重叠。
inline void reducePair(__m256 a0, __m256 a1, __m256 a2, __m256 a3, __m256 b0, __m256 b1,
                       __m256 b2, __m256 b3, float &da, float &db) {
  __m256 sa = _mm256_add_ps(_mm256_add_ps(a0, a1), _mm256_add_ps(a2, a3));
  __m256 sb = _mm256_add_ps(_mm256_add_ps(b0, b1), _mm256_add_ps(b2, b3));
  da = hsum256(sa);
  db = hsum256(sb);
}

// 双行交错、块数编译期固定的快路径。BLOCKS = dim/32。
// 每行求和顺序与旧的单行实现一致（块 j 独立累加、同序归约），结果逐位不变。
template <int BLOCKS>
void scanTopKBatched(const uint8_t *base, uint32_t slot_count, uint32_t slot_size,
                     uint32_t vec_off, const float *query, size_t k,
                     std::vector<TopKEntry> &out) {
  std::vector<TopKEntry> heap;
  heap.reserve(k);
  __m256 q[BLOCKS];
  for (int j = 0; j < BLOCKS; ++j) q[j] = _mm256_loadu_ps(query + j * 8);

  uint32_t s = 0;
  for (; s + 2 <= slot_count; s += 2) {
    const uint8_t *pa = base + static_cast<size_t>(s) * slot_size;
    const uint8_t *pb = pa + slot_size;
    const bool va = pa[0] != 0;
    const bool vb = pb[0] != 0;
    if (va && vb) {
      const float *ra = reinterpret_cast<const float *>(pa + vec_off);
      const float *rb = reinterpret_cast<const float *>(pb + vec_off);
      __m256 a[BLOCKS], b[BLOCKS];
      for (int j = 0; j < BLOCKS; ++j) {
        __m256 da = _mm256_sub_ps(_mm256_loadu_ps(ra + j * 8), q[j]);
        __m256 db = _mm256_sub_ps(_mm256_loadu_ps(rb + j * 8), q[j]);
        a[j] = _mm256_fmadd_ps(da, da, _mm256_setzero_ps());
        b[j] = _mm256_fmadd_ps(db, db, _mm256_setzero_ps());
      }
      float da, db;
      if (BLOCKS == 1) {
        da = hsum256(a[0]);
        db = hsum256(b[0]);
      } else {
        __m256 az = _mm256_setzero_ps();
        reducePair(a[0], a[1], BLOCKS > 2 ? a[2] : az, BLOCKS > 3 ? a[3] : az,
                   b[0], b[1], BLOCKS > 2 ? b[2] : az, BLOCKS > 3 ? b[3] : az, da, db);
      }
      int64_t ida, idb;
      std::memcpy(&ida, pa + 8, sizeof(ida));
      std::memcpy(&idb, pb + 8, sizeof(idb));
      heapPush(heap, k, TopKEntry{da, ida, s});
      heapPush(heap, k, TopKEntry{db, idb, s + 1});
    } else if (va || vb) {
      const uint8_t *p = va ? pa : pb;
      const uint32_t slot = va ? s : s + 1;
      const float *row = reinterpret_cast<const float *>(p + vec_off);
      float dsum = 0.0f;
      for (int j = 0; j < BLOCKS; ++j) {
        __m256 d = _mm256_sub_ps(_mm256_loadu_ps(row + j * 8), q[j]);
        dsum += hsum256(_mm256_fmadd_ps(d, d, _mm256_setzero_ps()));
      }
      int64_t id;
      std::memcpy(&id, p + 8, sizeof(id));
      heapPush(heap, k, TopKEntry{dsum, id, slot});
    }
  }
  if (s < slot_count) {
    const uint8_t *p = base + static_cast<size_t>(s) * slot_size;
    if (p[0] != 0) {
      const float *row = reinterpret_cast<const float *>(p + vec_off);
      float dsum = 0.0f;
      for (int j = 0; j < BLOCKS; ++j) {
        __m256 d = _mm256_sub_ps(_mm256_loadu_ps(row + j * 8), q[j]);
        dsum += hsum256(_mm256_fmadd_ps(d, d, _mm256_setzero_ps()));
      }
      int64_t id;
      std::memcpy(&id, p + 8, sizeof(id));
      heapPush(heap, k, TopKEntry{dsum, id, s});
    }
  }
  std::sort(heap.begin(), heap.end(), HeapCmp());
  out = std::move(heap);
}

void scanTopKAvx2(const uint8_t *base, uint32_t slot_count, uint32_t slot_size, uint32_t vec_off,
                  int dim, const float *query, size_t k, std::vector<TopKEntry> &out) {
  out.clear();
  k = std::min<size_t>(k, slot_count);
  if (k == 0) return;
  if (dim >= 32 && dim % 32 == 0 && dim <= 128) {
    switch (dim / 32) {
      case 1:
        scanTopKBatched<1>(base, slot_count, slot_size, vec_off, query, k, out);
        return;
      case 2:
        scanTopKBatched<2>(base, slot_count, slot_size, vec_off, query, k, out);
        return;
      case 3:
        scanTopKBatched<3>(base, slot_count, slot_size, vec_off, query, k, out);
        return;
      default:
        scanTopKBatched<4>(base, slot_count, slot_size, vec_off, query, k, out);
        return;
    }
  }
  // 通用形状：逐行计算（已在 AVX2 编译区域内，无跨 target 调用开销）。
  std::vector<TopKEntry> heap;
  heap.reserve(k);
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
}

}  // namespace

#pragma GCC pop_options

#endif  // x86

void scanTopK(const uint8_t *base, uint32_t slot_count, uint32_t slot_size, uint32_t vec_off,
              int dim, const float *query, size_t k, std::vector<TopKEntry> &out) {
  out.clear();
  k = std::min<size_t>(k, slot_count);
  if (k == 0) return;
#if defined(__x86_64__) || defined(__i386__)
  static const bool has_avx2 = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
  if (has_avx2) {
    scanTopKAvx2(base, slot_count, slot_size, vec_off, dim, query, k, out);
    return;
  }
#endif
  std::vector<TopKEntry> heap;
  heap.reserve(k);
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

}  // namespace mvd
