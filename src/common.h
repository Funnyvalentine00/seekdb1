// MiniVectorDB 公共定义：错误码、值类型、字符串与数值工具。
#pragma once

#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace mvd {

// 错误分类。协议层会把它渲染成稳定的错误名。
enum class ErrKind {
  Syntax = 1,
  Unsupported = 2,
  TableNotFound = 3,
  TableExists = 4,
  ColumnNotFound = 5,
  ColumnExists = 6,
  TypeMismatch = 7,
  ValueOutOfRange = 8,
  DuplicateKey = 9,
  NotNullViolation = 10,
  IndexNotFound = 11,
  IndexExists = 12,
  InvalidVector = 13,
  InvalidArgument = 14,
  Transaction = 15,
  Io = 16,
  Internal = 17,
  DuplicateColumn = 18,
  TooManyColumns = 19,
  DimensionMismatch = 20,
};

// 错误名与官方公开用例的断言一致：小写 snake_case，如 duplicate_key / unsupported_sql /
// dimension_mismatch。
inline const char *errName(ErrKind k) {
  switch (k) {
    case ErrKind::Syntax: return "syntax_error";
    case ErrKind::Unsupported: return "unsupported_sql";
    case ErrKind::TableNotFound: return "table_not_found";
    case ErrKind::TableExists: return "table_exists";
    case ErrKind::ColumnNotFound: return "column_not_found";
    case ErrKind::ColumnExists: return "column_exists";
    case ErrKind::TypeMismatch: return "type_mismatch";
    case ErrKind::ValueOutOfRange: return "value_out_of_range";
    case ErrKind::DuplicateKey: return "duplicate_key";
    case ErrKind::NotNullViolation: return "not_null_violation";
    case ErrKind::IndexNotFound: return "index_not_found";
    case ErrKind::IndexExists: return "index_exists";
    case ErrKind::InvalidVector: return "invalid_vector";
    case ErrKind::InvalidArgument: return "invalid_argument";
    case ErrKind::Transaction: return "transaction_error";
    case ErrKind::Io: return "io_error";
    case ErrKind::Internal: return "internal_error";
    case ErrKind::DuplicateColumn: return "duplicate_column";
    case ErrKind::TooManyColumns: return "too_many_columns";
    case ErrKind::DimensionMismatch: return "dimension_mismatch";
  }
  return "internal_error";
}

// 所有 SQL 层错误都以异常抛出，由语句执行边界统一捕获。
struct SqlError {
  ErrKind kind;
  std::string msg;
  SqlError(ErrKind k, std::string m) : kind(k), msg(std::move(m)) {}
};

[[noreturn]] inline void throwErr(ErrKind k, const std::string &m) { throw SqlError(k, m); }

inline bool isFiniteFloat(float f) { return std::isfinite(f); }

// UTF-8 码点计数；非法字节按单字节计入（不崩溃、不静默截断）。
inline int utf8CodepointCount(const std::string &s) {
  int n = 0;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    size_t step = 1;
    if ((c & 0x80) == 0) step = 1;
    else if ((c & 0xE0) == 0xC0) step = 2;
    else if ((c & 0xF0) == 0xE0) step = 3;
    else if ((c & 0xF8) == 0xF0) step = 4;
    if (i + step > s.size()) step = 1;
    i += step;
    ++n;
  }
  return n;
}

// 解析十进制整数，检测溢出。
inline bool parseInt64(const std::string &s, int64_t &out) {
  if (s.empty()) return false;
  size_t i = 0;
  bool neg = false;
  if (s[0] == '+' || s[0] == '-') {
    neg = (s[0] == '-');
    i = 1;
    if (i >= s.size()) return false;
  }
  unsigned long long acc = 0;
  const unsigned long long limit = neg ? (1ULL << 63) : (static_cast<unsigned long long>(INT64_MAX));
  for (; i < s.size(); ++i) {
    char c = s[i];
    if (c < '0' || c > '9') return false;
    unsigned long long d = static_cast<unsigned long long>(c - '0');
    if (acc > (limit - d) / 10ULL) return false;
    acc = acc * 10ULL + d;
  }
  if (neg) {
    if (acc == (1ULL << 63)) out = INT64_MIN;
    else out = -static_cast<int64_t>(acc);
  } else {
    out = static_cast<int64_t>(acc);
  }
  return true;
}

// 解析 double；拒绝 nan/inf 字面量，并检查能否无损落入 float32 有限范围。
inline bool parseFloat32(const std::string &s, float &out) {
  if (s.empty()) return false;
  // 只接受常规十进制（可带指数），拒绝 nan/INF 等词法。
  char *end = nullptr;
  errno = 0;
  double d = std::strtod(s.c_str(), &end);
  if (end == s.c_str() || *end != '\0') return false;
  if (!std::isfinite(d)) return false;
  if (d > static_cast<double>(std::numeric_limits<float>::max()) ||
      d < -static_cast<double>(std::numeric_limits<float>::max())) {
    return false;
  }
  float f = static_cast<float>(d);
  if (!std::isfinite(f)) return false;
  out = f;
  return true;
}

// 快速 float 解析：只接受十进制（可带指数），避免 strtod 的 locale/慢路径。
// 拒绝 nan/inf、hex 及任何非数字字符；并检查 float32 有限范围。
inline bool parseFloatFast(const char *p, const char *end, float &out) {
  const char *q = p;
  while (q < end && (*q == ' ' || *q == '\t')) ++q;
  bool neg = false;
  if (q < end && (*q == '+' || *q == '-')) {
    neg = (*q == '-');
    ++q;
  }
  bool any = false;
  double mant = 0.0;
  int digits = 0;
  while (q < end && *q >= '0' && *q <= '9') {
    mant = mant * 10.0 + (*q - '0');
    ++digits;
    ++q;
    any = true;
    if (digits > 38) return false;
  }
  if (q < end && *q == '.') {
    ++q;
    double scale = 0.1;
    while (q < end && *q >= '0' && *q <= '9') {
      mant += (*q - '0') * scale;
      scale *= 0.1;
      ++q;
      any = true;
      if (++digits > 38) return false;
    }
  }
  if (!any) return false;
  if (q < end && (*q == 'e' || *q == 'E')) {
    ++q;
    bool eneg = false;
    if (q < end && (*q == '+' || *q == '-')) {
      eneg = (*q == '-');
      ++q;
    }
    int exp = 0, ed = 0;
    while (q < end && *q >= '0' && *q <= '9') {
      exp = exp * 10 + (*q - '0');
      ++q;
      if (++ed > 4) return false;
    }
    if (ed == 0) return false;
    mant *= std::pow(10.0, eneg ? -exp : exp);
  }
  while (q < end && (*q == ' ' || *q == '\t')) ++q;
  if (q != end) return false;
  double d = neg ? -mant : mant;
  if (!std::isfinite(d)) return false;
  if (d > static_cast<double>(std::numeric_limits<float>::max()) ||
      d < -static_cast<double>(std::numeric_limits<float>::max())) {
    return false;
  }
  float f = static_cast<float>(d);
  if (!std::isfinite(f)) return false;
  out = f;
  return true;
}

// JSON 字符串转义（输出层使用）。
inline void jsonEscape(const std::string &s, std::string &out) {
  out.push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  out.push_back('"');
}

// 浮点渲染：保证结果看起来像浮点数（带小数点或指数），并保留足够精度。
inline void appendFloat(double d, std::string &out) {
  if (d == 0) {
    out += "0.0";
    return;
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.10g", d);
  out += buf;
  if (!std::strchr(buf, '.') && !std::strchr(buf, 'e') && !std::strchr(buf, 'E')) {
    out += ".0";
  }
}

// 向量渲染：紧凑形式，整数分量不补 .0，形如 [0,2] / [1.5,2]。
inline void appendVector(const float *v, int dim, std::string &out) {
  out.push_back('[');
  char buf[40];
  for (int i = 0; i < dim; ++i) {
    if (i) out.push_back(',');
    std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v[i]));
    out += buf;
  }
  out.push_back(']');
}

inline uint32_t alignUp(uint32_t x, uint32_t a) { return (x + a - 1) / a * a; }

// CRC32（多项式 0xEDB88320），用于 WAL 帧校验。
inline uint32_t crc32(const void *data, size_t len, uint32_t seed = 0) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      table[i] = c;
    }
    init = true;
  }
  uint32_t c = seed ^ 0xFFFFFFFFu;
  const uint8_t *p = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i < len; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

}  // namespace mvd
