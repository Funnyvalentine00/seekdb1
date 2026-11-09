// MiniVectorDB 进程入口：stdin 收 SQL，stdout 回结果，日志走 stderr。
#include <signal.h>
#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "common.h"
#include "engine.h"
#include "sql.h"

namespace {

struct Config {
  std::string data_dir = "/data";
  std::string vector_mode = "exact";
  std::string ready_marker;
  std::string format = "json";     // json | tsv
  std::string separator = " | ";   // tsv 分隔符
  bool header = true;              // tsv 表头
  bool numeric_errors = false;
};

void usage() {
  std::fprintf(stderr,
               "usage: minivectordb [--data-dir=DIR] [--vector-mode=exact|ann]\n"
               "                    [--format=json|tsv] [--separator=STR] [--header=on|off]\n"
               "                    [--ready-marker=TOKEN] [--numeric-errors]\n");
}

bool parseArgs(int argc, char **argv, Config &cfg) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto val = [&](const char *prefix) -> std::string {
      size_t n = std::strlen(prefix);
      if (a.compare(0, n, prefix) == 0) return a.substr(n);
      return std::string();
    };
    if (a == "--help" || a == "-h") {
      usage();
      return false;
    }
    // 同时支持 --opt=value 与 --opt value 两种写法
    if (a == "--data-dir" && i + 1 < argc) {
      cfg.data_dir = argv[++i];
      continue;
    }
    if (a == "--vector-mode" && i + 1 < argc) {
      cfg.vector_mode = argv[++i];
      continue;
    }
    if (a == "--format" && i + 1 < argc) {
      cfg.format = argv[++i];
      continue;
    }
    if (a == "--separator" && i + 1 < argc) {
      cfg.separator = argv[++i];
      continue;
    }
    if (a == "--header" && i + 1 < argc) {
      cfg.header = std::string(argv[++i]) != "off";
      continue;
    }
    if (a == "--ready-marker" && i + 1 < argc) {
      cfg.ready_marker = argv[++i];
      continue;
    }
    if (a.rfind("--data-dir=", 0) == 0) {
      cfg.data_dir = val("--data-dir=");
    } else if (a.rfind("--vector-mode=", 0) == 0) {
      cfg.vector_mode = val("--vector-mode=");
    } else if (a.rfind("--mode=", 0) == 0) {
      cfg.vector_mode = val("--mode=");
    } else if (a.rfind("--format=", 0) == 0) {
      cfg.format = val("--format=");
    } else if (a.rfind("--separator=", 0) == 0) {
      cfg.separator = val("--separator=");
    } else if (a.rfind("--header=", 0) == 0) {
      cfg.header = (val("--header=") != "off");
    } else if (a.rfind("--ready-marker=", 0) == 0) {
      cfg.ready_marker = val("--ready-marker=");
    } else if (a == "--numeric-errors") {
      cfg.numeric_errors = true;
    } else if (a == "--ann") {
      cfg.vector_mode = "ann";
    } else if (a == "--exact") {
      cfg.vector_mode = "exact";
    } else if (a.rfind("--", 0) == 0) {
      std::fprintf(stderr, "unknown option: %s\n", a.c_str());
      return false;
    }
  }
  return true;
}

void renderCellJson(const mvd::Cell &c, std::string &out) {
  using K = mvd::Cell::Kind;
  switch (c.kind) {
    case K::Int: out += std::to_string(c.i); break;
    case K::Str: mvd::jsonEscape(c.s, out); break;
    case K::Real: mvd::appendFloat(c.r, out); break;
    case K::Vec: {
      std::string v;
      mvd::appendVector(c.v.data(), static_cast<int>(c.v.size()), v);
      mvd::jsonEscape(v, out);
      break;
    }
  }
}

void renderJson(const mvd::QueryResult &r, std::string &out) {
  if (r.has_rows) {
    out += "{\"columns\":[";
    for (size_t i = 0; i < r.columns.size(); ++i) {
      if (i) out.push_back(',');
      mvd::jsonEscape(r.columns[i], out);
    }
    out += "],\"rows\":[";
    for (size_t i = 0; i < r.rows.size(); ++i) {
      if (i) out.push_back(',');
      out.push_back('[');
      const auto &row = r.rows[i];
      for (size_t j = 0; j < row.size(); ++j) {
        if (j) out.push_back(',');
        renderCellJson(row[j], out);
      }
      out.push_back(']');
    }
    out += "]}";
  } else if (r.affected >= 0) {
    out += "{\"affected_rows\":" + std::to_string(r.affected) + "}";
  } else {
    out += "{}";
  }
}

void renderTsvCell(const mvd::Cell &c, std::string &out) {
  using K = mvd::Cell::Kind;
  switch (c.kind) {
    case K::Int: out += std::to_string(c.i); break;
    case K::Str: out += c.s; break;
    case K::Real: mvd::appendFloat(c.r, out); break;
    case K::Vec: mvd::appendVector(c.v.data(), static_cast<int>(c.v.size()), out); break;
  }
}

void renderTsv(const mvd::QueryResult &r, const Config &cfg, std::string &out) {
  if (r.has_rows) {
    if (cfg.header) {
      for (size_t i = 0; i < r.columns.size(); ++i) {
        if (i) out += cfg.separator;
        out += r.columns[i];
      }
      out.push_back('\n');
    }
    for (const auto &row : r.rows) {
      for (size_t j = 0; j < row.size(); ++j) {
        if (j) out += cfg.separator;
        renderTsvCell(row[j], out);
      }
      out.push_back('\n');
    }
    if (!out.empty() && out.back() == '\n') out.pop_back();
  } else if (r.affected >= 0) {
    out += "OK " + std::to_string(r.affected);
  } else {
    out += "OK";
  }
}

void renderError(const mvd::SqlError &e, const Config &cfg, std::string &out) {
  if (cfg.format == "tsv") {
    if (cfg.numeric_errors) {
      out += "ERROR " + std::to_string(static_cast<int>(e.kind)) + ": " + e.msg;
    } else {
      out += std::string("ERROR ") + mvd::errName(e.kind) + ": " + e.msg;
    }
    return;
  }
  out += "{\"error\":";
  mvd::jsonEscape(mvd::errName(e.kind), out);
  out += ",\"message\":";
  mvd::jsonEscape(e.msg, out);
  if (cfg.numeric_errors) {
    out += ",\"error_code\":" + std::to_string(static_cast<int>(e.kind));
  }
  out += "}";
}

void emit(const std::string &line) {
  std::fwrite(line.data(), 1, line.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

bool blankSql(const std::string &s) {
  for (char c : s) {
    if (!std::isspace(static_cast<unsigned char>(c))) return false;
  }
  return true;
}

}  // namespace

const char *envOr(const char *name, const char *fallback) {
  const char *v = std::getenv(name);
  return (v && *v) ? v : fallback;
}

void onTerminate(int) {
  // 已确认提交的数据在 WAL 中已 fsync；未提交数据只在内存，直接退出等价于回滚。
  _exit(0);
}

int main(int argc, char **argv) {
  ::signal(SIGPIPE, SIG_IGN);
  ::signal(SIGTERM, onTerminate);
  ::signal(SIGINT, onTerminate);
  Config cfg;
  // 环境变量兜底：驱动可能通过环境而不是命令行传参
  cfg.data_dir = envOr("MVD_DATA_DIR", cfg.data_dir.c_str());
  cfg.vector_mode = envOr("MVD_VECTOR_MODE", cfg.vector_mode.c_str());
  cfg.ready_marker = envOr("MVD_READY_MARKER", "");
  cfg.format = envOr("MVD_FORMAT", cfg.format.c_str());
  cfg.separator = envOr("MVD_SEPARATOR", cfg.separator.c_str());
  cfg.header = std::string(envOr("MVD_HEADER", "on")) != "off";
  if (!parseArgs(argc, argv, cfg)) return 2;

  mvd::Engine engine(cfg.data_dir);
  try {
    engine.open();
    engine.setVectorMode(cfg.vector_mode);
  } catch (const mvd::SqlError &e) {
    std::fprintf(stderr, "startup failed: %s: %s\n", mvd::errName(e.kind), e.msg.c_str());
    return 1;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "startup failed: %s\n", e.what());
    return 1;
  }

  if (!cfg.ready_marker.empty()) emit(cfg.ready_marker);

  bool stop = false;
  auto runSql = [&](const std::string &sql) {
    if (blankSql(sql)) return;
    // 兼容驱动可能发送的退出指令
    std::string trimmed;
    for (char c : sql) {
      if (!std::isspace(static_cast<unsigned char>(c))) {
        trimmed.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
      }
    }
    if (trimmed == "exit" || trimmed == "quit" || trimmed == "\\q") {
      stop = true;
      return;
    }
    std::string out;
    try {
      mvd::Parser parser(sql);
      mvd::Statement st = parser.parse();
      mvd::QueryResult res;
      engine.execute(st, res);
      if (cfg.format == "tsv") renderTsv(res, cfg, out);
      else renderJson(res, out);
    } catch (const mvd::SqlError &e) {
      if (std::strcmp(e.msg.c_str(), "empty statement") == 0) return;
      renderError(e, cfg, out);
    } catch (const std::exception &e) {
      mvd::SqlError err(mvd::ErrKind::Internal, e.what());
      renderError(err, cfg, out);
    }
    emit(out);
  };

  std::string buffer;
  std::string line;
  bool first = true;
  while (!stop && std::getline(std::cin, line)) {
    buffer += line;
    buffer.push_back('\n');
    if (first) {
      first = false;
      // 剥离 UTF-8 BOM
      if (buffer.size() >= 3 && static_cast<unsigned char>(buffer[0]) == 0xEF &&
          static_cast<unsigned char>(buffer[1]) == 0xBB &&
          static_cast<unsigned char>(buffer[2]) == 0xBF) {
        buffer.erase(0, 3);
      }
    }
    std::vector<std::string> stmts;
    size_t consumed = 0;
    mvd::splitStatements(buffer, consumed, stmts);
    if (consumed > 0) buffer.erase(0, consumed);
    for (const std::string &sql : stmts) {
      runSql(sql);
      if (stop) break;
    }
  }
  // 输入结束时若还有未以分号结尾的语句，也执行掉
  if (!stop && !blankSql(buffer)) runSql(buffer);

  // EOF：回滚未提交事务并正常落盘
  try {
    mvd::Statement rollback;
    rollback.kind = mvd::StmtKind::Rollback;
    mvd::QueryResult ignored;
    engine.execute(rollback, ignored);
  } catch (...) {
  }
  engine.shutdown();
  return 0;
}
