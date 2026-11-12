// Focused differential test. Link the original vecops.cpp compiled with
// -Dmvd=scan_baseline and this variant's vecops.cpp as separate objects.
#include "../src/vecops.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>

namespace scan_baseline {
struct TopKEntry {
  float dist;
  int64_t id;
  uint32_t slot;
};
void scanTopK(const uint8_t *, uint32_t, uint32_t, uint32_t, int, const float *,
              size_t, std::vector<TopKEntry> &);
float l2DistanceScalar(const float *, const float *, size_t);
#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2,fma")))
float l2DistanceAvx2(const float *, const float *, size_t);
#endif
}  // namespace scan_baseline

namespace {
size_t cases = 0;

bool sameFloat(float a, float b) {
  uint32_t ab, bb;
  std::memcpy(&ab, &a, sizeof(ab));
  std::memcpy(&bb, &b, sizeof(bb));
  return ab == bb;
}

float baselineDistance(const float *a, const float *b, size_t dim) {
#if defined(__x86_64__) || defined(__i386__)
  if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")) {
    return scan_baseline::l2DistanceAvx2(a, b, dim);
  }
#endif
  return scan_baseline::l2DistanceScalar(a, b, dim);
}

void runCase(int dim, uint32_t count, int pattern, int deletion, std::mt19937 &rng) {
  // Two nonzero vector offsets exercise columns before the vector as well as
  // the compact id+vector row. Both are supported by the storage layout.
  const uint32_t vec_off = pattern % 2 ? 36 : 16;
  const uint32_t stride = (vec_off + 4 * dim + 63) / 64 * 64;
  std::vector<uint8_t> data(static_cast<size_t>(count) * stride, 0);
  std::vector<int64_t> ids(count);
  for (uint32_t s = 0; s < count; ++s) ids[s] = static_cast<int64_t>(s) - count / 2;
  if (count >= 2) {
    ids[0] = std::numeric_limits<int64_t>::min();
    ids[1] = std::numeric_limits<int64_t>::max();
  }
  std::shuffle(ids.begin(), ids.end(), rng);
  std::uniform_real_distribution<float> sample(-3.0f, 3.0f);
  std::vector<float> query(dim, 0.0f);
  if (pattern == 0 || pattern == 2) {
    for (float &v : query) v = sample(rng);
  }
  std::vector<scan_baseline::TopKEntry> reference;
  for (uint32_t s = 0; s < count; ++s) {
    uint8_t *p = data.data() + static_cast<size_t>(s) * stride;
    p[0] = deletion == 2 ? 0 : (deletion == 1 && s % 3 == 0 ? 0 : 1);
    std::memcpy(p + 8, &ids[s], sizeof(ids[s]));
    float *v = reinterpret_cast<float *>(p + vec_off);
    for (int j = 0; j < dim; ++j) {
      if (pattern == 0) v[j] = sample(rng);
      if (pattern == 1) v[j] = j == 0 ? (s % 2 ? 1.0f : -1.0f) : 0.0f;
      if (pattern == 2) v[j] = std::ldexp(sample(rng), static_cast<int>(rng() % 100) - 50);
      if (pattern == 3) v[j] = j == 0 ? (s % 2 ? std::numeric_limits<float>::max() : 0.0f) : 0.0f;
    }
    float expected = baselineDistance(v, query.data(), dim);
    if (!sameFloat(expected, mvd::l2Distance(v, query.data(), dim))) {
      throw std::runtime_error("distance bits changed");
    }
    if (p[0]) reference.push_back({expected, ids[s], s});
  }
  std::sort(reference.begin(), reference.end(), [](const auto &a, const auto &b) {
    return a.dist != b.dist ? a.dist < b.dist : a.id < b.id;
  });
  for (size_t k : {size_t{0}, size_t{1}, size_t{10}, size_t{100}, size_t{1000000}}) {
    std::vector<scan_baseline::TopKEntry> old_entries;
    // Prepopulate output to verify the clearing contract, including k == 0.
    std::vector<mvd::TopKEntry> entries{{-1.0f, -1, 0}};
    scan_baseline::scanTopK(data.data(), count, stride, vec_off, dim, query.data(), k, old_entries);
    mvd::scanTopK(data.data(), count, stride, vec_off, dim, query.data(), k, entries);
    const size_t expected_size = std::min(k, reference.size());
    if (entries.size() != expected_size || old_entries.size() != expected_size) {
      throw std::runtime_error("TopK size mismatch");
    }
    for (size_t i = 0; i < entries.size(); ++i) {
      const auto &got = entries[i];
      const auto &old = old_entries[i];
      const auto &ref = reference[i];
      if (got.id != old.id || got.slot != old.slot || !sameFloat(got.dist, old.dist) ||
          got.id != ref.id || got.slot != ref.slot || !sameFloat(got.dist, ref.dist)) {
        std::cerr << "dim=" << dim << " count=" << count << " pattern=" << pattern
                  << " deletion=" << deletion << " k=" << k << " result=" << i << '\n';
        throw std::runtime_error("TopK exact differential/oracle mismatch");
      }
    }
    ++cases;
  }
}
}  // namespace

int main() {
  std::mt19937 rng(0x5ca112);
  for (int dim : {1, 2, 31, 32, 127, 128, 129, 512}) {
    for (uint32_t count : {0U, 1U, 2U, 33U, 257U}) {
      for (int pattern = 0; pattern < 4; ++pattern) {
        for (int deletion = 0; deletion < 3; ++deletion) runCase(dim, count, pattern, deletion, rng);
      }
    }
  }
  for (int pattern = 0; pattern < 4; ++pattern) {
    for (int deletion = 0; deletion < 3; ++deletion) runCase(128, 10000, pattern, deletion, rng);
  }
  bool avx2 = false;
#if defined(__x86_64__) || defined(__i386__)
  avx2 = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#endif
  std::cout << "PASS " << cases << " exact differential/oracle cases; avx2=" << avx2 << '\n';
}
