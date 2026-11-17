#include "vecops.h"

#include <immintrin.h>

#include <algorithm>
#include <cstring>

namespace mvd {

#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("sse4.2"))) uint32_t crc32c_hw(const void *data, size_t len,
                                                     uint32_t seed) {
  uint64_t c = seed ^ 0xFFFFFFFFu;
  const uint8_t *p = static_cast<const uint8_t *>(data);
  size_t rest = len;
  while (rest >= 8) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    c = _mm_crc32_u64(c, v);
    p += 8;
    rest -= 8;
  }
  while (rest >= 4) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    c = _mm_crc32_u32(c, v);
    p += 4;
    rest -= 4;
  }
  while (rest--) {
    c = _mm_crc32_u8(c, *p++);
  }
  return static_cast<uint32_t>(c) ^ 0xFFFFFFFFu;
}
#endif

uint32_t crc32c(const void *data, size_t len, uint32_t seed) {
#if defined(__x86_64__) || defined(__i386__)
  static const bool has_sse42 = __builtin_cpu_supports("sse4.2");
  if (has_sse42) return crc32c_hw(data, len, seed);
#endif
  // 表驱动兜底（CRC-32C 多项式 0x82F63B78，反射形式）。
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t v = i;
      for (int k = 0; k < 8; ++k) v = (v & 1) ? (0x82F63B78u ^ (v >> 1)) : (v >> 1);
      table[i] = v;
    }
    init = true;
  }
  uint32_t c = seed ^ 0xFFFFFFFFu;
  const uint8_t *p = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i < len; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

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

// 双行交错快路径。VECS = dim/8（__m256 个数），调用处保证 dim % 32 == 0，
// 因此 VECS % 4 == 0：每 4 个 __m256（32 维）为一组，acc[j] 轮转累加——
// acc[j] 的维集合与求和树与旧的单行实现完全一致，结果逐位不变。
template <int VECS>
void scanTopKBatched(const uint8_t *base, uint32_t slot_count, uint32_t slot_size,
                     uint32_t vec_off, const float *query, size_t k,
                     std::vector<TopKEntry> &out) {
  std::vector<TopKEntry> heap;
  heap.reserve(k);
  __m256 q[VECS];
  for (int i = 0; i < VECS; ++i) q[i] = _mm256_loadu_ps(query + i * 8);
  constexpr int GROUPS = VECS / 4;

  // 单行全维累加：acc[j] 汇总维 (g*32 + j*8 .. +8)，g 遍历各组。
  auto accumRow = [&](const float *row, __m256 (&acc)[4]) {
    acc[0] = acc[1] = acc[2] = acc[3] = _mm256_setzero_ps();
    for (int g = 0; g < GROUPS; ++g) {
      const float *r = row + g * 32;
      const __m256 *qq = q + g * 4;
      __m256 d0 = _mm256_sub_ps(_mm256_loadu_ps(r + 0), qq[0]);
      acc[0] = _mm256_fmadd_ps(d0, d0, acc[0]);
      __m256 d1 = _mm256_sub_ps(_mm256_loadu_ps(r + 8), qq[1]);
      acc[1] = _mm256_fmadd_ps(d1, d1, acc[1]);
      __m256 d2 = _mm256_sub_ps(_mm256_loadu_ps(r + 16), qq[2]);
      acc[2] = _mm256_fmadd_ps(d2, d2, acc[2]);
      __m256 d3 = _mm256_sub_ps(_mm256_loadu_ps(r + 24), qq[3]);
      acc[3] = _mm256_fmadd_ps(d3, d3, acc[3]);
    }
  };

  uint32_t s = 0;
  for (; s + 2 <= slot_count; s += 2) {
    const uint8_t *pa = base + static_cast<size_t>(s) * slot_size;
    const uint8_t *pb = pa + slot_size;
    if (s + 4 <= slot_count) {
      // 硬件预取器对跨页 640B stride 的覆盖有限，软件预取下一对的首条向量线。
      _mm_prefetch(reinterpret_cast<const char *>(pa + 2 * slot_size + vec_off), _MM_HINT_T0);
      _mm_prefetch(reinterpret_cast<const char *>(pa + 3 * slot_size + vec_off), _MM_HINT_T0);
    }
    const bool va = pa[0] != 0;
    const bool vb = pb[0] != 0;
    if (va && vb) {
      __m256 a[4], b[4];
      accumRow(reinterpret_cast<const float *>(pa + vec_off), a);
      accumRow(reinterpret_cast<const float *>(pb + vec_off), b);
      float da, db;
      reducePair(a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3], da, db);
      int64_t ida, idb;
      std::memcpy(&ida, pa + 8, sizeof(ida));
      std::memcpy(&idb, pb + 8, sizeof(idb));
      heapPush(heap, k, TopKEntry{da, ida, s});
      heapPush(heap, k, TopKEntry{db, idb, s + 1});
    } else if (va || vb) {
      const uint8_t *p = va ? pa : pb;
      const uint32_t slot = va ? s : s + 1;
      __m256 acc[4];
      accumRow(reinterpret_cast<const float *>(p + vec_off), acc);
      float dsum;
      __m256 az = _mm256_setzero_ps();
      float db_unused;
      reducePair(acc[0], acc[1], acc[2], acc[3], az, az, az, az, dsum, db_unused);
      int64_t id;
      std::memcpy(&id, p + 8, sizeof(id));
      heapPush(heap, k, TopKEntry{dsum, id, slot});
    }
  }
  if (s < slot_count) {
    const uint8_t *p = base + static_cast<size_t>(s) * slot_size;
    if (p[0] != 0) {
      __m256 acc[4];
      accumRow(reinterpret_cast<const float *>(p + vec_off), acc);
      float dsum;
      __m256 az = _mm256_setzero_ps();
      float db_unused;
      reducePair(acc[0], acc[1], acc[2], acc[3], az, az, az, az, dsum, db_unused);
      int64_t id;
      std::memcpy(&id, p + 8, sizeof(id));
      heapPush(heap, k, TopKEntry{dsum, id, s});
    }
  }
  std::sort(heap.begin(), heap.end(), HeapCmp());
  out = std::move(heap);
}

// 连续布局的双行交错批处理：VECS = dim/8，GROUPS 组 × 4 累加器，
// 求和树与 scanTopKBatched/单行路径一致，结果逐位相同。
template <int VECS>
void scanTopKContigBatched(const float *vecs, const int64_t *ids, uint32_t count,
                           const float *query, size_t k, std::vector<TopKEntry> &out) {
  std::vector<TopKEntry> heap;
  heap.reserve(k);
  __m256 q[VECS];
  for (int i = 0; i < VECS; ++i) q[i] = _mm256_loadu_ps(query + i * 8);
  constexpr int GROUPS = VECS / 4;
  const int dim = VECS * 8;

  auto accumRow = [&](const float *row, __m256 (&acc)[4]) {
    acc[0] = acc[1] = acc[2] = acc[3] = _mm256_setzero_ps();
    for (int g = 0; g < GROUPS; ++g) {
      const float *r = row + g * 32;
      const __m256 *qq = q + g * 4;
      __m256 d0 = _mm256_sub_ps(_mm256_loadu_ps(r + 0), qq[0]);
      acc[0] = _mm256_fmadd_ps(d0, d0, acc[0]);
      __m256 d1 = _mm256_sub_ps(_mm256_loadu_ps(r + 8), qq[1]);
      acc[1] = _mm256_fmadd_ps(d1, d1, acc[1]);
      __m256 d2 = _mm256_sub_ps(_mm256_loadu_ps(r + 16), qq[2]);
      acc[2] = _mm256_fmadd_ps(d2, d2, acc[2]);
      __m256 d3 = _mm256_sub_ps(_mm256_loadu_ps(r + 24), qq[3]);
      acc[3] = _mm256_fmadd_ps(d3, d3, acc[3]);
    }
  };

  uint32_t s = 0;
  for (; s + 2 <= count; s += 2) {
    const float *ra = vecs + static_cast<size_t>(s) * dim;
    const float *rb = ra + dim;
    __m256 a[4], b[4];
    accumRow(ra, a);
    accumRow(rb, b);
    float da, db;
    reducePair(a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3], da, db);
    heapPush(heap, k, TopKEntry{da, ids[s], s});
    heapPush(heap, k, TopKEntry{db, ids[s + 1], s + 1});
  }
  if (s < count) {
    __m256 acc[4];
    accumRow(vecs + static_cast<size_t>(s) * dim, acc);
    float dsum;
    __m256 az = _mm256_setzero_ps();
    float db_unused;
    reducePair(acc[0], acc[1], acc[2], acc[3], az, az, az, az, dsum, db_unused);
    heapPush(heap, k, TopKEntry{dsum, ids[s], s});
  }
  std::sort(heap.begin(), heap.end(), HeapCmp());
  out = std::move(heap);
}

void scanTopKContiguousAvx2(const float *vecs, const int64_t *ids, uint32_t count, int dim,
                            const float *query, size_t k, std::vector<TopKEntry> &out) {
  out.clear();
  k = std::min<size_t>(k, count);
  if (k == 0) return;
  if (dim >= 32 && dim % 32 == 0 && dim <= 128) {
    switch (dim / 32) {
      case 1:
        scanTopKContigBatched<4>(vecs, ids, count, query, k, out);
        return;
      case 2:
        scanTopKContigBatched<8>(vecs, ids, count, query, k, out);
        return;
      case 3:
        scanTopKContigBatched<12>(vecs, ids, count, query, k, out);
        return;
      default:
        scanTopKContigBatched<16>(vecs, ids, count, query, k, out);
        return;
    }
  }
  // 通用形状：逐行计算。
  std::vector<TopKEntry> heap;
  heap.reserve(k);
  for (uint32_t s = 0; s < count; ++s) {
    const float *row = vecs + static_cast<size_t>(s) * dim;
    float d = l2DistanceAvx2(row, query, static_cast<size_t>(dim));
    heapPush(heap, k, TopKEntry{d, ids[s], s});
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
        scanTopKBatched<4>(base, slot_count, slot_size, vec_off, query, k, out);
        return;
      case 2:
        scanTopKBatched<8>(base, slot_count, slot_size, vec_off, query, k, out);
        return;
      case 3:
        scanTopKBatched<12>(base, slot_count, slot_size, vec_off, query, k, out);
        return;
      default:
        scanTopKBatched<16>(base, slot_count, slot_size, vec_off, query, k, out);
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

void scanTopKContiguous(const float *vecs, const int64_t *ids, uint32_t count, int dim,
                        const float *query, size_t k, std::vector<TopKEntry> &out) {
#if defined(__x86_64__) || defined(__i386__)
  static const bool has_avx2 = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
  if (has_avx2) {
    scanTopKContiguousAvx2(vecs, ids, count, dim, query, k, out);
    return;
  }
#endif
  out.clear();
  k = std::min<size_t>(k, count);
  if (k == 0) return;
  std::vector<TopKEntry> heap;
  heap.reserve(k);
  for (uint32_t s = 0; s < count; ++s) {
    const float *row = vecs + static_cast<size_t>(s) * dim;
    float d = l2DistanceScalar(row, query, static_cast<size_t>(dim));
    heapPush(heap, k, TopKEntry{d, ids[s], s});
  }
  std::sort(heap.begin(), heap.end(), HeapCmp());
  out = std::move(heap);
}

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
