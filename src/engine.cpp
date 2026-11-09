#include "engine.h"

#include <algorithm>
#include <cstring>
#include <unordered_set>

#include "vecops.h"

namespace mvd {

namespace {

std::string lower(const std::string &s) {
  std::string r = s;
  for (char &c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return r;
}

int findColumn(const TableDef &def, const std::string &name) {
  std::string l = lower(name);
  for (size_t i = 0; i < def.cols.size(); ++i) {
    if (lower(def.cols[i].name) == l) return static_cast<int>(i);
  }
  return -1;
}

std::vector<float> parseVectorLiteral(const std::string &text, int dim) {
  size_t b = 0, e = text.size();
  while (b < e && std::isspace(static_cast<unsigned char>(text[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(text[e - 1]))) --e;
  if (b >= e || text[b] != '[' || text[e - 1] != ']') {
    throwErr(ErrKind::InvalidVector, "vector literal must look like [1,2,3]");
  }
  std::vector<float> out;
  out.reserve(dim);
  const char *base = text.data();
  size_t i = b + 1;
  size_t end = e - 1;
  // 拒绝尾逗号，如 [1,2,]
  size_t tail = end;
  while (tail > b + 1 && std::isspace(static_cast<unsigned char>(text[tail - 1]))) --tail;
  if (tail > b + 1 && text[tail - 1] == ',') {
    throwErr(ErrKind::InvalidVector, "trailing comma in vector literal");
  }
  while (i < end) {
    size_t j = i;
    while (j < end && text[j] != ',') ++j;
    const char *cp = base + i;
    const char *ce = base + j;
    while (cp < ce && std::isspace(static_cast<unsigned char>(*cp))) ++cp;
    while (ce > cp && std::isspace(static_cast<unsigned char>(ce[-1]))) --ce;
    if (cp == ce) throwErr(ErrKind::InvalidVector, "empty vector component");
    float f;
    if (!parseFloatFast(cp, ce, f)) {
      throwErr(ErrKind::InvalidVector, "invalid vector component");
    }
    out.push_back(f);
    i = j + 1;
  }
  if (static_cast<int>(out.size()) != dim) {
    throwErr(ErrKind::InvalidVector,
             "vector dimension mismatch: expected " + std::to_string(dim) + ", got " +
                 std::to_string(out.size()));
  }
  return out;
}

}  // namespace

Engine::Engine(std::string data_dir) : storage_(std::move(data_dir)) {}
Engine::~Engine() = default;

void Engine::open() { storage_.open(); }
void Engine::shutdown() {
  clearPending();
  storage_.close();
}

void Engine::setVectorMode(const std::string &m) {
  if (m != "exact" && m != "ann") {
    throwErr(ErrKind::InvalidArgument, "vector mode must be exact or ann");
  }
  vector_mode_ = m;
}

bool Engine::tableHasPending(Table &t) {
  auto it = pending_.find(t.def().id);
  if (it == pending_.end()) return false;
  return !it->second.slots.empty() || !it->second.inserts.empty();
}

void Engine::execute(Statement &st, QueryResult &out) {
  out = QueryResult();
  switch (st.kind) {
    case StmtKind::CreateTable: execCreateTable(st, out); break;
    case StmtKind::CreateIndex: execCreateIndex(st, out); break;
    case StmtKind::DropTable: execDropTable(st, out); break;
    case StmtKind::DropIndex: execDropIndex(st, out); break;
    case StmtKind::Insert: execInsert(st, out); break;
    case StmtKind::Update: execUpdate(st, out); break;
    case StmtKind::Delete: execDelete(st, out); break;
    case StmtKind::Select: execSelect(st, out); break;
    case StmtKind::Begin: execBegin(); break;
    case StmtKind::Commit: execCommit(); break;
    case StmtKind::Rollback: execRollback(); break;
    case StmtKind::SetMode: setVectorMode(st.mode); break;
    case StmtKind::ShowTables: {
      out.has_rows = true;
      out.columns = {"table_name"};
      for (auto &n : storage_.tableNames()) {
        std::vector<Cell> row;
        Cell c;
        c.kind = Cell::Kind::Str;
        c.s = n;
        row.push_back(std::move(c));
        out.rows.push_back(std::move(row));
      }
      break;
    }
  }
}

// ---------------- DDL ----------------

void Engine::execCreateTable(Statement &st, QueryResult &out) {
  (void)out;
  if (in_txn_) commitPending();
  if (st.columns.empty()) throwErr(ErrKind::InvalidArgument, "table must have at least one column");
  TableDef def;
  def.name = st.table;
  std::unordered_set<std::string> names;
  int pk_count = 0, vec_count = 0;
  for (const ColumnDef &c : st.columns) {
    std::string l = lower(c.name);
    if (!names.insert(l).second) throwErr(ErrKind::DuplicateColumn, "duplicate column " + c.name);
    if (c.pk) {
      ++pk_count;
      if (c.type != ColType::BigInt) throwErr(ErrKind::InvalidArgument, "primary key must be BIGINT");
    }
    if (c.type == ColType::Vector) ++vec_count;
    def.cols.push_back(c);
  }
  if (pk_count == 0) throwErr(ErrKind::InvalidArgument, "table must have a primary key");
  if (pk_count > 1) throwErr(ErrKind::InvalidArgument, "only one primary key is allowed");
  if (vec_count > 1) throwErr(ErrKind::InvalidArgument, "at most one vector column is allowed");
  for (size_t i = 0; i < def.cols.size(); ++i) {
    if (def.cols[i].pk) def.pk_index = static_cast<int>(i);
  }
  storage_.createTable(def, st.if_not_exists);
}

void Engine::execCreateIndex(Statement &st, QueryResult &out) {
  (void)out;
  if (in_txn_) commitPending();
  Table *t = storage_.require(st.table);
  TableDef &def = t->def();
  for (auto &idx : def.indexes) {
    if (lower(idx.name) == lower(st.index_name)) {
      if (st.if_not_exists) return;
      throwErr(ErrKind::IndexExists, "index already exists: " + st.index_name);
    }
  }
  int ci = findColumn(def, st.index_column);
  if (ci < 0) throwErr(ErrKind::ColumnNotFound, "column not found: " + st.index_column);
  if (def.cols[ci].type != ColType::Vector) {
    throwErr(ErrKind::InvalidArgument, "vector index must be created on a VECTOR column");
  }
  IndexDef idx;
  idx.name = st.index_name;
  idx.column = def.cols[ci].name;
  def.indexes.push_back(idx);
  storage_.saveCatalog();
}

void Engine::execDropTable(Statement &st, QueryResult &out) {
  (void)out;
  if (in_txn_) commitPending();
  storage_.dropTable(st.table, st.if_exists);
}

void Engine::execDropIndex(Statement &st, QueryResult &out) {
  (void)out;
  if (in_txn_) commitPending();
  Table *t = storage_.require(st.table);
  TableDef &def = t->def();
  for (auto it = def.indexes.begin(); it != def.indexes.end(); ++it) {
    if (lower(it->name) == lower(st.index_name)) {
      def.indexes.erase(it);
      storage_.saveCatalog();
      return;
    }
  }
  if (!st.if_exists) throwErr(ErrKind::IndexNotFound, "index not found: " + st.index_name);
}

// ---------------- 事务 ----------------

void Engine::execBegin() {
  if (in_txn_) throwErr(ErrKind::Transaction, "nested transaction is not supported");
  in_txn_ = true;
}

void Engine::execCommit() {
  if (!in_txn_) return;
  commitPending();
  in_txn_ = false;
}

void Engine::execRollback() {
  if (!in_txn_) return;
  clearPending();
  in_txn_ = false;
}

void Engine::clearPending() { pending_.clear(); }

void Engine::commitPending() {
  for (auto &entry : pending_) {
    Table *t = storage_.findById(entry.first);
    if (!t) continue;
    TablePending &p = entry.second;
    std::vector<Op> ops;
    // 先删除
    for (auto &kv : p.slots) {
      if (!kv.second.deleted) continue;
      Op op;
      op.type = 3;
      op.slot = kv.first;
      ops.push_back(std::move(op));
    }
    // 再更新
    for (auto &kv : p.slots) {
      if (kv.second.deleted) continue;
      Op op;
      op.type = 2;
      op.slot = kv.first;
      t->buildSlot(kv.second.values, op.data);
      ops.push_back(std::move(op));
    }
    // 最后插入
    uint32_t next_slot = t->slotCount();
    for (auto &kv : p.inserts) {
      Op op;
      op.type = 1;
      op.slot = next_slot++;
      t->buildSlot(kv.second, op.data);
      ops.push_back(std::move(op));
    }
    if (!ops.empty()) t->commitOps(ops);
  }
  pending_.clear();
}

// ---------------- 行访问 ----------------

int64_t Engine::getInt(const TableDef &def, const RowRef &row, int col) {
  if (row.raw) {
    int64_t v;
    std::memcpy(&v, row.raw + def.col_offset[col], sizeof(v));
    return v;
  }
  return (*row.pend)[col].i;
}

const std::string *Engine::getStr(const TableDef &def, const RowRef &row, int col,
                                  std::string &tmp) {
  if (row.raw) {
    uint32_t len;
    std::memcpy(&len, row.raw + def.col_offset[col], 4);
    tmp.assign(reinterpret_cast<const char *>(row.raw + def.col_offset[col] + 4), len);
    return &tmp;
  }
  return &(*row.pend)[col].s;
}

const float *Engine::getVec(const TableDef &def, const RowRef &row, int col) {
  if (row.raw) {
    return reinterpret_cast<const float *>(row.raw + def.col_offset[col]);
  }
  return (*row.pend)[col].v.data();
}

Value Engine::getValue(const TableDef &def, const RowRef &row, int col) {
  Value v;
  switch (def.cols[col].type) {
    case ColType::BigInt:
      v.kind = Value::Kind::Int;
      v.i = getInt(def, row, col);
      break;
    case ColType::VarChar: {
      std::string tmp;
      const std::string *s = getStr(def, row, col, tmp);
      v.kind = Value::Kind::Str;
      v.s = *s;
      break;
    }
    case ColType::Vector: {
      v.kind = Value::Kind::Vec;
      const float *p = getVec(def, row, col);
      v.v.assign(p, p + def.cols[col].param);
      break;
    }
  }
  return v;
}

int64_t Engine::rowId(const TableDef &def, const RowRef &row) {
  return getInt(def, row, def.pk_index);
}

// ---------------- 绑定 ----------------

void Engine::bindExpr(Table &t, Expr *e) {
  if (!e) return;
  const TableDef &def = t.def();
  switch (e->kind) {
    case ExprKind::IntLit:
    case ExprKind::StrLit:
      break;
    case ExprKind::Column: {
      int ci = findColumn(def, e->col);
      if (ci < 0) throwErr(ErrKind::ColumnNotFound, "column not found: " + e->col);
      e->col_index = ci;
      e->col_type = static_cast<int>(def.cols[ci].type);
      e->col_offset = def.col_offset[ci];
      break;
    }
    case ExprKind::L2Distance: {
      if (!e->left || e->left->kind != ExprKind::Column) {
        throwErr(ErrKind::Unsupported, "L2_DISTANCE first argument must be a column");
      }
      bindExpr(t, e->left.get());
      if (e->left->col_type != static_cast<int>(ColType::Vector)) {
        throwErr(ErrKind::TypeMismatch, "L2_DISTANCE requires a VECTOR column");
      }
      if (!e->right || e->right->kind != ExprKind::StrLit) {
        throwErr(ErrKind::Unsupported, "L2_DISTANCE second argument must be a vector literal");
      }
      e->vec_lit = parseVectorLiteral(e->right->sval, def.cols[e->left->col_index].param);
      break;
    }
    case ExprKind::Compare: {
      bindExpr(t, e->left.get());
      bindExpr(t, e->right.get());
      auto typeOf = [](const Expr *x) -> int {
        if (!x) return -1;
        switch (x->kind) {
          case ExprKind::Column: return x->col_type;
          case ExprKind::IntLit: return static_cast<int>(ColType::BigInt);
          case ExprKind::StrLit: return static_cast<int>(ColType::VarChar);
          case ExprKind::L2Distance: return -3;  // real
          default: return -1;
        }
      };
      int lt = typeOf(e->left.get()), rt = typeOf(e->right.get());
      bool ln = (lt == static_cast<int>(ColType::BigInt) || lt == -3);
      bool rn = (rt == static_cast<int>(ColType::BigInt) || rt == -3);
      if (ln && rn) break;
      if (lt == static_cast<int>(ColType::VarChar) && rt == static_cast<int>(ColType::VarChar)) break;
      throwErr(ErrKind::TypeMismatch, "comparison between incompatible types");
    }
    case ExprKind::And:
      bindExpr(t, e->left.get());
      bindExpr(t, e->right.get());
      break;
  }
}

// ---------------- 求值 ----------------

bool Engine::evalWhere(const TableDef &def, const Expr *e, const RowRef &row) const {
  if (!e) return true;
  switch (e->kind) {
    case ExprKind::And:
      return evalWhere(def, e->left.get(), row) && evalWhere(def, e->right.get(), row);
    case ExprKind::Compare: {
      const Expr *lv = e->left.get();
      const Expr *rv = e->right.get();
      // 快路径：列与字面量直接比较，避免构造 Cell / std::string
      auto cmpResult = [&](int c) -> bool {
        switch (e->cmp) {
          case 0: return c == 0;
          case 1: return c < 0;
          case 2: return c <= 0;
          case 3: return c > 0;
          case 4: return c >= 0;
        }
        return false;
      };
      if (lv->kind == ExprKind::Column && rv->kind == ExprKind::IntLit &&
          def.cols[lv->col_index].type == ColType::BigInt) {
        int64_t v;
        if (row.raw) std::memcpy(&v, row.raw + def.col_offset[lv->col_index], 8);
        else v = (*row.pend)[lv->col_index].i;
        int64_t w = rv->ival;
        return cmpResult(v < w ? -1 : (v > w ? 1 : 0));
      }
      if (rv->kind == ExprKind::Column && lv->kind == ExprKind::IntLit &&
          def.cols[rv->col_index].type == ColType::BigInt) {
        int64_t v;
        if (row.raw) std::memcpy(&v, row.raw + def.col_offset[rv->col_index], 8);
        else v = (*row.pend)[rv->col_index].i;
        int64_t w = lv->ival;
        return cmpResult(w < v ? -1 : (w > v ? 1 : 0));
      }
      if (lv->kind == ExprKind::Column && rv->kind == ExprKind::StrLit &&
          def.cols[lv->col_index].type == ColType::VarChar) {
        const std::string &lit = rv->sval;
        if (row.raw) {
          uint32_t len;
          std::memcpy(&len, row.raw + def.col_offset[lv->col_index], 4);
          const char *p = reinterpret_cast<const char *>(row.raw + def.col_offset[lv->col_index] + 4);
          size_t n = std::min<size_t>(len, lit.size());
          int c = std::memcmp(p, lit.data(), n);
          if (c == 0) c = (len < lit.size()) ? -1 : (len > lit.size() ? 1 : 0);
          return cmpResult(c);
        }
        return cmpResult((*row.pend)[lv->col_index].s.compare(lit));
      }
      if (rv->kind == ExprKind::Column && lv->kind == ExprKind::StrLit &&
          def.cols[rv->col_index].type == ColType::VarChar) {
        const std::string &lit = lv->sval;
        if (row.raw) {
          uint32_t len;
          std::memcpy(&len, row.raw + def.col_offset[rv->col_index], 4);
          const char *p = reinterpret_cast<const char *>(row.raw + def.col_offset[rv->col_index] + 4);
          size_t n = std::min<size_t>(len, lit.size());
          int c = std::memcmp(lit.data(), p, n);
          if (c == 0) c = (len < lit.size()) ? -1 : (len > lit.size() ? 1 : 0);
          return cmpResult(c);
        }
        return cmpResult(-(*row.pend)[rv->col_index].s.compare(lit));
      }
      auto scalar = [&](const Expr *x) -> Cell {
        Cell c;
        if (x->kind == ExprKind::Column) {
          int ci = x->col_index;
          if (def.cols[ci].type == ColType::BigInt) {
            c.kind = Cell::Kind::Int;
            c.i = getInt(def, row, ci);
          } else if (def.cols[ci].type == ColType::VarChar) {
            c.kind = Cell::Kind::Str;
            std::string tmp;
            c.s = *getStr(def, row, ci, tmp);
          } else {
            throwErr(ErrKind::TypeMismatch, "VECTOR cannot be compared directly");
          }
        } else if (x->kind == ExprKind::IntLit) {
          c.kind = Cell::Kind::Int;
          c.i = x->ival;
        } else if (x->kind == ExprKind::StrLit) {
          c.kind = Cell::Kind::Str;
          c.s = x->sval;
        } else if (x->kind == ExprKind::L2Distance) {
          const float *v = getVec(def, row, x->left->col_index);
          c.kind = Cell::Kind::Real;
          c.r = std::sqrt(static_cast<double>(l2Distance(v, x->vec_lit.data(), x->vec_lit.size())));
        } else {
          throwErr(ErrKind::Unsupported, "unsupported expression in WHERE");
        }
        return c;
      };
      Cell a = scalar(e->left.get()), b = scalar(e->right.get());
      if (a.kind == Cell::Kind::Str && b.kind == Cell::Kind::Str) {
        int c = a.s.compare(b.s);
        switch (e->cmp) {
          case 0: return c == 0;
          case 1: return c < 0;
          case 2: return c <= 0;
          case 3: return c > 0;
          case 4: return c >= 0;
        }
      } else if (a.kind != Cell::Kind::Str && b.kind != Cell::Kind::Str) {
        double x = (a.kind == Cell::Kind::Int) ? static_cast<double>(a.i) : a.r;
        double y = (b.kind == Cell::Kind::Int) ? static_cast<double>(b.i) : b.r;
        switch (e->cmp) {
          case 0: return x == y;
          case 1: return x < y;
          case 2: return x <= y;
          case 3: return x > y;
          case 4: return x >= y;
        }
      }
      throwErr(ErrKind::TypeMismatch, "comparison between incompatible types");
    }
    default:
      throwErr(ErrKind::Unsupported, "unsupported WHERE expression");
  }
}

Cell Engine::evalExpr(const TableDef &def, const Expr *e, const RowRef &row) const {
  Cell c;
  switch (e->kind) {
    case ExprKind::Column: {
      int ci = e->col_index;
      switch (def.cols[ci].type) {
        case ColType::BigInt:
          c.kind = Cell::Kind::Int;
          c.i = getInt(def, row, ci);
          break;
        case ColType::VarChar: {
          c.kind = Cell::Kind::Str;
          std::string tmp;
          c.s = *getStr(def, row, ci, tmp);
          break;
        }
        case ColType::Vector: {
          c.kind = Cell::Kind::Vec;
          const float *p = getVec(def, row, ci);
          c.v.assign(p, p + def.cols[ci].param);
          break;
        }
      }
      return c;
    }
    case ExprKind::IntLit:
      c.kind = Cell::Kind::Int;
      c.i = e->ival;
      return c;
    case ExprKind::StrLit:
      c.kind = Cell::Kind::Str;
      c.s = e->sval;
      return c;
    case ExprKind::L2Distance: {
      const float *v = getVec(def, row, e->left->col_index);
      c.kind = Cell::Kind::Real;
      c.r = std::sqrt(static_cast<double>(l2Distance(v, e->vec_lit.data(), e->vec_lit.size())));
      return c;
    }
    default:
      throwErr(ErrKind::Unsupported, "unsupported expression in select list");
  }
}

// ---------------- 可见行 ----------------

void Engine::collectVisible(Table &t, std::vector<RowRef> &rows) {
  rows.clear();
  auto pit = pending_.find(t.def().id);
  size_t extra = (pit == pending_.end()) ? 0 : pit->second.inserts.size();
  rows.reserve(t.slotCount() + extra);
  const uint32_t n = t.slotCount();
  for (uint32_t s = 0; s < n; ++s) {
    if (!t.slotValid(s)) continue;
    if (pit != pending_.end()) {
      auto it = pit->second.slots.find(s);
      if (it != pit->second.slots.end()) {
        if (it->second.deleted) continue;
        RowRef r;
        r.pend = &it->second.values;
        r.slot = s;
        rows.push_back(r);
        continue;
      }
    }
    RowRef r;
    r.raw = t.slotPtr(s);
    r.slot = s;
    rows.push_back(r);
  }
  if (pit != pending_.end()) {
    for (auto &kv : pit->second.inserts) {
      RowRef r;
      r.pend = &kv.second;
      r.slot = UINT32_MAX;
      rows.push_back(r);
    }
  }
}

bool Engine::lookupById(Table &t, int64_t id, RowRef &row, uint32_t &slot) {
  auto pit = pending_.find(t.def().id);
  if (pit != pending_.end()) {
    auto pi = pit->second.inserts.find(id);
    if (pi != pit->second.inserts.end()) {
      row.raw = nullptr;
      row.pend = &pi->second;
      row.slot = UINT32_MAX;
      slot = UINT32_MAX;
      return true;
    }
  }
  uint32_t s;
  if (!t.findId(id, s)) return false;
  if (pit != pending_.end()) {
    auto ps = pit->second.slots.find(s);
    if (ps != pit->second.slots.end()) {
      if (ps->second.deleted) return false;
      row.raw = nullptr;
      row.pend = &ps->second.values;
      row.slot = s;
      slot = s;
      return true;
    }
  }
  row.raw = t.slotPtr(s);
  row.pend = nullptr;
  row.slot = s;
  slot = s;
  return true;
}

void Engine::collectMatching(Table &t, const Expr *where, std::vector<RowRef> &rows,
                             std::vector<uint32_t> *slots, size_t stop_after) {
  rows.clear();
  if (slots) slots->clear();
  const TableDef &def = t.def();
  if (where && where->kind == ExprKind::Compare && where->cmp == 0) {
    const Expr *l = where->left.get();
    const Expr *r = where->right.get();
    const Expr *lit = nullptr;
    if (l->kind == ExprKind::Column && l->col_index == def.pk_index && r->kind == ExprKind::IntLit) {
      lit = r;
    } else if (r->kind == ExprKind::Column && r->col_index == def.pk_index &&
               l->kind == ExprKind::IntLit) {
      lit = l;
    }
    if (lit) {
      RowRef row;
      uint32_t slot;
      if (lookupById(t, lit->ival, row, slot)) {
        rows.push_back(row);
        if (slots) slots->push_back(slot);
      }
      return;
    }
  }
  auto pit = pending_.find(def.id);
  const bool has_pending =
      pit != pending_.end() && (!pit->second.slots.empty() || !pit->second.inserts.empty());
  const uint32_t n = t.slotCount();
  if (!has_pending) {
    // 热路径：无事务暂存时直接扫槽，不做哈希查找
    rows.reserve(std::min<size_t>(n, stop_after == static_cast<size_t>(-1) ? n : stop_after * 2 + 64));
    for (uint32_t s = 0; s < n; ++s) {
      const uint8_t *p = t.slotPtr(s);
      if (p[0] == 0) continue;
      RowRef r;
      r.raw = p;
      r.slot = s;
      if (!evalWhere(def, where, r)) continue;
      rows.push_back(r);
      if (slots) slots->push_back(s);
      if (rows.size() >= stop_after) return;
    }
    return;
  }
  std::vector<RowRef> all;
  collectVisible(t, all);
  rows.reserve(all.size());
  for (auto &r : all) {
    if (!evalWhere(def, where, r)) continue;
    rows.push_back(r);
    if (slots) slots->push_back(r.slot);
    if (rows.size() >= stop_after) return;
  }
}

// ---------------- 值校验 ----------------

void Engine::validateValue(const ColumnDef &col, const Value &v) const {
  switch (col.type) {
    case ColType::BigInt:
      if (v.kind != Value::Kind::Int) throwErr(ErrKind::TypeMismatch, "expected BIGINT for " + col.name);
      break;
    case ColType::VarChar:
      if (v.kind != Value::Kind::Str) throwErr(ErrKind::TypeMismatch, "expected VARCHAR for " + col.name);
      if (utf8CodepointCount(v.s) > col.param) {
        throwErr(ErrKind::ValueOutOfRange, "value too long for " + col.name);
      }
      break;
    case ColType::Vector:
      if (v.kind != Value::Kind::Vec) throwErr(ErrKind::TypeMismatch, "expected VECTOR for " + col.name);
      if (static_cast<int>(v.v.size()) != col.param) {
        throwErr(ErrKind::InvalidVector, "vector dimension mismatch for " + col.name);
      }
      for (float f : v.v) {
        if (!std::isfinite(f)) throwErr(ErrKind::InvalidVector, "non-finite vector component");
      }
      break;
  }
}

Value Engine::parseValueForColumn(const ColumnDef &col, const Expr *e) const {
  Value v;
  switch (col.type) {
    case ColType::BigInt:
      if (e->kind != ExprKind::IntLit) throwErr(ErrKind::TypeMismatch, "expected integer for " + col.name);
      v.kind = Value::Kind::Int;
      v.i = e->ival;
      break;
    case ColType::VarChar:
      if (e->kind != ExprKind::StrLit) throwErr(ErrKind::TypeMismatch, "expected string for " + col.name);
      v.kind = Value::Kind::Str;
      v.s = e->sval;
      break;
    case ColType::Vector:
      if (e->kind != ExprKind::StrLit) {
        throwErr(ErrKind::TypeMismatch, "expected vector literal for " + col.name);
      }
      v.kind = Value::Kind::Vec;
      v.v = parseVectorLiteral(e->sval, col.param);
      break;
  }
  validateValue(col, v);
  return v;
}

std::vector<Value> Engine::buildFullRow(Table &t, const std::vector<ExprPtr> &exprs,
                                        const std::vector<std::string> &cols) const {
  const TableDef &def = t.def();
  std::vector<Value> values(def.cols.size());
  if (cols.empty()) {
    if (exprs.size() != def.cols.size()) {
      throwErr(ErrKind::InvalidArgument, "column count does not match value count");
    }
    for (size_t i = 0; i < def.cols.size(); ++i) {
      values[i] = parseValueForColumn(def.cols[i], exprs[i].get());
    }
    return values;
  }
  if (exprs.size() != cols.size()) throwErr(ErrKind::InvalidArgument, "column count mismatch");
  std::vector<bool> seen(def.cols.size(), false);
  for (size_t i = 0; i < cols.size(); ++i) {
    int ci = findColumn(def, cols[i]);
    if (ci < 0) throwErr(ErrKind::ColumnNotFound, "column not found: " + cols[i]);
    if (seen[ci]) throwErr(ErrKind::DuplicateColumn, "duplicate column in insert: " + cols[i]);
    seen[ci] = true;
    values[ci] = parseValueForColumn(def.cols[ci], exprs[i].get());
  }
  for (size_t i = 0; i < seen.size(); ++i) {
    if (!seen[i]) throwErr(ErrKind::NotNullViolation, "missing value for column " + def.cols[i].name);
  }
  return values;
}

// ---------------- DML ----------------

void Engine::execInsert(Statement &st, QueryResult &out) {
  Table *t = storage_.require(st.table);
  const TableDef &def = t->def();
  TablePending &p = pend(*t);
  std::vector<std::vector<Value>> newRows;
  newRows.reserve(st.insert_rows.size());
  std::unordered_set<int64_t> stmtIds;
  for (auto &exprs : st.insert_rows) {
    std::vector<Value> vals = buildFullRow(*t, exprs, st.insert_columns);
    int64_t id = vals[def.pk_index].i;
    if (!stmtIds.insert(id).second) {
      throwErr(ErrKind::DuplicateKey, "duplicate primary key in statement");
    }
    RowRef existing;
    uint32_t slot;
    if (lookupById(*t, id, existing, slot)) {
      throwErr(ErrKind::DuplicateKey, "duplicate primary key: " + std::to_string(id));
    }
    newRows.push_back(std::move(vals));
  }
  for (auto &vals : newRows) {
    int64_t id = vals[def.pk_index].i;
    p.inserts[id] = std::move(vals);
  }
  out.affected = static_cast<int64_t>(newRows.size());
  if (!in_txn_) commitPending();
}

void Engine::execUpdate(Statement &st, QueryResult &out) {
  Table *t = storage_.require(st.table);
  const TableDef &def = t->def();
  TablePending &p = pend(*t);
  std::vector<int> assign_cols;
  std::vector<Value> assign_vals;
  for (auto &a : st.assigns) {
    int ci = findColumn(def, a.column);
    if (ci < 0) throwErr(ErrKind::ColumnNotFound, "column not found: " + a.column);
    assign_cols.push_back(ci);
    assign_vals.push_back(parseValueForColumn(def.cols[ci], a.value.get()));
  }
  Expr *where = st.where.get();
  if (where) bindExpr(*t, where);

  std::vector<RowRef> rows;
  std::vector<uint32_t> slots;
  collectMatching(*t, where, rows, &slots);

  bool pk_changed = false;
  for (int ci : assign_cols) {
    if (ci == def.pk_index) pk_changed = true;
  }
  std::unordered_set<int64_t> used;
  if (pk_changed) {
    // 主键变更需要与全表可见行比较，而不仅是命中行
    std::vector<RowRef> all;
    collectVisible(*t, all);
    used.reserve(all.size() * 2 + 1);
    for (auto &r : all) used.insert(rowId(def, r));
  }

  struct Change {
    uint32_t slot;
    int64_t old_id;
    std::vector<Value> values;
  };
  std::vector<Change> changes;
  changes.reserve(rows.size());
  for (size_t i = 0; i < rows.size(); ++i) {
    std::vector<Value> vals(def.cols.size());
    for (size_t c = 0; c < def.cols.size(); ++c) {
      vals[c] = getValue(def, rows[i], static_cast<int>(c));
    }
    for (size_t a = 0; a < assign_cols.size(); ++a) vals[assign_cols[a]] = assign_vals[a];
    Change ch;
    ch.slot = slots[i];
    ch.old_id = rowId(def, rows[i]);
    for (size_t c = 0; c < def.cols.size(); ++c) validateValue(def.cols[c], vals[c]);
    if (pk_changed) {
      int64_t new_id = vals[def.pk_index].i;
      if (new_id != ch.old_id) {
        used.erase(ch.old_id);
        if (used.count(new_id)) {
          throwErr(ErrKind::DuplicateKey, "duplicate primary key: " + std::to_string(new_id));
        }
        used.insert(new_id);
      }
    }
    ch.values = std::move(vals);
    changes.push_back(std::move(ch));
  }

  for (auto &ch : changes) {
    if (ch.slot == UINT32_MAX) {
      auto it = p.inserts.find(ch.old_id);
      if (it != p.inserts.end()) {
        std::vector<Value> v = ch.values;
        int64_t new_id = v[def.pk_index].i;
        p.inserts.erase(it);
        p.inserts[new_id] = std::move(v);
      }
    } else {
      PendingSlot ps;
      ps.deleted = false;
      ps.values = ch.values;
      p.slots[ch.slot] = std::move(ps);
    }
  }
  out.affected = static_cast<int64_t>(changes.size());
  if (!in_txn_) commitPending();
}

void Engine::execDelete(Statement &st, QueryResult &out) {
  Table *t = storage_.require(st.table);
  TablePending &p = pend(*t);
  Expr *where = st.where.get();
  if (where) bindExpr(*t, where);
  std::vector<RowRef> rows;
  std::vector<uint32_t> slots;
  collectMatching(*t, where, rows, &slots);
  out.affected = static_cast<int64_t>(rows.size());
  for (size_t i = 0; i < rows.size(); ++i) {
    if (slots[i] == UINT32_MAX) {
      int64_t id = rowId(t->def(), rows[i]);
      p.inserts.erase(id);
    } else {
      PendingSlot ps;
      ps.deleted = true;
      p.slots[slots[i]] = std::move(ps);
    }
  }
  if (!in_txn_) commitPending();
}

// ---------------- SELECT ----------------

void Engine::execSelect(Statement &st, QueryResult &out) {
  Table *t = storage_.require(st.table);
  const TableDef &def = t->def();

  Expr *where = st.where.get();
  if (where) bindExpr(*t, where);

  // 投影描述：star 展开为列，其余为表达式
  struct Spec {
    bool is_col;
    int index;  // 列下标或 select item 下标
  };
  std::vector<Spec> specs;
  out.columns.clear();
  for (size_t i = 0; i < st.select_items.size(); ++i) {
    SelectItem &item = st.select_items[i];
    if (item.star) {
      for (size_t c = 0; c < def.cols.size(); ++c) {
        specs.push_back(Spec{true, static_cast<int>(c)});
        out.columns.push_back(def.cols[c].name);
      }
      continue;
    }
    bindExpr(*t, item.expr.get());
    specs.push_back(Spec{false, static_cast<int>(i)});
    if (item.has_alias) {
      out.columns.push_back(item.alias);
    } else if (item.expr->kind == ExprKind::Column) {
      out.columns.push_back(def.cols[item.expr->col_index].name);
    } else if (item.expr->kind == ExprKind::L2Distance) {
      out.columns.push_back("L2_DISTANCE");
    } else {
      out.columns.push_back("expr");
    }
  }

  // ORDER BY 绑定与别名解析
  for (OrderItem &oi : st.order_by) {
    if (oi.expr && oi.expr->kind == ExprKind::Column) {
      std::string lname = lower(oi.expr->col);
      int found = -1;
      for (size_t i = 0; i < st.select_items.size(); ++i) {
        if (st.select_items[i].has_alias && lower(st.select_items[i].alias) == lname) {
          found = static_cast<int>(i);
          break;
        }
      }
      if (found >= 0) {
        oi.alias_index = found;
        continue;
      }
    }
    bindExpr(*t, oi.expr.get());
  }
  // ORDER BY 的向量列必须在绑定阶段就拒绝（否则只有 0/1 行时不会触发比较）
  for (const OrderItem &oi : st.order_by) {
    const Expr *e = nullptr;
    if (oi.alias_index >= 0) {
      const SelectItem &si = st.select_items[oi.alias_index];
      if (!si.star) e = si.expr.get();
    } else {
      e = oi.expr.get();
    }
    if (e && e->kind == ExprKind::Column && def.cols[e->col_index].type == ColType::Vector) {
      throwErr(ErrKind::TypeMismatch, "VECTOR cannot be used in ORDER BY");
    }
  }

  auto materialize = [&](const RowRef &row) {
    std::vector<Cell> cells;
    cells.reserve(specs.size());
    for (const Spec &sp : specs) {
      if (sp.is_col) {
        Cell c;
        int ci = sp.index;
        switch (def.cols[ci].type) {
          case ColType::BigInt:
            c.kind = Cell::Kind::Int;
            c.i = getInt(def, row, ci);
            break;
          case ColType::VarChar: {
            c.kind = Cell::Kind::Str;
            std::string tmp;
            c.s = *getStr(def, row, ci, tmp);
            break;
          }
          case ColType::Vector: {
            c.kind = Cell::Kind::Vec;
            const float *p = getVec(def, row, ci);
            c.v.assign(p, p + def.cols[ci].param);
            break;
          }
        }
        cells.push_back(std::move(c));
      } else {
        cells.push_back(evalExpr(def, st.select_items[sp.index].expr.get(), row));
      }
    }
    return cells;
  };

  // 距离 TopK 快路径
  const Expr *topk_expr = nullptr;
  if (st.order_by.size() == 1 && !st.order_by[0].desc && st.has_limit) {
    const OrderItem &oi = st.order_by[0];
    if (oi.alias_index >= 0) {
      const SelectItem &si = st.select_items[oi.alias_index];
      if (!si.star && si.expr && si.expr->kind == ExprKind::L2Distance) topk_expr = si.expr.get();
    } else if (oi.expr && oi.expr->kind == ExprKind::L2Distance) {
      topk_expr = oi.expr.get();
    }
  }
  // ORDER BY 直接写 L2_DISTANCE 且未被投影时，也走快路径
  if (topk_expr && st.order_by[0].alias_index < 0 && st.order_by[0].expr &&
      st.order_by[0].expr->kind == ExprKind::L2Distance) {
    topk_expr = st.order_by[0].expr.get();
  }

  std::vector<RowRef> rows;
  out.has_rows = true;
  if (st.has_limit && st.limit == 0) return;

  // 无过滤且无事务暂存：走专用扫描（距离计算内联，避免逐行函数调用）
  if (topk_expr && !where && !tableHasPending(*t)) {
    int64_t k = st.limit;
    if (k <= 0) return;
    std::vector<TopKEntry> entries;
    int vcol = topk_expr->left->col_index;
    scanTopK(t->slotPtr(0), t->slotCount(), def.slot_size, def.col_offset[vcol],
             def.cols[vcol].param, topk_expr->vec_lit.data(), static_cast<int>(k), entries);
    out.rows.reserve(entries.size());
    for (const TopKEntry &e : entries) {
      RowRef rr;
      rr.raw = t->slotPtr(e.slot);
      rr.slot = e.slot;
      out.rows.push_back(materialize(rr));
    }
    return;
  }

  // 无 ORDER BY 时可以用 LIMIT 提前终止扫描
  size_t stop = st.order_by.empty() && st.has_limit ? static_cast<size_t>(st.limit)
                                                    : static_cast<size_t>(-1);
  collectMatching(*t, where, rows, nullptr, stop);

  if (topk_expr) {
    int64_t k = st.limit;
    if (k <= 0) return;
    struct Item {
      float dist;
      int64_t id;
      size_t row;
    };
    auto cmp = [](const Item &a, const Item &b) {
      if (a.dist != b.dist) return a.dist < b.dist;
      return a.id < b.id;
    };
    std::vector<Item> heap;
    heap.reserve(static_cast<size_t>(k) + 1);
    for (size_t i = 0; i < rows.size(); ++i) {
      const float *v = getVec(def, rows[i], topk_expr->left->col_index);
      float d = l2Distance(v, topk_expr->vec_lit.data(), topk_expr->vec_lit.size());
      Item it{d, rowId(def, rows[i]), i};
      if (static_cast<int64_t>(heap.size()) < k) {
        heap.push_back(it);
        std::push_heap(heap.begin(), heap.end(), cmp);
      } else if (d < heap.front().dist || (d == heap.front().dist && it.id < heap.front().id)) {
        std::pop_heap(heap.begin(), heap.end(), cmp);
        heap.back() = it;
        std::push_heap(heap.begin(), heap.end(), cmp);
      }
    }
    std::sort(heap.begin(), heap.end(), cmp);
    out.rows.reserve(heap.size());
    for (const Item &it : heap) out.rows.push_back(materialize(rows[it.row]));
    return;
  }

  std::vector<size_t> order(rows.size());
  for (size_t i = 0; i < rows.size(); ++i) order[i] = i;

  if (!st.order_by.empty()) {
    struct Keyed {
      std::vector<Cell> keys;
      int64_t id;
      size_t row;
    };
    std::vector<Keyed> keyed(rows.size());
    for (size_t i = 0; i < rows.size(); ++i) {
      Keyed &kd = keyed[i];
      kd.row = i;
      kd.id = rowId(def, rows[i]);
      kd.keys.reserve(st.order_by.size());
      for (const OrderItem &oi : st.order_by) {
        if (oi.alias_index >= 0) {
          const SelectItem &si = st.select_items[oi.alias_index];
          if (si.star) throwErr(ErrKind::Unsupported, "ORDER BY alias of * is not supported");
          kd.keys.push_back(evalExpr(def, si.expr.get(), rows[i]));
        } else {
          kd.keys.push_back(evalExpr(def, oi.expr.get(), rows[i]));
        }
      }
    }
    auto cellLess = [](const Cell &a, const Cell &b) -> bool {
      if (a.kind == Cell::Kind::Vec || b.kind == Cell::Kind::Vec) {
        throwErr(ErrKind::TypeMismatch, "VECTOR cannot be used in ORDER BY");
      }
      if (a.kind == Cell::Kind::Int && b.kind == Cell::Kind::Int) return a.i < b.i;
      if (a.kind == Cell::Kind::Str && b.kind == Cell::Kind::Str) return a.s < b.s;
      if (a.kind != Cell::Kind::Str && b.kind != Cell::Kind::Str) {
        double x = (a.kind == Cell::Kind::Int) ? static_cast<double>(a.i) : a.r;
        double y = (b.kind == Cell::Kind::Int) ? static_cast<double>(b.i) : b.r;
        return x < y;
      }
      throwErr(ErrKind::TypeMismatch, "ORDER BY compares incompatible types");
    };
    auto cellEqual = [](const Cell &a, const Cell &b) -> bool {
      if (a.kind == Cell::Kind::Vec || b.kind == Cell::Kind::Vec) {
        throwErr(ErrKind::TypeMismatch, "VECTOR cannot be used in ORDER BY");
      }
      if (a.kind == Cell::Kind::Int && b.kind == Cell::Kind::Int) return a.i == b.i;
      if (a.kind == Cell::Kind::Str && b.kind == Cell::Kind::Str) return a.s == b.s;
      if (a.kind != Cell::Kind::Str && b.kind != Cell::Kind::Str) {
        double x = (a.kind == Cell::Kind::Int) ? static_cast<double>(a.i) : a.r;
        double y = (b.kind == Cell::Kind::Int) ? static_cast<double>(b.i) : b.r;
        return x == y;
      }
      return false;
    };
    std::sort(keyed.begin(), keyed.end(), [&](const Keyed &a, const Keyed &b) {
      for (size_t i = 0; i < st.order_by.size(); ++i) {
        if (cellEqual(a.keys[i], b.keys[i])) continue;
        bool less = cellLess(a.keys[i], b.keys[i]);
        return st.order_by[i].desc ? !less : less;
      }
      return a.id < b.id;
    });
    for (size_t i = 0; i < keyed.size(); ++i) order[i] = keyed[i].row;
  }

  size_t emit = order.size();
  if (st.has_limit) emit = std::min<size_t>(emit, static_cast<size_t>(st.limit));
  out.rows.reserve(emit);
  for (size_t i = 0; i < emit; ++i) out.rows.push_back(materialize(rows[order[i]]));
}

}  // namespace mvd
