/* Minimal rapids_logger stub: just enough for raft/core/logger.hpp's
 * default_logger() to compile. The bridge never calls it; real log calls are
 * no-oped by the logger_macros shim. */
#pragma once
#include <cstdio>
#include <memory>
#include <string>

namespace rapids_logger {

struct basic_file_sink_mt {
  basic_file_sink_mt(const char*, bool) {}
};
struct stderr_sink_mt {};

using sink_ptr = std::shared_ptr<void>;

class logger {
 public:
  logger(const char*, std::initializer_list<sink_ptr>) {}
  void set_pattern(const std::string&) {}
  void set_level(int) {}
  void log(int, const char*) {}
};

}  // namespace rapids_logger
