#!/usr/bin/env python3
"""Spike-only WAL serializer byte comparison; does not test storage or durability.

Compile the actual commitOps serialization prefixes from two storage.cpp files
without compiling or replacing any storage/syscall code. Optional timings and
allocation counts cover serialization only, not the database or fsync.
"""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def serializer(path, name):
    source = path.read_text()
    signature = "void Table::commitOps(std::vector<Op> &ops) {"
    start = source.index(signature) + len(signature)
    end = source.index("\n  writeAll(wal_fd_, frame.data(), frame.size(),", start)
    body = source[start:end].replace("if (ops.empty()) return;", "if (ops.empty()) return {};")
    return f"std::vector<uint8_t> {name}(std::vector<Op> &ops) {{\n{body}\nreturn frame;\n}}\n"


HARNESS = r'''
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include "storage.h"

static bool count_allocations = false;
static size_t allocation_count = 0, allocation_bytes = 0;
void *operator new(size_t n) {
  if (count_allocations) { ++allocation_count; allocation_bytes += n; }
  if (void *p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, size_t) noexcept { std::free(p); }

using namespace mvd;
constexpr uint64_t kFrameMagic = 0x4D5644434F4D4D49ULL;
SERIALIZERS

static void require(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message);
}

// Independent bitwise CRC checks the emitted checksum, not crc32() itself.
static uint32_t reference_crc(const uint8_t *p, size_t n) {
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < n; ++i) {
    crc ^= p[i];
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
  }
  return crc ^ 0xffffffffu;
}

template <class T> static T read(const std::vector<uint8_t> &bytes, size_t &pos) {
  require(pos + sizeof(T) <= bytes.size(), "truncated frame field");
  T result;
  std::memcpy(&result, bytes.data() + pos, sizeof(T));
  pos += sizeof(T);
  return result;
}

static void check_frame(const std::vector<uint8_t> &frame, const std::vector<Op> &ops) {
  if (ops.empty()) { require(frame.empty(), "empty commit emits a frame"); return; }
  size_t pos = 0;
  const auto len = read<uint32_t>(frame, pos);
  const auto crc = read<uint32_t>(frame, pos);
  require(frame.size() == size_t(len) + 16, "wrong frame length");
  require(crc == reference_crc(frame.data() + 8, len), "wrong CRC coverage");
  require(read<uint32_t>(frame, pos) == ops.size(), "wrong operation count");
  for (const Op &op : ops) {
    require(read<uint8_t>(frame, pos) == op.type, "wrong operation type");
    require(read<uint32_t>(frame, pos) == op.slot, "wrong operation slot");
    if (op.type != 3) {
      require(read<uint32_t>(frame, pos) == op.data.size(), "wrong row-image length");
      require(pos + op.data.size() <= frame.size(), "truncated row image");
      require(std::equal(op.data.begin(), op.data.end(), frame.begin() + pos), "changed row bytes");
      pos += op.data.size();
    }
  }
  require(pos == size_t(len) + 8, "extra or missing operation bytes");
  require(read<uint64_t>(frame, pos) == 0x4D5644434F4D4D49ULL, "wrong frame magic");
  require(pos == frame.size(), "trailing bytes");
}

static std::vector<Op> make_ops(size_t count, size_t row_bytes, int mode) {
  std::vector<Op> ops;
  ops.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    Op op;
    op.type = mode ? mode : 1 + i % 3;
    op.slot = uint32_t(i * 7919u + 0x80000000u);
    // Delete data must be ignored even if a caller leaves a buffer attached.
    op.data.resize(row_bytes);
    for (size_t j = 0; j < row_bytes; ++j) op.data[j] = uint8_t(i * 17u + j * 31u);
    ops.push_back(std::move(op));
  }
  return ops;
}

using Serializer = std::vector<uint8_t> (*)(std::vector<Op> &);
static volatile uint64_t sink = 0;
static void measure(Serializer serialize, std::vector<Op> &ops, const char *name, int round) {
  allocation_count = allocation_bytes = 0;
  count_allocations = true;
  auto sample = serialize(ops);
  count_allocations = false;
  const size_t allocations = allocation_count, bytes = allocation_bytes;
  const size_t repeats = ops.size() == 1 ? 10000 : ops.size() == 100 ? 100 :
                         ops.size() == 1000 ? 5 : 1;
  const auto begin = std::chrono::steady_clock::now();
  for (size_t i = 0; i < repeats; ++i) {
    auto frame = serialize(ops);
    sink += frame[4] + frame.back();
  }
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - begin).count();
  std::cout << "BENCH," << name << ',' << round << ',' << ops.size() << ','
            << sample.size() << ',' << allocations << ',' << bytes << ','
            << ns / double(repeats) << '\n';
}

int main(int argc, char **argv) {
  size_t cases = 0;
  for (size_t count : {0, 1, 100, 1000, 10000}) {
    for (size_t row_bytes : {0, 1, 64, 256, 1024}) {
      for (int mode : {0, 1, 2, 3}) {
        auto ops = make_ops(count, row_bytes, mode);
        auto want = baseline(ops);
        auto actual = candidate(ops);
        require(actual == want, "candidate frame differs from baseline");
        check_frame(actual, ops);
        ++cases;
      }
    }
  }
  std::cout << "PASS: " << cases << " exact-byte and independent framing cases\n";
  if (argc > 1 && std::string(argv[1]) == "--benchmark") {
    std::cout << "BENCH_HEADER,variant,round,batch,frame_bytes,allocations,allocated_bytes,ns_per_frame\n";
    for (size_t count : {1, 100, 1000, 10000}) {
      auto ops = make_ops(count, 256, 1);
      for (int round = 0; round < 3; ++round) {
        if (round % 2) {
          measure(candidate, ops, "candidate", round);
          measure(baseline, ops, "baseline", round);
        } else {
          measure(baseline, ops, "baseline", round);
          measure(candidate, ops, "candidate", round);
        }
      }
    }
  }
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--candidate", type=Path, default=Path(__file__).resolve().parents[1] / "src/storage.cpp")
    parser.add_argument("--benchmark", action="store_true")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    serializers = serializer(args.baseline, "baseline") + serializer(args.candidate, "candidate")
    with tempfile.TemporaryDirectory(prefix="mvd-wal-serializer-") as tmp:
        source = Path(tmp) / "serialization.cpp"
        binary = Path(tmp) / "serialization"
        source.write_text(HARNESS.replace("SERIALIZERS", serializers))
        command = [os.environ.get("CXX", "c++"), "-std=c++17", "-O2", "-Wall", "-Wextra",
                   "-I", str(args.candidate.resolve().parent), str(source), "-o", str(binary)]
        if args.sanitize:
            command.extend(["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])
        subprocess.run(command, check=True)
        subprocess.run([str(binary)] + (["--benchmark"] if args.benchmark else []), check=True)


if __name__ == "__main__":
    main()
