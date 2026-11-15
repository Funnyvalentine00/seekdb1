// 向量距离计算：AVX2/FMA 运行时派发，标量路径兜底。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mvd {

float l2DistanceScalar(const float *a, const float *b, size_t dim);

// CRC-32C（Castagnoli）：SSE4.2 硬件指令，无 SSE4.2 时回退表驱动。
// 仅供 WAL 帧内部校验（自写自读），与外部 CRC-32 语义无关。
uint32_t crc32c(const void *data, size_t len, uint32_t seed = 0);

struct TopKEntry {
  float dist;
  int64_t id;
  uint32_t slot;
};

// 无过滤 TopK 专用扫描：base 指向槽 0，槽内 valid 在 +0、id 在 +8、向量在 vec_off。
// 结果按 (距离, 主键) 升序返回。
void scanTopK(const uint8_t *base, uint32_t slot_count, uint32_t slot_size, uint32_t vec_off,
              int dim, const float *query, size_t k, std::vector<TopKEntry> &out);

// 连续布局扫描：vecs 为 count × dim 连续向量、ids 平行主键（dim % 32 == 0 时走
// 批处理 AVX2；求和树与 scanTopK 一致，结果逐位相同）。
void scanTopKContiguous(const float *vecs, const int64_t *ids, uint32_t count, int dim,
                        const float *query, size_t k, std::vector<TopKEntry> &out);

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
