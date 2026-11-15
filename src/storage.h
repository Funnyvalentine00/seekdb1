// 存储层：catalog、定长行槽文件、WAL 与崩溃恢复。
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "common.h"
#include "sql.h"

namespace mvd {

// SQL 字面量值（也用于事务内暂存行）。
struct Value {
  enum class Kind { Int, Str, Vec } kind = Kind::Int;
  int64_t i = 0;
  std::string s;
  std::vector<float> v;
};

struct Op {
  uint8_t type = 0;  // 1 insert, 2 update, 3 delete
  uint32_t slot = 0;
  std::vector<uint8_t> data;  // insert/update 的完整槽内容
};

class Table {
 public:
  Table(TableDef def, std::string dir);
  ~Table();

  const TableDef &def() const { return def_; }
  TableDef &def() { return def_; }

  // 启动时打开行文件与 WAL，完成恢复；失败抛 SqlError。
  void open();
  void close();

  uint32_t slotCount() const { return slot_count_; }
  size_t rowCount() const { return id_to_slot_.size(); }
  const uint8_t *slotPtr(uint32_t slot) const {
    return rows_map_ + kHeaderSize + static_cast<size_t>(slot) * def_.slot_size;
  }
  bool slotValid(uint32_t slot) const { return slotPtr(slot)[0] != 0; }
  int64_t slotId(uint32_t slot) const {
    int64_t id;
    std::memcpy(&id, slotPtr(slot) + 8, sizeof(id));
    return id;
  }
  bool findId(int64_t id, uint32_t &slot) const {
    auto it = id_to_slot_.find(id);
    if (it == id_to_slot_.end()) return false;
    slot = it->second;
    return true;
  }

  // 事务提交：先写 WAL 并 fsync，再应用到行文件。
  void commitOps(std::vector<Op> &ops);

  // 构造一条新行的槽内容（不落盘）。
  void buildSlot(const std::vector<Value> &values, std::vector<uint8_t> &out) const;

  // ---- 非主键等值过滤的懒建列桶索引 ----
  // 首次对该列做等值过滤时按当前槽状态全量建桶；任何提交使桶失效，下次查询重建。
  // 返回 nullptr 表示索引不可用（调用方回退全表扫描）；非空指针为候选槽列表
  // （可能为空，表示确无匹配行）。桶建立在提交纪元一致的快照上，候选槽即为匹配行。
  const std::vector<uint32_t> *strBucket(uint32_t col, const std::string &key);
  const std::vector<uint32_t> *intBucket(uint32_t col, int64_t key);

 private:
  void ensureCapacity(uint32_t slots);
  void applyOp(const Op &op);
  void recover();
  void scanRows();
  void resetWal();
  void ensureFilter(uint32_t col);

  struct ColFilter {
    uint64_t epoch = 0;  // 建桶时的提交纪元；不等于当前纪元则失效
    bool valid = false;
    std::unordered_map<std::string, std::vector<uint32_t>> str;
    std::unordered_map<int64_t, std::vector<uint32_t>> ints;
  };

  static constexpr size_t kHeaderSize = 64;
  TableDef def_;
  std::string dir_;
  int rows_fd_ = -1;
  int wal_fd_ = -1;
  uint8_t *rows_map_ = nullptr;
  size_t map_size_ = 0;
  size_t file_size_ = 0;
  size_t capacity_slots_ = 0;
  uint32_t slot_count_ = 0;
  size_t wal_size_ = 0;
  std::unordered_map<int64_t, uint32_t> id_to_slot_;
  std::vector<ColFilter> filters_;
  uint64_t epoch_ = 1;
};

class Storage {
 public:
  explicit Storage(std::string data_dir);
  ~Storage();

  void open();
  void close();

  Table *find(const std::string &name);
  Table *findById(int id);
  Table *require(const std::string &name);
  void createTable(const TableDef &def, bool if_not_exists);
  void dropTable(const std::string &name, bool if_exists);
  std::vector<std::string> tableNames() const;

  void saveCatalog();

 private:
  std::string data_dir_;
  std::string catalog_path_;
  int next_table_id_ = 1;
  std::vector<std::unique_ptr<Table>> tables_;
};

}  // namespace mvd
