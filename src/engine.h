// 执行引擎：DDL/DML/查询/事务。
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "common.h"
#include "sql.h"
#include "storage.h"

namespace mvd {

struct Cell {
  enum class Kind { Int, Str, Real, Vec } kind = Kind::Int;
  int64_t i = 0;
  double r = 0;
  std::string s;
  std::vector<float> v;
};

struct QueryResult {
  bool has_rows = false;
  std::vector<std::string> columns;
  std::vector<std::vector<Cell>> rows;
  int64_t affected = -1;
};

class Engine {
 public:
  explicit Engine(std::string data_dir);
  ~Engine();

  void open();
  void shutdown();

  void execute(Statement &st, QueryResult &out);

  const std::string &vectorMode() const { return vector_mode_; }
  void setVectorMode(const std::string &m);

 private:
  struct RowRef {
    const uint8_t *raw = nullptr;              // 基表行
    const std::vector<Value> *pend = nullptr;  // 事务内新行/改后行
    uint32_t slot = UINT32_MAX;                // 基表槽位；事务新插入为 UINT32_MAX
  };

  struct PendingSlot {
    bool deleted = false;
    std::vector<Value> values;
  };

  struct TablePending {
    std::unordered_map<uint32_t, PendingSlot> slots;
    std::unordered_map<int64_t, std::vector<Value>> inserts;
  };

  TablePending &pend(Table &t) { return pending_[t.def().id]; }
  bool tableHasPending(Table &t);

  // DDL
  void execCreateTable(Statement &st, QueryResult &out);
  void execCreateIndex(Statement &st, QueryResult &out);
  void execDropTable(Statement &st, QueryResult &out);
  void execDropIndex(Statement &st, QueryResult &out);

  // DML
  void execInsert(Statement &st, QueryResult &out);
  void execUpdate(Statement &st, QueryResult &out);
  void execDelete(Statement &st, QueryResult &out);
  void execSelect(Statement &st, QueryResult &out);

  // 事务
  void execBegin();
  void execCommit();
  void execRollback();
  void commitPending();
  void clearPending();

  // 绑定与求值
  void bindExpr(Table &t, Expr *e);
  bool evalWhere(const TableDef &def, const Expr *e, const RowRef &row) const;
  Cell evalExpr(const TableDef &def, const Expr *e, const RowRef &row) const;

  // 行访问
  static int64_t getInt(const TableDef &def, const RowRef &row, int col);
  static const std::string *getStr(const TableDef &def, const RowRef &row, int col,
                                   std::string &tmp);
  static const float *getVec(const TableDef &def, const RowRef &row, int col);
  static Value getValue(const TableDef &def, const RowRef &row, int col);
  static int64_t rowId(const TableDef &def, const RowRef &row);

  // 可见行遍历
  void collectVisible(Table &t, std::vector<RowRef> &rows);
  bool lookupById(Table &t, int64_t id, RowRef &row, uint32_t &slot);
  void collectMatching(Table &t, const Expr *where, std::vector<RowRef> &rows,
                       std::vector<uint32_t> *slots, size_t stop_after = static_cast<size_t>(-1));

  // 值校验
  void validateValue(const ColumnDef &col, const Value &v) const;
  Value parseValueForColumn(const ColumnDef &col, const Expr *e) const;
  std::vector<Value> buildFullRow(Table &t, const std::vector<ExprPtr> &exprs,
                                  const std::vector<std::string> &cols) const;

  Storage storage_;
  std::string vector_mode_ = "exact";
  bool in_txn_ = false;
  std::unordered_map<int, TablePending> pending_;
};

}  // namespace mvd
