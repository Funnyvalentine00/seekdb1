#include "storage.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace mvd {

namespace {

constexpr char kRowsMagic[8] = {'M', 'V', 'D', 'R', 'O', 'W', 'S', '1'};
constexpr uint64_t kFrameMagic = 0x4D5644434F4D4D49ULL;  // "MVDCOMMI"

void writeAll(int fd, const void *buf, size_t len, off_t off) {
  const uint8_t *p = static_cast<const uint8_t *>(buf);
  size_t done = 0;
  while (done < len) {
    ssize_t n = ::pwrite(fd, p + done, len - done, off + static_cast<off_t>(done));
    if (n <= 0) throwErr(ErrKind::Io, "pwrite failed");
    done += static_cast<size_t>(n);
  }
}

std::string tableFileName(const std::string &dir, int id, const char *suffix) {
  return dir + "/t_" + std::to_string(id) + suffix;
}

std::string lowerName(const std::string &s) {
  std::string r = s;
  for (char &c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return r;
}

}  // namespace

Table::Table(TableDef def, std::string dir) : def_(std::move(def)), dir_(std::move(dir)) {
  // 计算列偏移与槽大小
  uint32_t off = 16;  // valid(1)+pad(3)+version(4)+id(8)
  def_.col_offset.assign(def_.cols.size(), 0);
  for (size_t i = 0; i < def_.cols.size(); ++i) {
    const ColumnDef &c = def_.cols[i];
    switch (c.type) {
      case ColType::BigInt:
        off = alignUp(off, 8);
        def_.col_offset[i] = off;
        off += 8;
        break;
      case ColType::VarChar:
        off = alignUp(off, 4);
        def_.col_offset[i] = off;
        off += 4 + 4u * static_cast<uint32_t>(c.param);
        break;
      case ColType::Vector:
        off = alignUp(off, 64);
        def_.col_offset[i] = off;
        def_.vec_offset = off;
        def_.vector_col = static_cast<int>(i);
        off += 4u * static_cast<uint32_t>(c.param);
        break;
    }
  }
  def_.slot_size = alignUp(off, 64);
}

Table::~Table() { close(); }

void Table::open() {
  std::string rows_path = tableFileName(dir_, def_.id, ".rows");
  std::string wal_path = tableFileName(dir_, def_.id, ".wal");
  rows_fd_ = ::open(rows_path.c_str(), O_RDWR | O_CREAT, 0644);
  if (rows_fd_ < 0) throwErr(ErrKind::Io, "cannot open " + rows_path);
  wal_fd_ = ::open(wal_path.c_str(), O_RDWR | O_CREAT, 0644);
  if (wal_fd_ < 0) throwErr(ErrKind::Io, "cannot open " + wal_path);

  struct stat st{};
  if (::fstat(rows_fd_, &st) != 0) throwErr(ErrKind::Io, "fstat failed");
  if (st.st_size < static_cast<off_t>(kHeaderSize)) {
    uint8_t header[kHeaderSize] = {0};
    std::memcpy(header, kRowsMagic, 8);
    uint32_t ss = def_.slot_size;
    std::memcpy(header + 8, &ss, 4);
    uint32_t z = 0;
    std::memcpy(header + 12, &z, 4);
    if (::ftruncate(rows_fd_, kHeaderSize) != 0) throwErr(ErrKind::Io, "ftruncate failed");
    writeAll(rows_fd_, header, kHeaderSize, 0);
    ::fsync(rows_fd_);
  }
  if (::fstat(rows_fd_, &st) != 0) throwErr(ErrKind::Io, "fstat failed");
  file_size_ = static_cast<size_t>(st.st_size);
  if (file_size_ < kHeaderSize) file_size_ = kHeaderSize;
  map_size_ = file_size_;
  rows_map_ = static_cast<uint8_t *>(
      ::mmap(nullptr, map_size_, PROT_READ | PROT_WRITE, MAP_SHARED, rows_fd_, 0));
  if (rows_map_ == MAP_FAILED) {
    rows_map_ = nullptr;
    throwErr(ErrKind::Io, "mmap rows failed");
  }
  uint32_t ss = 0, cnt = 0;
  std::memcpy(&ss, rows_map_ + 8, 4);
  std::memcpy(&cnt, rows_map_ + 12, 4);
  if (ss != def_.slot_size) throwErr(ErrKind::Io, "row file slot size mismatch");
  slot_count_ = cnt;
  capacity_slots_ = (file_size_ - kHeaderSize) / def_.slot_size;
  if (slot_count_ > capacity_slots_) slot_count_ = capacity_slots_;

  struct stat ws{};
  if (::fstat(wal_fd_, &ws) != 0) throwErr(ErrKind::Io, "fstat wal failed");
  wal_size_ = static_cast<size_t>(ws.st_size);

  scanRows();
  recover();
}

void Table::close() {
  if (rows_map_) {
    ::msync(rows_map_, map_size_, MS_SYNC);
    ::munmap(rows_map_, map_size_);
    rows_map_ = nullptr;
  }
  if (rows_fd_ >= 0) {
    ::close(rows_fd_);
    rows_fd_ = -1;
  }
  if (wal_fd_ >= 0) {
    ::close(wal_fd_);
    wal_fd_ = -1;
  }
}

void Table::scanRows() {
  id_to_slot_.clear();
  id_to_slot_.reserve(slot_count_ * 2 + 16);
  for (uint32_t i = 0; i < slot_count_; ++i) {
    const uint8_t *p = slotPtr(i);
    if (p[0] == 0) continue;
    int64_t id;
    std::memcpy(&id, p + 8, sizeof(id));
    id_to_slot_[id] = i;
  }
}

void Table::ensureCapacity(uint32_t slots) {
  if (slots <= capacity_slots_) return;
  uint32_t new_cap = std::max<uint32_t>(slots + 256, capacity_slots_ * 2);
  size_t need = kHeaderSize + static_cast<size_t>(new_cap) * def_.slot_size;
  need = (need + 4095) / 4096 * 4096;
  if (need < map_size_) need = map_size_;
  if (::ftruncate(rows_fd_, static_cast<off_t>(need)) != 0) throwErr(ErrKind::Io, "ftruncate grow failed");
  ::msync(rows_map_, map_size_, MS_ASYNC);
  ::munmap(rows_map_, map_size_);
  rows_map_ = static_cast<uint8_t *>(
      ::mmap(nullptr, need, PROT_READ | PROT_WRITE, MAP_SHARED, rows_fd_, 0));
  if (rows_map_ == MAP_FAILED) {
    rows_map_ = nullptr;
    throwErr(ErrKind::Io, "mmap grow failed");
  }
  map_size_ = need;
  file_size_ = need;
  capacity_slots_ = new_cap;
}

void Table::applyOp(const Op &op) {
  const bool deleting = op.type == 3;
  if (deleting && op.slot >= slot_count_) return;
  if (!deleting) ensureCapacity(op.slot + 1);
  uint8_t *p = rows_map_ + kHeaderSize + static_cast<size_t>(op.slot) * def_.slot_size;
  if (op.slot < slot_count_ && p[0] != 0) {
    int64_t old_id;
    std::memcpy(&old_id, p + 8, sizeof(old_id));
    const auto it = id_to_slot_.find(old_id);
    // Another slot may already own this key during a multi-row change.
    if (it != id_to_slot_.end() && it->second == op.slot) id_to_slot_.erase(it);
  }
  if (deleting) {
    p[0] = 0;
    return;
  }
  std::memcpy(p, op.data.data(), def_.slot_size);
  p[0] = 1;
  if (op.slot >= slot_count_) {
    slot_count_ = op.slot + 1;
    std::memcpy(rows_map_ + 12, &slot_count_, 4);
  }
  int64_t id;
  std::memcpy(&id, p + 8, sizeof(id));
  id_to_slot_[id] = op.slot;
}

void Table::recover() {
  if (wal_size_ == 0) return;
  std::vector<uint8_t> payload;
  size_t pos = 0;
  size_t valid_end = 0;
  while (pos + 16 <= wal_size_) {
    uint32_t len = 0, crc = 0;
    if (::pread(wal_fd_, &len, 4, static_cast<off_t>(pos)) != 4) break;
    if (::pread(wal_fd_, &crc, 4, static_cast<off_t>(pos + 4)) != 4) break;
    if (len == 0 || len > (64u << 20)) break;
    if (pos + 8 + len + 8 > wal_size_) break;
    payload.resize(len);
    if (::pread(wal_fd_, payload.data(), len, static_cast<off_t>(pos + 8)) !=
        static_cast<ssize_t>(len)) {
      break;
    }
    uint64_t magic = 0;
    if (::pread(wal_fd_, &magic, 8, static_cast<off_t>(pos + 8 + len)) != 8) break;
    if (magic != kFrameMagic) break;
    if (crc32(payload.data(), payload.size()) != crc) break;
    // 解析并应用
    size_t off = 0;
    uint32_t nops = 0;
    if (payload.size() < 4) break;
    std::memcpy(&nops, payload.data(), 4);
    off = 4;
    bool ok = true;
    for (uint32_t i = 0; i < nops; ++i) {
      if (off + 5 > payload.size()) {
        ok = false;
        break;
      }
      Op op;
      op.type = payload[off];
      off += 1;
      std::memcpy(&op.slot, payload.data() + off, 4);
      off += 4;
      if (op.type != 3) {
        uint32_t dl = 0;
        if (off + 4 > payload.size()) {
          ok = false;
          break;
        }
        std::memcpy(&dl, payload.data() + off, 4);
        off += 4;
        if (dl != def_.slot_size || off + dl > payload.size()) {
          ok = false;
          break;
        }
        op.data.assign(payload.begin() + off, payload.begin() + off + dl);
        off += dl;
      }
      applyOp(op);
    }
    if (!ok) break;
    pos += 8 + len + 8;
    valid_end = pos;
  }
  if (valid_end < wal_size_) {
    if (::ftruncate(wal_fd_, static_cast<off_t>(valid_end)) != 0) throwErr(ErrKind::Io, "truncate wal failed");
    ::fsync(wal_fd_);
    wal_size_ = valid_end;
  }
  // 恢复完成后做一次检查点，避免 WAL 无限增长
  if (wal_size_ > (8u << 20)) resetWal();
}

void Table::resetWal() {
  ::msync(rows_map_, map_size_, MS_SYNC);
  if (::ftruncate(wal_fd_, 0) != 0) throwErr(ErrKind::Io, "truncate wal failed");
  ::fsync(wal_fd_);
  wal_size_ = 0;
}

void Table::commitOps(std::vector<Op> &ops) {
  if (ops.empty()) return;
  // 预留整帧空间，避免逐操作扩容和 payload 到 frame 的重复复制。
  size_t payload_size = 4;
  for (const Op &op : ops) {
    payload_size += 1 + 4 + ((op.type == 3) ? 0 : 4 + op.data.size());
  }
  std::vector<uint8_t> frame;
  frame.reserve(8 + payload_size + 8);
  frame.resize(12);  // len + crc + nops
  uint32_t nops = static_cast<uint32_t>(ops.size());
  std::memcpy(frame.data() + 8, &nops, 4);
  for (const Op &op : ops) {
    frame.push_back(op.type);
    frame.insert(frame.end(), reinterpret_cast<const uint8_t *>(&op.slot),
                 reinterpret_cast<const uint8_t *>(&op.slot) + 4);
    if (op.type != 3) {
      uint32_t dl = static_cast<uint32_t>(op.data.size());
      frame.insert(frame.end(), reinterpret_cast<const uint8_t *>(&dl),
                   reinterpret_cast<const uint8_t *>(&dl) + 4);
      frame.insert(frame.end(), op.data.begin(), op.data.end());
    }
  }
  uint32_t crc = crc32(frame.data() + 8, payload_size);
  uint32_t len = static_cast<uint32_t>(payload_size);
  std::memcpy(frame.data(), &len, 4);
  std::memcpy(frame.data() + 4, &crc, 4);
  frame.insert(frame.end(), reinterpret_cast<const uint8_t *>(&kFrameMagic),
               reinterpret_cast<const uint8_t *>(&kFrameMagic) + 8);

  writeAll(wal_fd_, frame.data(), frame.size(), static_cast<off_t>(wal_size_));
  // 提交点：必须先落盘再回复客户端
  if (::fdatasync(wal_fd_) != 0) throwErr(ErrKind::Io, "fdatasync wal failed");
  wal_size_ += frame.size();

  for (const Op &op : ops) applyOp(op);

  if (wal_size_ > (64u << 20)) resetWal();
}

void Table::buildSlot(const std::vector<Value> &values, std::vector<uint8_t> &out) const {
  out.assign(def_.slot_size, 0);
  out[0] = 1;
  int64_t id = values[static_cast<size_t>(def_.pk_index)].i;
  std::memcpy(out.data() + 8, &id, sizeof(id));
  for (size_t i = 0; i < def_.cols.size(); ++i) {
    const ColumnDef &c = def_.cols[i];
    uint8_t *p = out.data() + def_.col_offset[i];
    switch (c.type) {
      case ColType::BigInt: {
        std::memcpy(p, &values[i].i, 8);
        break;
      }
      case ColType::VarChar: {
        uint32_t len = static_cast<uint32_t>(values[i].s.size());
        std::memcpy(p, &len, 4);
        std::memcpy(p + 4, values[i].s.data(), len);
        break;
      }
      case ColType::Vector: {
        std::memcpy(p, values[i].v.data(), values[i].v.size() * sizeof(float));
        break;
      }
    }
  }
}

Storage::Storage(std::string data_dir) : data_dir_(std::move(data_dir)) {
  catalog_path_ = data_dir_ + "/catalog.meta";
}

Storage::~Storage() { close(); }

void Storage::open() {
  ::mkdir(data_dir_.c_str(), 0755);
  std::ifstream in(catalog_path_);
  if (!in.good()) {
    next_table_id_ = 1;
    return;
  }
  std::string line;
  std::getline(in, line);
  if (line != "v1") throwErr(ErrKind::Io, "bad catalog version");
  TableDef cur;
  bool in_table = false;
  while (std::getline(in, line)) {
    std::istringstream ss(line);
    std::string tag;
    ss >> tag;
    if (tag == "next_table_id") {
      ss >> next_table_id_;
    } else if (tag == "table") {
      cur = TableDef();
      std::string name;
      size_t ncols = 0;
      ss >> cur.id >> name >> ncols;
      cur.name = name;
      cur.cols.clear();
      cur.indexes.clear();
      in_table = true;
    } else if (tag == "col" && in_table) {
      ColumnDef c;
      std::string ty;
      int pk = 0;
      ss >> c.name >> ty >> c.param >> pk;
      if (ty == "BIGINT") c.type = ColType::BigInt;
      else if (ty == "VARCHAR") c.type = ColType::VarChar;
      else c.type = ColType::Vector;
      c.pk = pk != 0;
      cur.cols.push_back(std::move(c));
    } else if (tag == "index" && in_table) {
      IndexDef idx;
      ss >> idx.name >> idx.column;
      cur.indexes.push_back(std::move(idx));
    } else if (tag == "tableend") {
      in_table = false;
      for (size_t i = 0; i < cur.cols.size(); ++i) {
        if (cur.cols[i].pk) cur.pk_index = static_cast<int>(i);
      }
      auto t = std::make_unique<Table>(cur, data_dir_);
      t->open();
      tables_.push_back(std::move(t));
    }
  }
  if (next_table_id_ < 1) next_table_id_ = 1;
}

void Storage::close() {
  for (auto &t : tables_) t->close();
  tables_.clear();
}

Table *Storage::find(const std::string &name) {
  for (auto &t : tables_) {
    if (lowerName(t->def().name) == lowerName(name)) return t.get();
  }
  return nullptr;
}

Table *Storage::findById(int id) {
  for (auto &t : tables_) {
    if (t->def().id == id) return t.get();
  }
  return nullptr;
}

Table *Storage::require(const std::string &name) {
  Table *t = find(name);
  if (!t) throwErr(ErrKind::TableNotFound, "table not found: " + name);
  return t;
}

void Storage::createTable(const TableDef &def, bool if_not_exists) {
  if (find(def.name)) {
    if (if_not_exists) return;
    throwErr(ErrKind::TableExists, "table already exists: " + def.name);
  }
  TableDef copy = def;
  copy.id = next_table_id_++;
  auto t = std::make_unique<Table>(copy, data_dir_);
  t->open();
  tables_.push_back(std::move(t));
  saveCatalog();
}

void Storage::dropTable(const std::string &name, bool if_exists) {
  for (auto it = tables_.begin(); it != tables_.end(); ++it) {
    if (lowerName((*it)->def().name) == lowerName(name)) {
      int id = (*it)->def().id;
      (*it)->close();
      tables_.erase(it);
      ::unlink(tableFileName(data_dir_, id, ".rows").c_str());
      ::unlink(tableFileName(data_dir_, id, ".wal").c_str());
      saveCatalog();
      return;
    }
  }
  if (!if_exists) throwErr(ErrKind::TableNotFound, "table not found: " + name);
}

std::vector<std::string> Storage::tableNames() const {
  std::vector<std::string> names;
  for (auto &t : tables_) names.push_back(t->def().name);
  std::sort(names.begin(), names.end());
  return names;
}

void Storage::saveCatalog() {
  std::string tmp = catalog_path_ + ".tmp";
  std::string content = "v1\n";
  content += "next_table_id " + std::to_string(next_table_id_) + "\n";
  for (auto &t : tables_) {
    const TableDef &d = t->def();
    content += "table " + std::to_string(d.id) + " " + d.name + " " +
               std::to_string(d.cols.size()) + "\n";
    for (const ColumnDef &c : d.cols) {
      const char *ty = c.type == ColType::BigInt ? "BIGINT"
                       : c.type == ColType::VarChar ? "VARCHAR"
                                                    : "VECTOR";
      content += "col " + c.name + " " + ty + " " + std::to_string(c.param) + " " +
                 (c.pk ? "1" : "0") + "\n";
    }
    for (const IndexDef &idx : d.indexes) {
      content += "index " + idx.name + " " + idx.column + "\n";
    }
    content += "tableend\n";
  }
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) throwErr(ErrKind::Io, "cannot write catalog");
  try {
    writeAll(fd, content.data(), content.size(), 0);
  } catch (...) {
    ::close(fd);
    throw;
  }
  ::fsync(fd);
  ::close(fd);
  if (::rename(tmp.c_str(), catalog_path_.c_str()) != 0) throwErr(ErrKind::Io, "rename catalog failed");
  int dfd = ::open(data_dir_.c_str(), O_RDONLY | O_DIRECTORY);
  if (dfd >= 0) {
    ::fsync(dfd);
    ::close(dfd);
  }
}

}  // namespace mvd
