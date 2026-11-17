#include "vecops.h"

#include <iostream>
#include <random>

namespace {

uint32_t referenceCrc32c(const uint8_t *data, size_t size, uint32_t seed) {
  uint32_t crc = seed ^ 0xffffffffu;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      const uint32_t mask = 0u - (crc & 1u);
      crc = (crc >> 1) ^ (0x82f63b78u & mask);
    }
  }
  return crc ^ 0xffffffffu;
}

bool check(size_t size, size_t offset, std::mt19937 &rng) {
  // End each input at the allocation boundary so ASan can detect tail overreads.
  std::vector<uint8_t> bytes(offset + size + (offset + size == 0));
  for (uint8_t &byte : bytes) byte = static_cast<uint8_t>(rng());

  const uint8_t *data = bytes.data() + offset;
  const uint32_t seed = static_cast<uint32_t>(rng());
  const uint32_t expected = referenceCrc32c(data, size, seed);
  const uint32_t actual = mvd::crc32c(data, size, seed);
  const size_t split = rng() % (size + 1);
  const uint32_t first = mvd::crc32c(data, split, seed);
  const uint32_t chained = mvd::crc32c(data + split, size - split, first);
  if (actual == expected && chained == expected) return true;

  std::cerr << "CRC mismatch for size=" << size << " offset=" << offset << " seed=" << seed
            << " split=" << split << " expected=" << expected << " actual=" << actual
            << " chained=" << chained << '\n';
  return false;
}

}  // namespace

int main() {
  if (mvd::crc32c("123456789", 9) != 0xe3069283u ||
      mvd::crc32c(nullptr, 0) != 0 || mvd::crc32c(nullptr, 0, 0x10203040u) != 0x10203040u) {
    std::cerr << "CRC check vector or empty input failed\n";
    return 1;
  }

  std::mt19937 rng(0x5eedu);
  size_t cases = 0;
  for (size_t size = 0; size < 256; ++size) {
    for (size_t offset = 0; offset < 16; ++offset) {
      if (!check(size, offset, rng)) return 1;
      ++cases;
    }
  }
  for (size_t size : {4095u, 4096u, 4097u, 64904u, 649004u, 6490004u}) {
    if (!check(size, 7, rng)) return 1;
    ++cases;
  }
  std::cout << "CRC32C known vector, empty input and " << cases
            << " seeded/unaligned/chained cases passed\n";
}
