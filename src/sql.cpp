#include "sql.h"

#include <cctype>
#include <algorithm>

namespace mvd {

namespace {

std::string lower(const std::string &s) {
  std::string r = s;
  for (char &c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return r;
}

bool isIdentStart(unsigned char c) { return std::isalpha(c) || c == '_'; }
bool isIdentChar(unsigned char c) { return std::isalnum(c) || c == '_' || c == '$'; }

}  // namespace

Token Lexer::next() {
  skipSpaceAndComments();
  Token t;
  t.pos = pos_;
  if (pos_ >= src_.size()) {
    t.kind = TokKind::End;
    return t;
  }
  unsigned char c = static_cast<unsigned char>(src_[pos_]);
  if (isIdentStart(c)) {
    size_t start = pos_;
    while (pos_ < src_.size() && isIdentChar(static_cast<unsigned char>(src_[pos_]))) ++pos_;
    t.kind = TokKind::Ident;
    t.text = src_.substr(start, pos_ - start);
    return t;
  }
  if (std::isdigit(c)) {
    size_t start = pos_;
    while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) ++pos_;
    t.kind = TokKind::Int;
    t.text = src_.substr(start, pos_ - start);
    return t;
  }
  if (c == '\'' || c == '"') {
    char quote = static_cast<char>(c);
    ++pos_;
    std::string val;
    bool closed = false;
    while (pos_ < src_.size()) {
      char ch = src_[pos_];
      if (ch == quote) {
        if (pos_ + 1 < src_.size() && src_[pos_ + 1] == quote) {
          val.push_back(quote);
          pos_ += 2;
          continue;
        }
        ++pos_;
        closed = true;
        break;
      }
      if (ch == '\\' && quote == '"' && pos_ + 1 < src_.size()) {
        // 双引号字符串支持常见转义，方便调试；单引号按标准 SQL 处理。
        char e = src_[pos_ + 1];
        switch (e) {
          case 'n': val.push_back('\n'); break;
          case 't': val.push_back('\t'); break;
          case 'r': val.push_back('\r'); break;
          case '\\': val.push_back('\\'); break;
          case '"': val.push_back('"'); break;
          default: val.push_back(e); break;
        }
        pos_ += 2;
        continue;
      }
      val.push_back(ch);
      ++pos_;
    }
    if (!closed) throwErr(ErrKind::Syntax, "unterminated string literal");
    t.kind = TokKind::Str;
    t.text = val;
    return t;
  }
  // 标点：双字符优先
  if (pos_ + 1 < src_.size()) {
    std::string two = src_.substr(pos_, 2);
    if (two == "<=" || two == ">=" || two == "<>" || two == "!=") {
      t.kind = TokKind::Punct;
      t.text = two;
      pos_ += 2;
      return t;
    }
  }
  if (std::strchr("(),;=<>*.-+/", c)) {
    t.kind = TokKind::Punct;
    t.text = std::string(1, static_cast<char>(c));
    ++pos_;
    return t;
  }
  throwErr(ErrKind::Syntax, std::string("unexpected character '") + static_cast<char>(c) + "'");
}

void Lexer::skipSpaceAndComments() {
  while (pos_ < src_.size()) {
    char c = src_[pos_];
    if (std::isspace(static_cast<unsigned char>(c))) {
      ++pos_;
      continue;
    }
    if (c == '-' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '-') {
      pos_ += 2;
      while (pos_ < src_.size() && src_[pos_] != '\n') ++pos_;
      continue;
    }
    if (c == '#') {
      ++pos_;
      while (pos_ < src_.size() && src_[pos_] != '\n') ++pos_;
      continue;
    }
    if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '*') {
      pos_ += 2;
      bool closed = false;
      while (pos_ + 1 < src_.size()) {
        if (src_[pos_] == '*' && src_[pos_ + 1] == '/') {
          pos_ += 2;
          closed = true;
          break;
        }
        ++pos_;
      }
      if (!closed) throwErr(ErrKind::Syntax, "unterminated comment");
      continue;
    }
    break;
  }
}

void Parser::advance() { cur_ = lex_.next(); }

bool Parser::isPunct(const char *p) const {
  return cur_.kind == TokKind::Punct && cur_.text == p;
}

bool Parser::isKw(const char *kw) const {
  return cur_.kind == TokKind::Ident && lower(cur_.text) == kw;
}

void Parser::expectPunct(const char *p) {
  if (!isPunct(p)) throwErr(ErrKind::Syntax, std::string("expected '") + p + "'");
  advance();
}

void Parser::expectKw(const char *kw) {
  if (!isKw(kw)) throwErr(ErrKind::Syntax, std::string("expected keyword ") + kw);
  advance();
}

std::string Parser::expectIdent() {
  if (cur_.kind != TokKind::Ident) throwErr(ErrKind::Syntax, "expected identifier");
  std::string s = cur_.text;
  advance();
  return s;
}

int64_t Parser::parseIntLit() {
  bool neg = false;
  if (isPunct("-")) {
    neg = true;
    advance();
  }
  if (cur_.kind != TokKind::Int) throwErr(ErrKind::Syntax, "expected integer");
  int64_t v;
  const bool parsed = neg ? parseInt64("-" + cur_.text, v) : parseInt64(cur_.text, v);
  if (!parsed) throwErr(ErrKind::ValueOutOfRange, "integer literal out of range");
  advance();
  return v;
}

ExprPtr Parser::parsePrimary() {
  if (isPunct("(")) {
    advance();
    ExprPtr e = parseAnd();
    expectPunct(")");
    return e;
  }
  if (isPunct("-") || cur_.kind == TokKind::Int) {
    int64_t v = parseIntLit();
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::IntLit;
    e->ival = v;
    return e;
  }
  if (cur_.kind == TokKind::Str) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::StrLit;
    e->sval = cur_.text;
    advance();
    return e;
  }
  if (isPunct("*")) throwErr(ErrKind::Unsupported, "'*' is only supported in the select list");
  if (cur_.kind == TokKind::Ident) {
    std::string name = cur_.text;
    std::string lname = lower(name);
    advance();
    if (isPunct("(")) {
      if (lname != "l2_distance") {
        throwErr(ErrKind::Unsupported, "unsupported function " + name);
      }
      advance();
      ExprPtr col = parsePrimary();
      expectPunct(",");
      ExprPtr lit = parsePrimary();
      expectPunct(")");
      auto e = std::make_unique<Expr>();
      e->kind = ExprKind::L2Distance;
      e->left = std::move(col);
      e->right = std::move(lit);
      return e;
    }
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Column;
    e->col = name;
    return e;
  }
  throwErr(ErrKind::Syntax, "expected expression");
}

ExprPtr Parser::parseCompare() {
  ExprPtr left = parsePrimary();
  int cmp = -1;
  if (isPunct("=")) cmp = 0;
  else if (isPunct("<")) cmp = 1;
  else if (isPunct("<=")) cmp = 2;
  else if (isPunct(">")) cmp = 3;
  else if (isPunct(">=")) cmp = 4;
  else if (isPunct("<>") || isPunct("!=")) throwErr(ErrKind::Unsupported, "operator not supported");
  if (cmp < 0) return left;
  advance();
  ExprPtr right = parsePrimary();
  auto e = std::make_unique<Expr>();
  e->kind = ExprKind::Compare;
  e->cmp = cmp;
  e->left = std::move(left);
  e->right = std::move(right);
  return e;
}

ExprPtr Parser::parseAnd() {
  ExprPtr left = parseCompare();
  while (isKw("and")) {
    advance();
    ExprPtr right = parseCompare();
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::And;
    e->left = std::move(left);
    e->right = std::move(right);
    left = std::move(e);
  }
  return left;
}

ExprPtr Parser::parseExpr() { return parseAnd(); }

ExprPtr Parser::parseValue() {
  // INSERT/UPDATE 的值只允许字面量（向量以字符串形式给出）。
  if (isPunct("-") || cur_.kind == TokKind::Int) {
    int64_t v = parseIntLit();
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::IntLit;
    e->ival = v;
    return e;
  }
  if (cur_.kind == TokKind::Str) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::StrLit;
    e->sval = cur_.text;
    advance();
    return e;
  }
  throwErr(ErrKind::Syntax, "expected literal value");
}

Statement Parser::parse() {
  Statement st;
  if (isKw("create")) {
    advance();
    if (isKw("table")) {
      st.kind = StmtKind::CreateTable;
      advance();
      if (isKw("if")) {
        advance();
        expectKw("not");
        expectKw("exists");
        st.if_not_exists = true;
      }
      st.table = expectIdent();
      expectPunct("(");
      while (true) {
        if (isKw("primary")) {
          advance();
          expectKw("key");
          expectPunct("(");
          std::string pkcol = expectIdent();
          expectPunct(")");
          // 用属性标记回填
          bool found = false;
          for (auto &c : st.columns) {
            if (lower(c.name) == lower(pkcol)) {
              c.pk = true;
              found = true;
            }
          }
          if (!found) {
            // 表级主键可能出现在列定义之前，先记录为特殊列名
            ColumnDef dummy;
            dummy.name = "\x01pk\x01" + pkcol;
            st.columns.push_back(dummy);
          }
        } else {
          ColumnDef c;
          c.name = expectIdent();
          if (cur_.kind != TokKind::Ident) throwErr(ErrKind::Syntax, "expected column type");
          std::string ty = lower(cur_.text);
          advance();
          if (ty == "bigint" || ty == "int" || ty == "integer") {
            c.type = ColType::BigInt;
          } else if (ty == "varchar" || ty == "char") {
            c.type = ColType::VarChar;
            expectPunct("(");
            int64_t n = parseIntLit();
            if (n <= 0 || n > 65535) throwErr(ErrKind::ValueOutOfRange, "VARCHAR length out of range");
            c.param = static_cast<int>(n);
            expectPunct(")");
          } else if (ty == "vector") {
            c.type = ColType::Vector;
            expectPunct("(");
            int64_t n = parseIntLit();
            if (n <= 0 || n > 65536) throwErr(ErrKind::ValueOutOfRange, "VECTOR dimension out of range");
            c.param = static_cast<int>(n);
            expectPunct(")");
          } else {
            throwErr(ErrKind::Unsupported, "unsupported column type " + ty);
          }
          if (isKw("primary")) {
            advance();
            expectKw("key");
            c.pk = true;
          }
          // 忽略 NOT NULL / NULL 等常见修饰，语义上本题不支持 NULL。
          while (isKw("not") || isKw("null") || isKw("unique")) {
            if (isKw("not")) {
              advance();
              if (isKw("null")) advance();
            } else {
              advance();
            }
          }
          st.columns.push_back(std::move(c));
        }
        if (isPunct(",")) {
          advance();
          continue;
        }
        break;
      }
      expectPunct(")");
      // 后置的表级主键回填
      std::vector<ColumnDef> fixed;
      std::string pkname;
      for (auto &c : st.columns) {
        if (!c.name.empty() && c.name[0] == '\x01') {
          pkname = c.name.substr(3);
        } else {
          fixed.push_back(std::move(c));
        }
      }
      if (!pkname.empty()) {
        bool found = false;
        for (auto &c : fixed) {
          if (lower(c.name) == lower(pkname)) {
            c.pk = true;
            found = true;
          }
        }
        if (!found) throwErr(ErrKind::ColumnNotFound, "primary key column not found: " + pkname);
      }
      st.columns = std::move(fixed);
    } else if (isKw("vector") || isKw("index")) {
      if (isKw("vector")) advance();
      st.kind = StmtKind::CreateIndex;
      expectKw("index");
      if (isKw("if")) {
        advance();
        expectKw("not");
        expectKw("exists");
        st.if_not_exists = true;
      }
      st.index_name = expectIdent();
      expectKw("on");
      st.table = expectIdent();
      expectPunct("(");
      st.index_column = expectIdent();
      expectPunct(")");
    } else {
      throwErr(ErrKind::Unsupported, "unsupported CREATE statement");
    }
  } else if (isKw("drop")) {
    advance();
    if (isKw("table")) {
      st.kind = StmtKind::DropTable;
      advance();
      if (isKw("if")) {
        advance();
        expectKw("exists");
        st.if_exists = true;
      }
      st.table = expectIdent();
    } else if (isKw("index")) {
      st.kind = StmtKind::DropIndex;
      advance();
      if (isKw("if")) {
        advance();
        expectKw("exists");
        st.if_exists = true;
      }
      st.index_name = expectIdent();
      expectKw("on");
      st.table = expectIdent();
    } else {
      throwErr(ErrKind::Unsupported, "unsupported DROP statement");
    }
  } else if (isKw("insert")) {
    st.kind = StmtKind::Insert;
    advance();
    expectKw("into");
    st.table = expectIdent();
    if (isPunct("(")) {
      advance();
      while (true) {
        st.insert_columns.push_back(expectIdent());
        if (isPunct(",")) {
          advance();
          continue;
        }
        break;
      }
      expectPunct(")");
    }
    expectKw("values");
    while (true) {
      expectPunct("(");
      std::vector<ExprPtr> row;
      while (true) {
        row.push_back(parseValue());
        if (isPunct(",")) {
          advance();
          continue;
        }
        break;
      }
      expectPunct(")");
      st.insert_rows.push_back(std::move(row));
      if (isPunct(",")) {
        advance();
        continue;
      }
      break;
    }
  } else if (isKw("update")) {
    st.kind = StmtKind::Update;
    advance();
    st.table = expectIdent();
    expectKw("set");
    while (true) {
      AssignItem a;
      a.column = expectIdent();
      expectPunct("=");
      a.value = parseValue();
      st.assigns.push_back(std::move(a));
      if (isPunct(",")) {
        advance();
        continue;
      }
      break;
    }
    if (isKw("where")) {
      advance();
      st.where = parseExpr();
    }
  } else if (isKw("delete")) {
    st.kind = StmtKind::Delete;
    advance();
    expectKw("from");
    st.table = expectIdent();
    if (isKw("where")) {
      advance();
      st.where = parseExpr();
    }
  } else if (isKw("select")) {
    st.kind = StmtKind::Select;
    advance();
    while (true) {
      SelectItem item;
      if (isPunct("*")) {
        item.star = true;
        advance();
      } else {
        item.expr = parseExpr();
        if (isKw("as")) {
          advance();
          item.alias = expectIdent();
          item.has_alias = true;
        } else if (cur_.kind == TokKind::Ident && !isKw("from") && !isKw("order") &&
                   !isKw("limit") && !isKw("where")) {
          item.alias = cur_.text;
          item.has_alias = true;
          advance();
        }
      }
      st.select_items.push_back(std::move(item));
      if (isPunct(",")) {
        advance();
        continue;
      }
      break;
    }
    expectKw("from");
    st.table = expectIdent();
    if (isKw("where")) {
      advance();
      st.where = parseExpr();
    }
    if (isKw("order")) {
      advance();
      expectKw("by");
      while (true) {
        OrderItem oi;
        oi.expr = parseExpr();
        if (isKw("asc")) {
          advance();
        } else if (isKw("desc")) {
          oi.desc = true;
          advance();
        }
        st.order_by.push_back(std::move(oi));
        if (isPunct(",")) {
          advance();
          continue;
        }
        break;
      }
    }
    if (isKw("limit")) {
      advance();
      int64_t n = parseIntLit();
      if (n < 0) throwErr(ErrKind::InvalidArgument, "LIMIT must be non-negative");
      st.limit = n;
      st.has_limit = true;
    }
  } else if (isKw("begin")) {
    st.kind = StmtKind::Begin;
    advance();
    if (isKw("work") || isKw("transaction")) advance();
  } else if (isKw("start")) {
    advance();
    expectKw("transaction");
    st.kind = StmtKind::Begin;
  } else if (isKw("commit")) {
    st.kind = StmtKind::Commit;
    advance();
    if (isKw("work")) advance();
  } else if (isKw("rollback")) {
    st.kind = StmtKind::Rollback;
    advance();
    if (isKw("work")) advance();
  } else if (isKw("set")) {
    advance();
    st.kind = StmtKind::SetMode;
    // SET [@@]vector_mode = 'ann'|'exact'
    std::string name = expectIdent();
    if (name == "@@vector_mode" || lower(name) == "vector_mode") {
      expectPunct("=");
      if (cur_.kind != TokKind::Str && cur_.kind != TokKind::Ident)
        throwErr(ErrKind::Syntax, "expected mode value");
      st.mode = lower(cur_.text);
      advance();
    } else {
      throwErr(ErrKind::Unsupported, "unsupported SET variable " + name);
    }
  } else if (isKw("show")) {
    advance();
    expectKw("tables");
    st.kind = StmtKind::ShowTables;
  } else if (cur_.kind == TokKind::End) {
    throwErr(ErrKind::Syntax, "empty statement");
  } else if (cur_.kind == TokKind::Ident) {
    throwErr(ErrKind::Unsupported, "unsupported statement " + cur_.text);
  } else {
    throwErr(ErrKind::Syntax, "unexpected token");
  }

  if (isPunct(";")) advance();
  if (cur_.kind != TokKind::End) {
    throwErr(ErrKind::Syntax, "unexpected trailing token");
  }
  return st;
}

bool splitStatements(const std::string &buffer, size_t &consumed,
                     std::vector<std::string> &out) {
  size_t i = consumed;
  size_t start = i;
  bool any = false;
  bool inSingle = false, inDouble = false, inLineComment = false, inBlockComment = false;
  while (i < buffer.size()) {
    char c = buffer[i];
    char n = (i + 1 < buffer.size()) ? buffer[i + 1] : '\0';
    if (inLineComment) {
      if (c == '\n') inLineComment = false;
      ++i;
      continue;
    }
    if (inBlockComment) {
      if (c == '*' && n == '/') {
        inBlockComment = false;
        i += 2;
      } else {
        ++i;
      }
      continue;
    }
    if (inSingle) {
      if (c == '\'') {
        if (n == '\'') {
          i += 2;
          continue;
        }
        inSingle = false;
      }
      ++i;
      continue;
    }
    if (inDouble) {
      if (c == '"') {
        if (n == '"') {
          i += 2;
          continue;
        }
        inDouble = false;
      }
      ++i;
      continue;
    }
    if (c == '-' && n == '-') {
      inLineComment = true;
      i += 2;
      continue;
    }
    if (c == '#') {
      inLineComment = true;
      ++i;
      continue;
    }
    if (c == '/' && n == '*') {
      inBlockComment = true;
      i += 2;
      continue;
    }
    if (c == '\'') {
      inSingle = true;
      ++i;
      continue;
    }
    if (c == '"') {
      inDouble = true;
      ++i;
      continue;
    }
    if (c == ';') {
      std::string stmt = buffer.substr(start, i - start);
      // 跳过纯空白/注释语句
      bool hasContent = false;
      for (char ch : stmt) {
        if (!std::isspace(static_cast<unsigned char>(ch))) {
          hasContent = true;
          break;
        }
      }
      if (hasContent) {
        out.push_back(std::move(stmt));
        any = true;
      }
      ++i;
      start = i;
      consumed = i;
      continue;
    }
    ++i;
  }
  // 未闭合的字符串/块注释：等待更多输入
  if (inSingle || inDouble || inBlockComment) {
    return false;
  }
  consumed = start;
  return any;
}

}  // namespace mvd
