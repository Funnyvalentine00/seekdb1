// JSON Lines 请求解析。id 保留为原始 JSON，避免数值转换损失精度。
#pragma once

#include <optional>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "common.h"

namespace mvd::stdio {

struct Request {
  std::string_view id;  // 引用当前输入行
  std::string sql;
  std::string search_mode = "exact";
  bool shutdown = false;
};

// 返回首个 UTF-8 码点的字节数，非法序列返回 0。
inline size_t utf8Length(std::string_view text) {
  const auto lead = static_cast<unsigned char>(text.front());
  if (lead < 0x80) return 1;
  if (lead < 0xc2 || lead > 0xf4) return 0;
  const size_t length = lead < 0xe0 ? 2 : (lead < 0xf0 ? 3 : 4);
  if (text.size() < length) return 0;
  uint32_t codepoint = lead & (0x7f >> length);
  for (size_t i = 1; i < length; ++i) {
    const auto byte = static_cast<unsigned char>(text[i]);
    if ((byte & 0xc0) != 0x80) return 0;
    codepoint = (codepoint << 6) | (byte & 0x3f);
  }
  if ((length == 3 && codepoint < 0x800) || (length == 4 && codepoint < 0x10000) ||
      (codepoint >= 0xd800 && codepoint <= 0xdfff) || codepoint > 0x10ffff) return 0;
  return length;
}

// 错误消息可能含有词法分析器截取的不完整 UTF-8 码点。
inline void escapeString(const std::string &value, std::string &out) {
  std::string_view tail(value);
  while (!tail.empty()) {
    const size_t length = utf8Length(tail);
    if (length == 0) break;
    tail.remove_prefix(length);
  }
  if (tail.empty()) {
    jsonEscape(value, out);
    return;
  }
  std::string valid = value.substr(0, value.size() - tail.size());
  while (!tail.empty()) {
    const size_t length = utf8Length(tail);
    if (length == 0) {
      valid += "\xef\xbf\xbd";
      tail.remove_prefix(1);
    } else {
      valid.append(tail.substr(0, length));
      tail.remove_prefix(length);
    }
  }
  jsonEscape(valid, out);
}

class RequestParser {
 public:
  explicit RequestParser(std::string_view line) : line_(line) {}

  void parse(Request &request) {
    std::optional<std::string> op, sql, mode;
    bool valid_types = true;
    skipSpace();
    readObject([&](const std::string &key) {
      if (key == "id") {
        const size_t start = pos_;
        readValue(1);
        request.id = line_.substr(start, pos_ - start);
        return;
      }
      std::optional<std::string> *field = nullptr;
      if (key == "op") field = &op;
      else if (key == "sql") field = &sql;
      else if (key == "search_mode") field = &mode;
      if (field && peek() == '"') {
        *field = readString();
      } else {
        readValue(1);
        if (field) valid_types = false;
      }
    });
    skipSpace();
    if (pos_ != line_.size()) invalidJson();

    if (!valid_types) {
      throwErr(ErrKind::InvalidArgument, "op, sql and search_mode must be strings when present");
    }
    if (request.id.empty() || !op) {
      throwErr(ErrKind::InvalidArgument, "request requires id and a string op");
    }
    if (*op == "shutdown") {
      request.shutdown = true;
      return;
    }
    if (*op != "execute" || !sql) {
      throwErr(ErrKind::InvalidArgument, "expected execute with a SQL string or shutdown");
    }
    request.sql = std::move(*sql);
    if (mode) request.search_mode = std::move(*mode);
  }

 private:
  [[noreturn]] static void invalidJson() {
    throwErr(ErrKind::InvalidArgument, "invalid JSON request");
  }

  char peek() const { return pos_ < line_.size() ? line_[pos_] : '\0'; }

  bool take(char expected) {
    if (peek() != expected) return false;
    ++pos_;
    return true;
  }

  void expect(char expected) {
    if (!take(expected)) invalidJson();
  }

  void skipSpace() {
    while (peek() == ' ' || peek() == '\t' || peek() == '\r' || peek() == '\n') ++pos_;
  }

  template <typename ReadMember>
  void readObject(ReadMember readMember) {
    expect('{');
    skipSpace();
    if (take('}')) return;
    std::unordered_set<std::string> keys;
    do {
      auto [key, inserted] = keys.insert(readString());
      if (!inserted) invalidJson();
      skipSpace();
      expect(':');
      skipSpace();
      readMember(*key);
      skipSpace();
      if (take('}')) return;
      expect(',');
      skipSpace();
    } while (true);
  }

  void readValue(int depth) {
    if (depth > 256) invalidJson();
    switch (peek()) {
      case '"': readString(); break;
      case '{': readObject([&](const std::string &) { readValue(depth + 1); }); break;
      case '[':
        expect('[');
        skipSpace();
        if (take(']')) break;
        do {
          readValue(depth + 1);
          skipSpace();
          if (take(']')) break;
          expect(',');
          skipSpace();
        } while (true);
        break;
      case 'n': readLiteral("null"); break;
      case 't': readLiteral("true"); break;
      case 'f': readLiteral("false"); break;
      default: readNumber(); break;
    }
  }

  void readLiteral(std::string_view literal) {
    if (line_.substr(pos_, literal.size()) != literal) invalidJson();
    pos_ += literal.size();
  }

  void readDigits() {
    if (peek() < '0' || peek() > '9') invalidJson();
    do {
      ++pos_;
    } while (peek() >= '0' && peek() <= '9');
  }

  void readNumber() {
    take('-');
    if (!take('0')) readDigits();
    if (take('.')) readDigits();
    if (take('e') || take('E')) {
      if (!take('+')) take('-');
      readDigits();
    }
  }

  uint32_t readHexQuad() {
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = peek();
      value <<= 4;
      if (c >= '0' && c <= '9') value |= c - '0';
      else if (c >= 'a' && c <= 'f') value |= c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') value |= c - 'A' + 10;
      else invalidJson();
      ++pos_;
    }
    return value;
  }

  void readUnicodeEscape(std::string &out) {
    uint32_t codepoint = readHexQuad();
    if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
      expect('\\');
      expect('u');
      const uint32_t low = readHexQuad();
      if (low < 0xdc00 || low > 0xdfff) invalidJson();
      codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
    } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
      invalidJson();
    }
    if (codepoint <= 0x7f) {
      out.push_back(static_cast<char>(codepoint));
      return;
    }
    const int tail = codepoint <= 0x7ff ? 1 : (codepoint <= 0xffff ? 2 : 3);
    const int prefix = tail == 1 ? 0xc0 : (tail == 2 ? 0xe0 : 0xf0);
    out.push_back(static_cast<char>(prefix | (codepoint >> (6 * tail))));
    for (int i = tail - 1; i >= 0; --i) {
      out.push_back(static_cast<char>(0x80 | ((codepoint >> (6 * i)) & 0x3f)));
    }
  }

  std::string readString() {
    expect('"');
    std::string value;
    while (!take('"')) {
      const auto byte = static_cast<unsigned char>(peek());
      if (byte < 0x20) invalidJson();
      if (take('\\')) {
        const char escaped = peek();
        ++pos_;
        switch (escaped) {
          case '"': case '\\': case '/': value.push_back(escaped); break;
          case 'b': value.push_back('\b'); break;
          case 'f': value.push_back('\f'); break;
          case 'n': value.push_back('\n'); break;
          case 'r': value.push_back('\r'); break;
          case 't': value.push_back('\t'); break;
          case 'u': readUnicodeEscape(value); break;
          default: invalidJson();
        }
      } else if (byte < 0x80) {
        value.push_back(static_cast<char>(byte));
        ++pos_;
      } else {
        const size_t length = utf8Length(line_.substr(pos_));
        if (length == 0) invalidJson();
        value.append(line_.substr(pos_, length));
        pos_ += length;
      }
    }
    return value;
  }

  std::string_view line_;
  size_t pos_ = 0;
};

}  // namespace mvd::stdio
