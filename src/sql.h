// SQL 子集的词法、AST 与递归下降语法分析。
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common.h"

namespace mvd {

enum class TokKind { Ident, Int, Str, Punct, End };

struct Token {
  TokKind kind = TokKind::End;
  std::string text;
  size_t pos = 0;
};

// 注释与空白由 lexer 跳过。字符串支持 '' 转义。
class Lexer {
 public:
  explicit Lexer(const std::string &src) : src_(src) {}
  Token next();

 private:
  void skipSpaceAndComments();
  const std::string &src_;
  size_t pos_ = 0;
};

enum class ColType { BigInt, VarChar, Vector };

struct ColumnDef {
  std::string name;
  ColType type = ColType::BigInt;
  int param = 0;  // VARCHAR 字符数 / VECTOR 维度
  bool pk = false;
};

struct IndexDef {
  std::string name;
  std::string column;
};

struct TableDef {
  int id = 0;
  std::string name;
  std::vector<ColumnDef> cols;
  std::vector<IndexDef> indexes;
  int pk_index = -1;
  // 运行时布局
  uint32_t slot_size = 0;
  std::vector<uint32_t> col_offset;  // 相对 slot 起始
  uint32_t vec_offset = 0;
  int vector_col = -1;
};

enum class ExprKind { IntLit, StrLit, Column, L2Distance, Compare, And };

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Expr {
  ExprKind kind;
  int64_t ival = 0;
  std::string sval;
  std::string col;       // 列名
  int col_index = -1;    // 解析后回填
  int col_type = -1;     // 绑定后的列类型（ColType 枚举值）
  uint32_t col_offset = 0;
  int cmp = 0;           // 0:= 1:< 2:<= 3:> 4:>=
  std::vector<float> vec_lit;  // L2_DISTANCE 第二个参数解析结果
  ExprPtr left, right;
};

enum class StmtKind {
  CreateTable,
  CreateIndex,
  DropTable,
  DropIndex,
  Insert,
  Update,
  Delete,
  Select,
  Begin,
  Commit,
  Rollback,
  SetMode,
  ShowTables,
};

struct SelectItem {
  bool star = false;
  ExprPtr expr;
  std::string alias;
  bool has_alias = false;
};

struct OrderItem {
  ExprPtr expr;
  bool desc = false;
  int alias_index = -1;  // ORDER BY 引用投影别名时回填
};

struct AssignItem {
  std::string column;
  ExprPtr value;
};

struct Statement {
  StmtKind kind;
  std::string table;
  // CREATE TABLE
  std::vector<ColumnDef> columns;
  bool if_not_exists = false;
  bool if_exists = false;
  // CREATE/DROP INDEX
  std::string index_name;
  std::string index_column;
  // INSERT
  std::vector<std::string> insert_columns;
  std::vector<std::vector<ExprPtr>> insert_rows;
  // UPDATE
  std::vector<AssignItem> assigns;
  // DELETE / SELECT
  ExprPtr where;
  std::vector<SelectItem> select_items;
  std::vector<OrderItem> order_by;
  bool has_limit = false;
  int64_t limit = -1;
  // SET
  std::string mode;
};

// 解析器。parse() 解析一条完整语句；多余 token 视为语法错误。
class Parser {
 public:
  explicit Parser(const std::string &sql) : lex_(sql) { advance(); }
  Statement parse();

 private:
  void advance();
  bool isPunct(const char *p) const;
  bool isKw(const char *kw) const;
  void expectPunct(const char *p);
  void expectKw(const char *kw);
  std::string expectIdent();
  ExprPtr parseExpr();
  ExprPtr parsePrimary();
  ExprPtr parseCompare();
  ExprPtr parseAnd();
  ExprPtr parseValue();
  int64_t parseIntLit();

  Lexer lex_;
  Token cur_;
};

// 把语句拆成单条：处理引号、注释与分号。返回值表示是否解析出完整语句。
bool splitStatements(const std::string &buffer, size_t &consumed,
                     std::vector<std::string> &out);

}  // namespace mvd
