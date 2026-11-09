// 向量距离计算：AVX2/FMA 运行时派发，标量路径兜底。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mvd {

float l2DistanceScalar(const float *a, const float *b, size_t dim);

struct TopKEntry {
  float dist;
  int64_t id;
  uint32_t slot;
};

// 无过滤 TopK 专用扫描：base 指向槽 0，槽内 valid 在 +0、id 在 +8、向量在 vec_off。
// 结果按 (距离, 主键) 升序返回。
void scanTopK(const uint8_t *base, uint32_t slot_count, uint32_t slot_size, uint32_t vec_off,
              int dim, const float *query, int k, std::vector<TopKEntry> &out);

#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2,fma"))) float l2DistanceAvx2(const float *a, const float *b, size_t dim);
#endif

// 运行时选择最优实现（首次调用做一次 CPU 特性检查）。
inline float l2Distance(const float *a, const float *b, size_t dim) {
#if defined(__x86_64__) || defined(__i386__)
  static const bool has_avx2 = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
  if (has_avx2) return l2DistanceAvx2(a, b, dim);
#endif
  return l2DistanceScalar(a, b, dim);
}

}  // namespace mvd
