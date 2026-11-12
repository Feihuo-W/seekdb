#include "engine.h"

#include <cerrno>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <limits>
#include <queue>
#include <unordered_set>
#include <utility>

#if defined(__x86_64__)
#include <immintrin.h>
#endif

#include "fastfloat.h"
#include "json.h"

namespace vdb {
namespace {

constexpr uint32_t kMagic = 0x4D564442;  // "MVDB"
enum RecType : uint8_t {
  REC_SCHEMA = 1,
  REC_BEGIN = 2,
  REC_PUT = 3,
  REC_DEL = 4,
  REC_COMMIT = 5,
};
constexpr size_t kRecHeader = 17;  // magic(4) len(4) crc(4) type(1) txn(4)
constexpr size_t kWalGrowBytes = 16u << 20;
// Large tables must not trigger frequent full-snapshot rewrites: the WAL cap is
// generous so bulk loads compact a handful of times, and recovery replays at
// most this much log.
constexpr size_t kCompactWalBytes = 256u << 20;

uint32_t Crc32(const void* data, size_t n) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    init = true;
  }
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

inline void PutU8(std::string& b, uint8_t v) { b.push_back(static_cast<char>(v)); }
inline void PutU32(std::string& b, uint32_t v) {
  char buf[4];
  std::memcpy(buf, &v, 4);
  b.append(buf, 4);
}
inline void PutI64(std::string& b, int64_t v) {
  char buf[8];
  std::memcpy(buf, &v, 8);
  b.append(buf, 8);
}
inline void PutF32(std::string& b, float v) {
  char buf[4];
  std::memcpy(buf, &v, 4);
  b.append(buf, 4);
}
inline uint32_t GetU32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}
inline int64_t GetI64(const uint8_t* p) {
  int64_t v;
  std::memcpy(&v, p, 8);
  return v;
}
inline float GetF32(const uint8_t* p) {
  float v;
  std::memcpy(&v, p, 4);
  return v;
}

uint64_t Utf8CodePoints(const std::string& s) {
  uint64_t n = 0;
  for (unsigned char c : s) {
    if ((c & 0xC0) != 0x80) ++n;
  }
  return n;
}

// 解析向量文本（形如 "[1,2,3]"），直接产出 float32。
// 与旧实现等价（旧版先产出 double 数组再逐个转 float32），但省掉了中间
// double 向量的分配与拷贝——实测这是写入期与查询期都在付的固定开销。
bool ParseVecText(const std::string& text, std::vector<float>* out) {
  size_t i = 0;
  auto skip = [&]() {
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
  };
  skip();
  if (i >= text.size() || text[i] != '[') return false;
  ++i;
  while (true) {
    skip();
    if (i < text.size() && text[i] == ']') {
      ++i;
      break;
    }
    const char* start = text.c_str() + i;
    const char* next = nullptr;
    double v = 0;
    if (!FastParseDouble(start, text.c_str() + text.size(), &v, &next)) {
      char* endp = nullptr;
      v = std::strtod(start, &endp);
      if (endp == start) return false;
      next = endp;
    }
    i = static_cast<size_t>(next - text.c_str());
    out->push_back(static_cast<float>(v));
    skip();
    if (i < text.size() && text[i] == ',') {
      ++i;
      continue;
    }
    if (i < text.size() && text[i] == ']') {
      ++i;
      break;
    }
    return false;
  }
  skip();
  return i == text.size();
}

bool WriteAll(int fd, const void* data, size_t n) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  while (n > 0) {
    ssize_t w = ::write(fd, p, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    p += w;
    n -= static_cast<size_t>(w);
  }
  return true;
}

bool FsyncDir(const std::string& dir) {
  int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) return false;
  bool ok = ::fsync(fd) == 0;
  ::close(fd);
  return ok;
}

}  // namespace

int Schema::FindCol(const std::string& lname) const {
  for (size_t i = 0; i < cols.size(); ++i) {
    if (cols[i].lname == lname) return static_cast<int>(i);
  }
  return -1;
}

uint32_t Schema::FixedWidth(int col) const {
  const Column& c = cols[col];
  return c.type.kind == ColumnType::BIGINT ? 8u : c.type.param * 4u;
}

void Schema::ComputeLayout() {
  fixed_off.assign(cols.size(), UINT32_MAX);
  varlen_off.assign(cols.size(), 0);
  uint32_t off = 0;
  nvarchar = 0;
  for (size_t i = 0; i < cols.size(); ++i) {
    if (cols[i].type.kind == ColumnType::VARCHAR) {
      ++nvarchar;
    } else {
      fixed_off[i] = off;
      off += FixedWidth(static_cast<int>(i));
    }
  }
  fixed_size = off;
  uint32_t lo = off;
  for (size_t i = 0; i < cols.size(); ++i) {
    if (cols[i].type.kind == ColumnType::VARCHAR) {
      varlen_off[i] = lo;
      lo += 4;
    }
  }
}

bool Engine::RowView::Valid() const { return p != nullptr && n > 0; }

Engine::Engine(std::string dir) : dir_(std::move(dir)) {
  if (!Init()) {
    ok_ = false;
    if (init_error_.empty()) init_error_ = "io_error";
  }
}

Engine::~Engine() { CloseAll(); }

bool Engine::EnsureDir() {
  if (::mkdir(dir_.c_str(), 0755) == 0) return true;
  return errno == EEXIST;
}

bool Engine::Init() {
  if (!EnsureDir()) {
    init_error_ = "io_error";
    return false;
  }
  // Leftover temp files from an interrupted compaction are never authoritative:
  // snapshot.dat + wal.log always form a recoverable pair on their own.
  ::unlink((dir_ + "/snapshot.tmp").c_str());
  ::unlink((dir_ + "/wal.tmp").c_str());
  if (!OpenSnapshot()) return false;
  if (!OpenWal()) return false;
  if (!LoadSnapshot()) return false;
  ReplayWal();
  return true;
}

bool Engine::OpenSnapshot() {
  if (snap_base_ != nullptr) {
    ::munmap(snap_base_, snap_size_);
    snap_base_ = nullptr;
    snap_size_ = 0;
  }
  if (snap_fd_ >= 0) {
    ::close(snap_fd_);
    snap_fd_ = -1;
  }
  std::string path = dir_ + "/snapshot.dat";
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return errno == ENOENT;
  struct stat st;
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    return false;
  }
  if (st.st_size > 0) {
    void* p = ::mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) {
      ::close(fd);
      return false;
    }
    snap_base_ = static_cast<uint8_t*>(p);
    snap_size_ = static_cast<size_t>(st.st_size);
  }
  snap_fd_ = fd;
  return true;
}

bool Engine::OpenWal() {
  std::string path = dir_ + "/wal.log";
  int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd < 0) {
    init_error_ = "io_error";
    return false;
  }
  struct stat st;
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    init_error_ = "io_error";
    return false;
  }
  size_t size = static_cast<size_t>(st.st_size);
  size_t cap = std::max<size_t>(size + kWalGrowBytes, kWalGrowBytes);
  if (::ftruncate(fd, static_cast<off_t>(cap)) != 0) {
    ::close(fd);
    init_error_ = "io_error";
    return false;
  }
  void* p = ::mmap(nullptr, cap, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) {
    ::close(fd);
    init_error_ = "io_error";
    return false;
  }
  wal_fd_ = fd;
  wal_base_ = static_cast<uint8_t*>(p);
  wal_size_ = size;
  wal_cap_ = cap;
  return true;
}

void Engine::CloseAll() {
  if (snap_base_ != nullptr) {
    ::munmap(snap_base_, snap_size_);
    snap_base_ = nullptr;
  }
  if (snap_fd_ >= 0) {
    ::close(snap_fd_);
    snap_fd_ = -1;
  }
  if (wal_base_ != nullptr) {
    ::munmap(wal_base_, wal_cap_);
    wal_base_ = nullptr;
  }
  if (wal_fd_ >= 0) {
    ::close(wal_fd_);
    wal_fd_ = -1;
  }
  wal_size_ = 0;
  wal_cap_ = 0;
}

bool Engine::AppendRecord(uint8_t type, uint32_t txn, const std::string& payload,
                          uint64_t* off) {
  size_t need = kRecHeader + payload.size();
  if (wal_size_ + need > wal_cap_) {
    size_t new_cap = std::max(wal_size_ + need + kWalGrowBytes, wal_cap_ * 2);
    if (::ftruncate(wal_fd_, static_cast<off_t>(new_cap)) != 0) return false;
    void* p = ::mremap(wal_base_, wal_cap_, new_cap, MREMAP_MAYMOVE);
    if (p == MAP_FAILED) return false;
    wal_base_ = static_cast<uint8_t*>(p);
    wal_cap_ = new_cap;
  }
  uint8_t* dst = wal_base_ + wal_size_;
  uint32_t len = static_cast<uint32_t>(payload.size());
  uint32_t crc = Crc32(payload.data(), payload.size());
  std::memcpy(dst, &kMagic, 4);
  std::memcpy(dst + 4, &len, 4);
  std::memcpy(dst + 8, &crc, 4);
  dst[12] = type;
  std::memcpy(dst + 13, &txn, 4);
  if (!payload.empty()) std::memcpy(dst + kRecHeader, payload.data(), payload.size());
  if (off != nullptr) *off = wal_size_;
  wal_size_ += need;
  return true;
}

bool Engine::SyncWal() { return ::fdatasync(wal_fd_) == 0; }

bool Engine::LoadSnapshot() {
  size_t pos = 0;
  while (pos + kRecHeader <= snap_size_) {
    const uint8_t* p = snap_base_ + pos;
    if (GetU32(p) != kMagic) break;
    uint32_t len = GetU32(p + 4);
    uint32_t crc = GetU32(p + 8);
    uint8_t type = p[12];
    if (pos + kRecHeader + len > snap_size_) break;
    const uint8_t* payload = p + kRecHeader;
    if (Crc32(payload, len) != crc) break;
    if (type == REC_SCHEMA) {
      Schema s;
      size_t used = 0;
      if (DeserializeSchema(payload, len, &s, &used)) RegisterSchema(s);
    } else if (type == REC_PUT) {
      if (len >= 12) {
        uint32_t tid = GetU32(payload);
        int64_t pk = GetI64(payload + 4);
        auto it = tables_.find(tid);
        if (it != tables_.end()) it->second.rows[pk] = RowRef{pos, 0};
      }
    }
    pos += kRecHeader + len;
  }
  return true;
}

void Engine::ReplayWal() {
  size_t pos = 0;
  size_t valid_end = 0;
  struct PendingOp {
    uint8_t type;
    uint32_t tid;
    int64_t pk;
    uint64_t off;
    Schema schema;
    bool has_schema;
  };
  std::map<uint32_t, std::vector<PendingOp>> pending;
  while (pos + kRecHeader <= wal_size_) {
    const uint8_t* p = wal_base_ + pos;
    if (GetU32(p) != kMagic) break;
    uint32_t len = GetU32(p + 4);
    uint32_t crc = GetU32(p + 8);
    uint8_t type = p[12];
    uint32_t txn = GetU32(p + 13);
    if (pos + kRecHeader + len > wal_size_) break;
    const uint8_t* payload = p + kRecHeader;
    if (Crc32(payload, len) != crc) break;
    valid_end = pos + kRecHeader + len;
    if (type == REC_SCHEMA) {
      PendingOp op{};
      op.type = type;
      op.has_schema = DeserializeSchema(payload, len, &op.schema, nullptr);
      if (op.has_schema) pending[txn].push_back(std::move(op));
    } else if (type == REC_PUT) {
      if (len >= 12) {
        PendingOp op{};
        op.type = type;
        op.tid = GetU32(payload);
        op.pk = GetI64(payload + 4);
        op.off = pos;
        pending[txn].push_back(std::move(op));
      }
    } else if (type == REC_DEL) {
      if (len >= 12) {
        PendingOp op{};
        op.type = type;
        op.tid = GetU32(payload);
        op.pk = GetI64(payload + 4);
        op.off = pos;
        pending[txn].push_back(std::move(op));
      }
    } else if (type == REC_COMMIT) {
      auto it = pending.find(txn);
      if (it != pending.end()) {
        for (auto& op : it->second) {
          if (op.type == REC_SCHEMA) {
            if (op.has_schema) RegisterSchema(op.schema);
          } else if (op.type == REC_PUT) {
            auto t = tables_.find(op.tid);
            if (t != tables_.end()) t->second.rows[op.pk] = RowRef{op.off, 1};
          } else if (op.type == REC_DEL) {
            auto t = tables_.find(op.tid);
            if (t != tables_.end()) t->second.rows.erase(op.pk);
          }
        }
        pending.erase(it);
      }
      if (txn >= next_txn_) next_txn_ = txn + 1;
    }
    pos += kRecHeader + len;
  }
  // The wal file is preallocated to its capacity; the logical size is the end
  // of the last valid record. Stale bytes beyond it are simply overwritten.
  wal_size_ = valid_end;
}

std::string Engine::SerializeSchema(const Schema& s) const {
  std::string b;
  b.reserve(64);
  PutU32(b, s.id);
  PutU32(b, static_cast<uint32_t>(s.name.size()));
  b += s.name;
  PutU32(b, static_cast<uint32_t>(s.cols.size()));
  for (const auto& c : s.cols) {
    PutU32(b, static_cast<uint32_t>(c.name.size()));
    b += c.name;
    PutU8(b, static_cast<uint8_t>(c.type.kind));
    PutU32(b, c.type.param);
  }
  PutU32(b, static_cast<uint32_t>(s.pk));
  PutU32(b, static_cast<uint32_t>(s.vec));
  PutU8(b, s.has_vec_index ? 1 : 0);
  return b;
}

bool Engine::DeserializeSchema(const uint8_t* p, size_t n, Schema* out, size_t* used) const {
  size_t i = 0;
  auto need = [&](size_t k) { return i + k <= n; };
  if (!need(4)) return false;
  out->id = GetU32(p + i);
  i += 4;
  if (!need(4)) return false;
  uint32_t nlen = GetU32(p + i);
  i += 4;
  if (!need(nlen)) return false;
  out->name.assign(reinterpret_cast<const char*>(p + i), nlen);
  i += nlen;
  if (!need(4)) return false;
  uint32_t ncols = GetU32(p + i);
  i += 4;
  if (ncols > 4096) return false;
  out->cols.clear();
  out->cols.reserve(ncols);
  for (uint32_t c = 0; c < ncols; ++c) {
    if (!need(4)) return false;
    uint32_t clen = GetU32(p + i);
    i += 4;
    if (!need(clen + 5)) return false;
    Column col;
    col.name.assign(reinterpret_cast<const char*>(p + i), clen);
    col.lname = col.name;
    for (char& ch : col.lname) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    i += clen;
    uint8_t kind = p[i++];
    col.type.kind = static_cast<ColumnType::Kind>(kind);
    col.type.param = GetU32(p + i);
    i += 4;
    out->cols.push_back(std::move(col));
  }
  if (!need(9)) return false;
  uint32_t pk = GetU32(p + i);
  i += 4;
  uint32_t vec = GetU32(p + i);
  i += 4;
  out->pk = static_cast<int>(static_cast<int32_t>(pk));
  out->vec = static_cast<int>(static_cast<int32_t>(vec));
  out->has_vec_index = p[i++] != 0;
  if (used != nullptr) *used = i;
  return true;
}

void Engine::RegisterSchema(const Schema& s) {
  auto it = table_ids_.find(s.name);
  if (it == table_ids_.end()) {
    table_ids_[s.name] = s.id;
    TableState ts;
    ts.schema = s;
    tables_[s.id] = std::move(ts);
  } else {
    uint32_t id = it->second;
    auto t = tables_.find(id);
    if (t == tables_.end()) {
      TableState ts;
      ts.schema = s;
      tables_[id] = std::move(ts);
    } else {
      t->second.schema = s;
      t->second.schema.id = id;
    }
  }
  auto idit = table_ids_.find(s.name);
  if (idit != table_ids_.end()) {
    auto t = tables_.find(idit->second);
    if (t != tables_.end()) t->second.schema.ComputeLayout();
  }
  if (s.id >= next_table_id_) next_table_id_ = s.id + 1;
}

Engine::TableState* Engine::FindTable(const std::string& lname) {
  auto it = table_ids_.find(lname);
  if (it == table_ids_.end()) return nullptr;
  auto t = tables_.find(it->second);
  if (t == tables_.end()) return nullptr;
  return &t->second;
}

std::string Engine::EncodeRow(const Schema& s, const std::vector<ColumnValue>& vals) const {
  std::string b;
  b.reserve(s.fixed_size + static_cast<size_t>(s.nvarchar) * 4 + 32);
  for (size_t c = 0; c < s.cols.size(); ++c) {
    const Column& col = s.cols[c];
    if (col.type.kind == ColumnType::BIGINT) {
      PutI64(b, vals[c].i);
    } else if (col.type.kind == ColumnType::VECTOR) {
      for (uint32_t d = 0; d < col.type.param; ++d) PutF32(b, vals[c].v[d]);
    }
  }
  for (size_t c = 0; c < s.cols.size(); ++c) {
    if (s.cols[c].type.kind == ColumnType::VARCHAR)
      PutU32(b, static_cast<uint32_t>(vals[c].s.size()));
  }
  for (size_t c = 0; c < s.cols.size(); ++c) {
    if (s.cols[c].type.kind == ColumnType::VARCHAR) b += vals[c].s;
  }
  return b;
}

std::vector<Engine::ColumnValue> Engine::DecodeRow(const Schema& s, const RowView& v) const {
  std::vector<ColumnValue> vals(s.cols.size());
  for (size_t c = 0; c < s.cols.size(); ++c) {
    const Column& col = s.cols[c];
    if (col.type.kind == ColumnType::BIGINT) {
      vals[c].i = GetI64(v.p + s.fixed_off[c]);
    } else if (col.type.kind == ColumnType::VECTOR) {
      vals[c].v.resize(col.type.param);
      const uint8_t* p = v.p + s.fixed_off[c];
      for (uint32_t d = 0; d < col.type.param; ++d) vals[c].v[d] = GetF32(p + d * 4);
    }
  }
  uint32_t data = s.fixed_size + static_cast<uint32_t>(s.nvarchar) * 4;
  for (size_t c = 0; c < s.cols.size(); ++c) {
    if (s.cols[c].type.kind != ColumnType::VARCHAR) continue;
    uint32_t len = GetU32(v.p + s.varlen_off[c]);
    vals[c].s.assign(reinterpret_cast<const char*>(v.p + data), len);
    data += len;
  }
  return vals;
}

int64_t Engine::PkOf(const Schema& s, const RowView& v) const {
  return GetI64(v.p + s.fixed_off[s.pk]);
}

const uint8_t* Engine::RowPayload(const RowRef& ref, uint32_t* len) const {
  const uint8_t* base = ref.file == 0 ? snap_base_ : wal_base_;
  if (base == nullptr) return nullptr;
  const uint8_t* p = base + ref.off + kRecHeader;
  uint32_t total = GetU32(base + ref.off + 4);
  if (total < 12) return nullptr;
  *len = total - 12;
  return p + 12;
}

Engine::RowView Engine::MakeView(const RowRef& ref) const {
  RowView v;
  uint32_t len = 0;
  const uint8_t* p = RowPayload(ref, &len);
  v.p = p;
  v.n = len;
  return v;
}

bool Engine::BindLiteral(const Column& col, const Literal& lit, ColumnValue* out,
                         std::string& err) const {
  switch (col.type.kind) {
    case ColumnType::BIGINT:
      if (lit.kind != LitKind::Int) {
        err = "type_mismatch";
        return false;
      }
      out->i = lit.i;
      return true;
    case ColumnType::VARCHAR:
      if (lit.kind != LitKind::Str) {
        err = "type_mismatch";
        return false;
      }
      out->s = lit.s;
      return true;
    case ColumnType::VECTOR: {
      if (lit.kind == LitKind::Vec) {
        // 词法器解析出的数值序列（double）
        if (lit.nums.size() != col.type.param) {
          err = "dimension_mismatch";
          return false;
        }
        out->v.resize(lit.nums.size());
        for (size_t i = 0; i < lit.nums.size(); ++i) {
          double d = lit.nums[i];
          if (!std::isfinite(d)) {
            err = "invalid_vector";
            return false;
          }
          float f = static_cast<float>(d);
          if (!std::isfinite(f)) {
            err = "invalid_vector";
            return false;
          }
          out->v[i] = f;
        }
        return true;
      }
      if (lit.kind == LitKind::Str) {
        // 文本形式 '[...]'（官方负载的写法）：直接解析成 float32，
        // 不再经过"double 向量 -> float 向量"的中间拷贝。
        out->v.reserve(col.type.param);  // 避免解析过程中反复扩容
        if (!ParseVecText(lit.s, &out->v)) {
          err = "invalid_vector";
          return false;
        }
        if (out->v.size() != col.type.param) {
          err = "dimension_mismatch";
          return false;
        }
        for (float f : out->v) {
          if (!std::isfinite(f)) {
            err = "invalid_vector";
            return false;
          }
        }
        return true;
      }
      err = "type_mismatch";
      return false;
    }
  }
  err = "unsupported_sql";
  return false;
}

bool Engine::BindQueryVector(const Column& col, const Literal& lit, std::vector<float>* out,
                             std::string& err) const {
  ColumnValue v;
  if (!BindLiteral(col, lit, &v, err)) return false;
  *out = std::move(v.v);
  return true;
}

void Engine::EnsureScan(const TableState& t) const {
  if (!t.scan_dirty) return;
  t.scan.clear();
  t.scan.reserve(t.rows.size());
  for (const auto& kv : t.rows) t.scan.emplace_back(kv.first, kv.second);
  t.cache_col = -1;
  t.cache_ids.clear();
  t.cache_dict.clear();
  t.cache_int.clear();
  t.cache_lists.clear();
  t.cache_int_lists.clear();
  t.scan_dirty = false;
}

template <typename F>
void Engine::ForEachRow(const TableState& t, F&& fn) const {
  auto oit = overlay_.find(t.schema.id);
  bool has_overlay = oit != overlay_.end() && !oit->second.empty();
  if (!has_overlay) {
    EnsureScan(t);
    for (const auto& kv : t.scan) {
      RowView v = MakeView(kv.second);
      v.s = &t.schema;
      if (v.Valid()) fn(kv.first, v);
    }
    return;
  }
  const std::map<int64_t, OverlayRow>& ov = oit->second;
  auto ib = t.rows.begin();
  auto ie = t.rows.end();
  auto ob = ov.begin();
  auto oe = ov.end();
  while (ib != ie || ob != oe) {
    if (ob == oe || (ib != ie && ib->first < ob->first)) {
      RowView v = MakeView(ib->second);
      v.s = &t.schema;
      if (v.Valid()) fn(ib->first, v);
      ++ib;
    } else if (ib == ie || ob->first < ib->first) {
      if (!ob->second.del) {
        RowView v;
        v.s = &t.schema;
        v.p = reinterpret_cast<const uint8_t*>(ob->second.bytes.data());
        v.n = ob->second.bytes.size();
        if (v.Valid()) fn(ob->first, v);
      }
      ++ob;
    } else {
      if (!ob->second.del) {
        RowView v;
        v.s = &t.schema;
        v.p = reinterpret_cast<const uint8_t*>(ob->second.bytes.data());
        v.n = ob->second.bytes.size();
        if (v.Valid()) fn(ib->first, v);
      }
      ++ib;
      ++ob;
    }
  }
}

bool Engine::BuildPredicates(const Schema& s, const std::vector<Condition>& conds,
                             std::vector<CondBound>* out, std::string& err) const {
  for (const auto& c : conds) {
    std::string lname = c.col;
    for (char& ch : lname) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    int idx = s.FindCol(lname);
    if (idx < 0) {
      err = "unknown_column";
      return false;
    }
    const Column& col = s.cols[idx];
    if (col.type.kind == ColumnType::VECTOR) {
      err = "unsupported_sql";
      return false;
    }
    CondBound b;
    b.col = idx;
    b.op = c.op;
    ColumnValue v;
    if (!BindLiteral(col, c.lit, &v, err)) return false;
    if (col.type.kind == ColumnType::BIGINT) {
      b.is_str = false;
      b.ival = v.i;
    } else {
      b.is_str = true;
      b.sval = std::move(v.s);
    }
    out->push_back(std::move(b));
  }
  return true;
}

namespace {

bool ColAt(const Engine::RowView& v, const Schema& s, int col, const uint8_t** p,
           uint32_t* len) {
  if (s.fixed_off[col] != UINT32_MAX) {
    uint32_t off = s.fixed_off[col];
    uint32_t w = s.FixedWidth(col);
    if (static_cast<size_t>(off) + w > v.n) return false;
    *p = v.p + off;
    *len = w;
    return true;
  }
  uint32_t l = GetU32(v.p + s.varlen_off[col]);
  uint32_t off = s.fixed_size + static_cast<uint32_t>(s.nvarchar) * 4;
  for (int c = 0; c < col; ++c) {
    if (s.cols[c].type.kind == ColumnType::VARCHAR) off += GetU32(v.p + s.varlen_off[c]);
  }
  if (static_cast<size_t>(off) + l > v.n) return false;
  *p = v.p + off;
  *len = l;
  return true;
}

}  // namespace

bool Engine::EvalPredicates(const std::vector<CondBound>& conds, const RowView& row) const {
  for (const auto& c : conds) {
    const uint8_t* p = nullptr;
    uint32_t len = 0;
    if (!ColAt(row, *row.s, c.col, &p, &len)) return false;
    if (!c.is_str) {
      int64_t v = GetI64(p);
      switch (c.op) {
        case CmpOp::EQ: if (!(v == c.ival)) return false; break;
        case CmpOp::LT: if (!(v < c.ival)) return false; break;
        case CmpOp::LE: if (!(v <= c.ival)) return false; break;
        case CmpOp::GT: if (!(v > c.ival)) return false; break;
        case CmpOp::GE: if (!(v >= c.ival)) return false; break;
      }
    } else {
      std::string_view v(reinterpret_cast<const char*>(p), len);
      int cmp = v.compare(c.sval);
      switch (c.op) {
        case CmpOp::EQ: if (cmp != 0) return false; break;
        case CmpOp::LT: if (!(cmp < 0)) return false; break;
        case CmpOp::LE: if (!(cmp <= 0)) return false; break;
        case CmpOp::GT: if (!(cmp > 0)) return false; break;
        case CmpOp::GE: if (!(cmp >= 0)) return false; break;
      }
    }
  }
  return true;
}

namespace {

#if defined(__x86_64__)
__attribute__((target("avx2,fma"))) float L2SqAvx2(const float* a, const float* b, int dim) {
  __m256 a0 = _mm256_setzero_ps();
  __m256 a1 = a0, a2 = a0, a3 = a0;
  int i = 0;
  for (; i + 32 <= dim; i += 32) {
    __m256 d0 = _mm256_sub_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i));
    __m256 d1 = _mm256_sub_ps(_mm256_loadu_ps(a + i + 8), _mm256_loadu_ps(b + i + 8));
    __m256 d2 = _mm256_sub_ps(_mm256_loadu_ps(a + i + 16), _mm256_loadu_ps(b + i + 16));
    __m256 d3 = _mm256_sub_ps(_mm256_loadu_ps(a + i + 24), _mm256_loadu_ps(b + i + 24));
    a0 = _mm256_fmadd_ps(d0, d0, a0);
    a1 = _mm256_fmadd_ps(d1, d1, a1);
    a2 = _mm256_fmadd_ps(d2, d2, a2);
    a3 = _mm256_fmadd_ps(d3, d3, a3);
  }
  for (; i + 8 <= dim; i += 8) {
    __m256 d = _mm256_sub_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i));
    a0 = _mm256_fmadd_ps(d, d, a0);
  }
  __m256 s = _mm256_add_ps(_mm256_add_ps(a0, a1), _mm256_add_ps(a2, a3));
  // 横向求和：原先用 store 到栈数组再逐个标量相加，会引入 store-forwarding 停顿
  // （实测每行多花十几个周期）。改用 hadd 指令链，全程留在寄存器里。
  __m256 h = _mm256_hadd_ps(s, s);          // [s0+s1, s2+s3, s4+s5, s6+s7, ...]
  __m256 l = _mm256_hadd_ps(h, h);          // [s0..s3, s4..s7, ...]
  __m128 lo = _mm256_castps256_ps128(l);    // l[0] = s0+s1+s2+s3
  __m128 hi = _mm256_extractf128_ps(l, 1);  // hi[1] = l[5] = s4+s5+s6+s7
  __m128 hi1 = _mm_shuffle_ps(hi, hi, _MM_SHUFFLE(1, 1, 1, 1));
  float sum = _mm_cvtss_f32(_mm_add_ss(lo, hi1));
  for (; i < dim; ++i) {
    float d = a[i] - b[i];
    sum += d * d;
  }
  return sum;
}
#endif

// 标量回退：保持原来的 double 累加语义（非 AVX2 机器上结果不变）。
double L2SqScalar(const float* a, const float* b, int dim) {
  double sum = 0.0;
  for (int i = 0; i < dim; ++i) {
    double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    sum += d * d;
  }
  return sum;
}

#if defined(__x86_64__)
__attribute__((target("avx2,fma"))) double L2SqAvx2D(const float* a, const float* b, int dim) {
  return static_cast<double>(L2SqAvx2(a, b, dim));
}
#endif

// 距离计算分派：原先每次调用都要经过函数内 static 守卫检查（线程安全初始化
// 守卫），热路径每行多一次原子读+分支。这里在静态初始化期解析一次，
// 热路径只保留一次间接调用。
using L2Fn = double (*)(const float*, const float*, int);

L2Fn ResolveL2Fn() {
#if defined(__x86_64__)
  if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")) return &L2SqAvx2D;
#endif
  return &L2SqScalar;
}

const L2Fn g_l2_fn = ResolveL2Fn();

}  // namespace

double Engine::L2Squared(const float* a, const float* b, int dim) const {
  return g_l2_fn(a, b, dim);
}

bool Engine::CommitOverlay() {
  if (overlay_.empty()) return true;
  uint32_t txn = next_txn_++;
  std::string empty;
  if (!AppendRecord(REC_BEGIN, txn, empty, nullptr)) {
    DropOverlay();
    return false;
  }
  struct Applied {
    uint32_t tid;
    int64_t pk;
    uint64_t off;
    bool del;
  };
  std::vector<Applied> applied;
  for (const auto& [tid, rows] : overlay_) {
    for (const auto& [pk, row] : rows) {
      std::string payload;
      PutU32(payload, tid);
      PutI64(payload, pk);
      uint64_t off = 0;
      if (row.del) {
        if (!AppendRecord(REC_DEL, txn, payload, &off)) {
          DropOverlay();
          return false;
        }
      } else {
        payload += row.bytes;
        if (!AppendRecord(REC_PUT, txn, payload, &off)) {
          DropOverlay();
          return false;
        }
      }
      applied.push_back(Applied{tid, pk, off, row.del});
    }
  }
  if (!AppendRecord(REC_COMMIT, txn, empty, nullptr)) {
    DropOverlay();
    return false;
  }
  if (!SyncWal()) {
    DropOverlay();
    return false;
  }
  for (const auto& a : applied) {
    auto t = tables_.find(a.tid);
    if (t == tables_.end()) continue;
    if (a.del) {
      t->second.rows.erase(a.pk);
    } else {
      t->second.rows[a.pk] = RowRef{a.off, 1};
    }
    t->second.scan_dirty = true;
  }
  overlay_.clear();
  return true;
}

void Engine::DropOverlay() { overlay_.clear(); }

std::string Engine::Execute(const std::string& sql, const OutSink& sink) {
  if (!ok_) return JsonError(init_error_.empty() ? "io_error" : init_error_);
  MaybeCompactStep();
  Statement st = Parse(sql);
  if (!st.error.empty()) return JsonError(st.error);
  switch (st.kind) {
    case StmtKind::Empty: return JsonOk();
    case StmtKind::CreateTable: return ExecCreateTable(st);
    case StmtKind::CreateVectorIndex: return ExecCreateIndex(st);
    case StmtKind::Insert: return ExecInsert(st);
    case StmtKind::Select: return ExecSelect(st, sink);
    case StmtKind::Update: return ExecUpdate(st);
    case StmtKind::Delete: return ExecDelete(st);
    case StmtKind::Begin: return ExecBegin();
    case StmtKind::Commit: return ExecCommit();
    case StmtKind::Rollback: return ExecRollback();
  }
  return JsonError("unsupported_sql");
}

namespace {
std::string LowerName(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
}  // namespace

std::string Engine::ExecCreateTable(const Statement& st) {
  std::string tname = LowerName(st.table);
  if (table_ids_.count(tname)) {
    return st.if_not_exists ? JsonOk() : JsonError("table_exists");
  }
  if (st.columns.empty()) return JsonError("syntax_error");
  Schema s;
  s.name = tname;
  s.id = next_table_id_;
  int vector_cols = 0;
  int pk_col = -1;
  for (const auto& def : st.columns) {
    Column c;
    c.name = def.name;
    c.lname = LowerName(def.name);
    if (s.FindCol(c.lname) >= 0) return JsonError("duplicate_column");
    c.type = def.type;
    if (c.type.kind == ColumnType::VECTOR) {
      if (++vector_cols > 1) return JsonError("unsupported_sql");
      s.vec = static_cast<int>(s.cols.size());
    }
    if (def.primary) {
      if (pk_col >= 0) return JsonError("syntax_error");
      if (c.type.kind != ColumnType::BIGINT) return JsonError("unsupported_sql");
      pk_col = static_cast<int>(s.cols.size());
    }
    s.cols.push_back(std::move(c));
  }
  if (pk_col < 0) return JsonError("primary_key_required");
  s.pk = pk_col;
  uint32_t txn = next_txn_++;
  std::string payload = SerializeSchema(s);
  if (!AppendRecord(REC_BEGIN, txn, std::string(), nullptr)) return JsonError("io_error");
  if (!AppendRecord(REC_SCHEMA, txn, payload, nullptr)) return JsonError("io_error");
  if (!AppendRecord(REC_COMMIT, txn, std::string(), nullptr)) return JsonError("io_error");
  if (!SyncWal()) return JsonError("io_error");
  RegisterSchema(s);
  return JsonOk();
}

std::string Engine::ExecCreateIndex(const Statement& st) {
  TableState* t = FindTable(LowerName(st.table));
  if (t == nullptr) return JsonError("unknown_table");
  int idx = t->schema.FindCol(LowerName(st.index_col));
  if (idx < 0) return JsonError("unknown_column");
  if (t->schema.cols[idx].type.kind != ColumnType::VECTOR) return JsonError("unsupported_sql");
  Schema s = t->schema;
  s.has_vec_index = true;
  uint32_t txn = next_txn_++;
  std::string payload = SerializeSchema(s);
  if (!AppendRecord(REC_BEGIN, txn, std::string(), nullptr)) return JsonError("io_error");
  if (!AppendRecord(REC_SCHEMA, txn, payload, nullptr)) return JsonError("io_error");
  if (!AppendRecord(REC_COMMIT, txn, std::string(), nullptr)) return JsonError("io_error");
  if (!SyncWal()) return JsonError("io_error");
  RegisterSchema(s);
  return JsonOk();
}

std::string Engine::ExecBegin() {
  if (txn_active_) return JsonError("transaction_state_error");
  txn_active_ = true;
  overlay_.clear();
  return JsonOk();
}

std::string Engine::ExecCommit() {
  if (!txn_active_) return JsonOk();
  txn_active_ = false;
  bool ok = CommitOverlay();
  overlay_.clear();
  return ok ? JsonOk() : JsonError("io_error");
}

std::string Engine::ExecRollback() {
  txn_active_ = false;
  overlay_.clear();
  return JsonOk();
}

std::string Engine::ExecInsert(const Statement& st) {
  TableState* t = FindTable(LowerName(st.table));
  if (t == nullptr) return JsonError("unknown_table");
  const Schema& s = t->schema;
  std::vector<int> targets;
  if (st.insert_cols.empty()) {
    for (size_t i = 0; i < s.cols.size(); ++i) targets.push_back(static_cast<int>(i));
  } else {
    for (const auto& name : st.insert_cols) {
      int idx = s.FindCol(LowerName(name));
      if (idx < 0) return JsonError("unknown_column");
      if (std::find(targets.begin(), targets.end(), idx) != targets.end())
        return JsonError("syntax_error");
      targets.push_back(idx);
    }
  }
  auto overlay_it = overlay_.find(s.id);
  std::map<int64_t, OverlayRow>* ov = overlay_it == overlay_.end() ? nullptr : &overlay_it->second;
  std::vector<std::pair<int64_t, std::string>> ready;
  std::unordered_set<int64_t> seen;
  for (const auto& row : st.rows) {
    if (row.size() != targets.size()) return JsonError("column_count_mismatch");
    std::vector<ColumnValue> vals(s.cols.size());
    std::vector<bool> present(s.cols.size(), false);
    for (size_t i = 0; i < targets.size(); ++i) {
      std::string err;
      if (!BindLiteral(s.cols[targets[i]], row[i], &vals[targets[i]], err))
        return JsonError(err);
      if (s.cols[targets[i]].type.kind == ColumnType::VARCHAR &&
          Utf8CodePoints(vals[targets[i]].s) > s.cols[targets[i]].type.param)
        return JsonError("value_too_long");
      present[targets[i]] = true;
    }
    for (size_t c = 0; c < s.cols.size(); ++c) {
      if (!present[c]) return JsonError("missing_column_value");
    }
    int64_t pk = vals[s.pk].i;
    bool dup = seen.count(pk) != 0;
    if (!dup) {
      if (ov != nullptr) {
        auto it = ov->find(pk);
        if (it != ov->end() && !it->second.del) dup = true;
      }
      if (!dup && t->rows.count(pk)) dup = true;
    }
    if (dup) return JsonError("duplicate_key");
    seen.insert(pk);
    ready.emplace_back(pk, EncodeRow(s, vals));
  }
  for (auto& [pk, bytes] : ready) {
    overlay_[s.id][pk] = OverlayRow{false, std::move(bytes)};
  }
  if (!txn_active_) {
    if (!CommitOverlay()) return JsonError("io_error");
  }
  return JsonAffected(static_cast<int64_t>(ready.size()));
}

std::string Engine::ExecSelect(const Statement& st, const OutSink& sink) {
  TableState* t = FindTable(LowerName(st.table));
  if (t == nullptr) return JsonError("unknown_table");
  const Schema& s = t->schema;

  struct Item {
    int kind = 0;  // 0 star, 1 col, 2 distance
    int col = -1;
    std::vector<float> qvec;
    std::string name;
  };
  std::vector<Item> items;
  std::map<std::string, int> name_map;
  for (const auto& si : st.select_items) {
    Item it;
    if (si.star) {
      it.kind = 0;
    } else if (si.is_distance) {
      int idx = s.FindCol(LowerName(si.col));
      if (idx < 0) return JsonError("unknown_column");
      if (s.cols[idx].type.kind != ColumnType::VECTOR) return JsonError("unsupported_sql");
      std::string err;
      std::vector<float> q;
      if (!BindQueryVector(s.cols[idx], si.qvec, &q, err)) return JsonError(err);
      it.kind = 2;
      it.col = idx;
      it.qvec = std::move(q);
      it.name = si.alias.empty() ? "l2_distance" : si.alias;
      name_map[LowerName(it.name)] = static_cast<int>(items.size());
    } else {
      int idx = s.FindCol(LowerName(si.col));
      if (idx < 0) return JsonError("unknown_column");
      it.kind = 1;
      it.col = idx;
      it.name = si.alias.empty() ? s.cols[idx].name : si.alias;
      name_map[LowerName(it.name)] = static_cast<int>(items.size());
    }
    items.push_back(std::move(it));
  }

  std::vector<CondBound> conds;
  std::string err;
  if (!BuildPredicates(s, st.where, &conds, err)) return JsonError(err);

  struct OrderBound {
    int item = -1;
    int col = -1;
    bool desc = false;
    bool dist_expr = false;  // ORDER BY L2_DISTANCE(...) that is not a select item
    int dcol = -1;
    std::vector<float> dqvec;
  };
  std::vector<OrderBound> orders;
  for (const auto& oi : st.order) {
    OrderBound ob;
    ob.desc = oi.desc;
    if (oi.is_distance) {
      int idx = s.FindCol(LowerName(oi.col));
      if (idx < 0) return JsonError("unknown_column");
      if (s.cols[idx].type.kind != ColumnType::VECTOR) return JsonError("unsupported_sql");
      std::string err;
      std::vector<float> q;
      if (!BindQueryVector(s.cols[idx], oi.qvec, &q, err)) return JsonError(err);
      int match = -1;
      for (size_t j = 0; j < items.size(); ++j) {
        if (items[j].kind == 2 && items[j].col == idx && items[j].qvec == q) {
          match = static_cast<int>(j);
          break;
        }
      }
      if (match >= 0) {
        ob.item = match;
      } else {
        ob.dist_expr = true;
        ob.dcol = idx;
        ob.dqvec = std::move(q);
      }
      orders.push_back(std::move(ob));
      continue;
    }
    std::string lname = LowerName(oi.name);
    auto it = name_map.find(lname);
    if (it != name_map.end()) {
      ob.item = it->second;
    } else {
      int idx = s.FindCol(lname);
      if (idx < 0) return JsonError("unknown_column");
      ob.col = idx;
    }
    orders.push_back(std::move(ob));
  }

  bool need_dist = false;
  for (const auto& it : items) if (it.kind == 2) need_dist = true;
  for (const auto& ob : orders) {
    if (ob.item >= 0 && items[ob.item].kind == 2) need_dist = true;
  }
  // A distance item's column is needed for evaluation.
  for (auto& it : items) {
    if (it.kind == 2) it.col = it.col;  // resolved above
  }

  auto value_of_item = [&](const Item& it, const RowView& v, double dist) -> OutVal {
    OutVal out;
    if (it.kind == 2) {
      out.kind = OutVal::FLT;
      out.f = std::sqrt(dist);
      return out;
    }
    const uint8_t* p = nullptr;
    uint32_t len = 0;
    ColAt(v, s, it.col, &p, &len);
    switch (s.cols[it.col].type.kind) {
      case ColumnType::BIGINT:
        out.kind = OutVal::INT;
        out.i = GetI64(p);
        break;
      case ColumnType::VARCHAR:
        out.kind = OutVal::STR;
        out.s.assign(reinterpret_cast<const char*>(p), len);
        break;
      case ColumnType::VECTOR:
        out.kind = OutVal::VEC;
        out.v.resize(s.cols[it.col].type.param);
        for (uint32_t d = 0; d < s.cols[it.col].type.param; ++d) out.v[d] = GetF32(p + d * 4);
        break;
    }
    return out;
  };

  auto out_columns = [&]() {
    std::vector<std::string> cols;
    for (const auto& it : items) {
      if (it.kind == 0) {
        for (const auto& c : s.cols) cols.push_back(c.name);
      } else {
        cols.push_back(it.name);
      }
    }
    return cols;
  };

  auto compute_dist = [&](const RowView& v, int vcol, const std::vector<float>& q) {
    const uint8_t* p = nullptr;
    uint32_t len = 0;
    ColAt(v, s, vcol, &p, &len);
    const float* a = reinterpret_cast<const float*>(p);
    double sum = 0.0;
    for (size_t i = 0; i < q.size(); ++i) {
      double d = static_cast<double>(a[i]) - static_cast<double>(q[i]);
      sum += d * d;
    }
    return sum;
  };

  std::vector<float> query_vec;
  int dist_col = -1;
  if (need_dist) {
    for (const auto& it : items) {
      if (it.kind == 2) { dist_col = it.col; query_vec = it.qvec; break; }
    }
    if (dist_col < 0) {
      for (const auto& ob : orders) {
        if (ob.item >= 0 && items[ob.item].kind == 2) {
          dist_col = items[ob.item].col;
          query_vec = items[ob.item].qvec;
          break;
        }
      }
    }
  }

  bool distance_order =
      need_dist && st.limit >= 0 && orders.size() == 1 && orders[0].item >= 0 &&
      items[orders[0].item].kind == 2 && !orders[0].desc;

  auto exact_distance = [&](const RowView& v) {
    const uint8_t* p = nullptr;
    uint32_t len = 0;
    ColAt(v, s, dist_col, &p, &len);
    const float* a = reinterpret_cast<const float*>(p);
    double sum = 0.0;
    for (size_t i = 0; i < query_vec.size(); ++i) {
      double d = static_cast<double>(a[i]) - static_cast<double>(query_vec[i]);
      sum += d * d;
    }
    return sum;
  };

  // A sort reference keeps only the primary key, distance and a view pointing
  // into the mmap'd storage (or the transaction overlay), both of which stay
  // valid for the whole query. Ordering a result larger than RAM therefore
  // never copies row bodies.
  struct SortRef {
    int64_t pk = 0;
    double dist = 0;
    RowView view;
  };

  auto cmp_double = [](double a, double b) { return a < b ? -1 : (a > b ? 1 : 0); };

  auto dist_expr_of = [&](const OrderBound& ob, const RowView& v) {
    const uint8_t* p = nullptr;
    uint32_t len = 0;
    if (!ColAt(v, s, ob.dcol, &p, &len)) return 0.0;
    const float* a = reinterpret_cast<const float*>(p);
    double sum = 0.0;
    for (size_t i = 0; i < ob.dqvec.size(); ++i) {
      double d = static_cast<double>(a[i]) - static_cast<double>(ob.dqvec[i]);
      sum += d * d;
    }
    return sum;
  };

  auto less_ref = [&](const SortRef& a, const SortRef& b) {
    for (const auto& ob : orders) {
      int sgn = 0;
      if (ob.dist_expr) {
        sgn = cmp_double(dist_expr_of(ob, a.view), dist_expr_of(ob, b.view));
      } else if (ob.item >= 0 && items[ob.item].kind == 2) {
        sgn = cmp_double(a.dist, b.dist);
      } else {
        int col = ob.item >= 0 ? items[ob.item].col : ob.col;
        const uint8_t *pa = nullptr, *pb = nullptr;
        uint32_t la = 0, lb = 0;
        ColAt(a.view, s, col, &pa, &la);
        ColAt(b.view, s, col, &pb, &lb);
        if (s.cols[col].type.kind == ColumnType::BIGINT) {
          int64_t xa = GetI64(pa), xb = GetI64(pb);
          sgn = xa < xb ? -1 : (xa > xb ? 1 : 0);
        } else {
          std::string_view xa(reinterpret_cast<const char*>(pa), la);
          std::string_view xb(reinterpret_cast<const char*>(pb), lb);
          int c = xa.compare(xb);
          sgn = c < 0 ? -1 : (c > 0 ? 1 : 0);
        }
      }
      if (sgn != 0) return ob.desc ? sgn > 0 : sgn < 0;
    }
    return a.pk < b.pk;
  };

  std::string rowbuf;
  bool first_row = true;
  auto emit_row = [&](const RowView& v, double dist) {
    rowbuf.clear();
    if (!first_row) rowbuf += ',';
    first_row = false;
    rowbuf += '[';
    bool firstc = true;
    auto add_val = [&](const OutVal& ov) {
      if (!firstc) rowbuf += ',';
      firstc = false;
      OutValToJson(rowbuf, ov);
    };
    for (const auto& it : items) {
      if (it.kind == 0) {
        for (size_t c = 0; c < s.cols.size(); ++c) {
          Item tmp;
          tmp.kind = 1;
          tmp.col = static_cast<int>(c);
          add_val(value_of_item(tmp, v, dist));
        }
      } else {
        add_val(value_of_item(it, v, dist));
      }
    }
    rowbuf += ']';
    sink(rowbuf.data(), rowbuf.size());
  };
  auto emit_header = [&]() {
    std::string header = "{\"ok\":true,\"columns\":[";
    std::vector<std::string> cols = out_columns();
    for (size_t i = 0; i < cols.size(); ++i) {
      if (i) header += ',';
      header += '"';
      JsonEscapeTo(header, cols[i]);
      header += '"';
    }
    header += "],\"rows\":[";
    sink(header.data(), header.size());
  };
  auto emit_tail = [&]() { sink("]}", 2); };

  if (distance_order) {
    size_t k = static_cast<size_t>(st.limit);
    // Keep a margin of candidates selected with the fast float32 distance, then
    // re-rank them with the exact double distance and cut down to K. This keeps
    // exact mode exact at the K boundary without paying double cost per row.
    size_t k_candidates = k == 0 ? 0 : k + std::max<size_t>(k * 3, 64);
    // priority_queue keeps the "largest" element on top; with this ordering the
    // largest is the worst candidate (largest distance, then largest primary key).
    auto worse_first = [](const SortRef& a, const SortRef& b) {
      if (a.dist != b.dist) return a.dist < b.dist;
      return a.pk < b.pk;
    };
    std::priority_queue<SortRef, std::vector<SortRef>, decltype(worse_first)> heap(worse_first);
    const uint32_t vec_off = s.fixed_off[dist_col];
    const float* qptr = query_vec.data();
    const int qdim = static_cast<int>(query_vec.size());
    // 热路径直接用已解析好的函数指针，避免每行再过一次成员函数与守卫检查。
    const L2Fn l2fn = g_l2_fn;
    auto consider = [&](int64_t pk, const RowView& v) {
      double dist = l2fn(reinterpret_cast<const float*>(v.p + vec_off), qptr, qdim);
      if (heap.size() < k_candidates) {
        heap.push(SortRef{pk, dist, v});
      } else {
        const SortRef& top = heap.top();
        if (dist < top.dist || (dist == top.dist && pk < top.pk)) {
          heap.pop();
          heap.push(SortRef{pk, dist, v});
        }
      }
    };
    auto ovit = overlay_.find(s.id);
    bool no_overlay = ovit == overlay_.end() || ovit->second.empty();
    bool cacheable = false;
    if (no_overlay && conds.size() == 1 && conds[0].op == CmpOp::EQ &&
        t->rows.size() <= 500000) {
      const Column& fc = s.cols[conds[0].col];
      cacheable = (fc.type.kind == ColumnType::BIGINT) ||
                  (fc.type.kind == ColumnType::VARCHAR && fc.type.param <= 64);
    }
    if (cacheable && k_candidates > 0) {
      EnsureScan(*t);
      const CondBound& cb = conds[0];
      bool is_str = s.cols[cb.col].type.kind == ColumnType::VARCHAR;
      if (t->cache_col != cb.col) {
        t->cache_col = cb.col;
        t->cache_ids.clear();
        t->cache_dict.clear();
        t->cache_int.clear();
        t->cache_lists.clear();
        t->cache_int_lists.clear();
        size_t n = t->scan.size();
        if (is_str) {
          t->cache_ids.assign(n, 0);
          for (size_t i = 0; i < n; ++i) {
            RowView v = MakeView(t->scan[i].second);
            v.s = &s;
            const uint8_t* p = nullptr;
            uint32_t l = 0;
            if (!ColAt(v, s, cb.col, &p, &l)) continue;
            std::string key(reinterpret_cast<const char*>(p), l);
            auto it = t->cache_dict.find(key);
            uint32_t id;
            if (it == t->cache_dict.end()) {
              id = static_cast<uint32_t>(t->cache_dict.size());
              t->cache_dict.emplace(std::move(key), id);
            } else {
              id = it->second;
            }
            t->cache_ids[i] = id;
            t->cache_lists[id].push_back(static_cast<uint32_t>(i));
          }
        } else {
          t->cache_int.assign(n, 0);
          for (size_t i = 0; i < n; ++i) {
            RowView v = MakeView(t->scan[i].second);
            v.s = &s;
            const uint8_t* p = nullptr;
            uint32_t l = 0;
            if (ColAt(v, s, cb.col, &p, &l)) {
              t->cache_int[i] = GetI64(p);
              t->cache_int_lists[t->cache_int[i]].push_back(static_cast<uint32_t>(i));
            }
          }
        }
      }
      uint32_t want_id = 0;
      bool want_found = true;
      if (is_str) {
        auto it = t->cache_dict.find(cb.sval);
        want_found = it != t->cache_dict.end();
        if (want_found) want_id = it->second;
      }
      if (!is_str || want_found) {
        // 倒排索引：只遍历命中行（原先每个过滤查询都要扫一遍全部行的缓存条目）。
        const std::vector<uint32_t>* hit = nullptr;
        if (is_str) {
          auto lit = t->cache_lists.find(want_id);
          if (lit != t->cache_lists.end()) hit = &lit->second;
        } else {
          auto lit = t->cache_int_lists.find(cb.ival);
          if (lit != t->cache_int_lists.end()) hit = &lit->second;
        }
        if (hit != nullptr) {
          const std::vector<uint32_t>& hit_rows = *hit;
          for (size_t j = 0; j < hit_rows.size(); ++j) {
            const uint32_t i = hit_rows[j];
            RowView v = MakeView(t->scan[i].second);
            v.s = &s;
            if (!v.Valid()) continue;
            consider(t->scan[i].first, v);
          }
        }
      }
    } else if (k_candidates > 0) {
      ForEachRow(*t, [&](int64_t pk, const RowView& v) {
        if (!EvalPredicates(conds, v)) return;
        consider(pk, v);
      });
    }
    std::vector<SortRef> results;
    results.reserve(heap.size());
    while (!heap.empty()) {
      results.push_back(heap.top());
      heap.pop();
    }
    for (auto& r : results) r.dist = exact_distance(r.view);
    std::sort(results.begin(), results.end(), [](const SortRef& a, const SortRef& b) {
      if (a.dist != b.dist) return a.dist < b.dist;
      return a.pk < b.pk;
    });
    if (results.size() > k) results.resize(k);
    emit_header();
    for (const auto& r : results) emit_row(r.view, r.dist);
    emit_tail();
  } else {
    emit_header();
    if (orders.empty()) {
      int64_t emitted = 0;
      ForEachRow(*t, [&](int64_t, const RowView& v) {
        if (st.limit >= 0 && emitted >= st.limit) return;
        if (!EvalPredicates(conds, v)) return;
        double dist = need_dist ? compute_dist(v, dist_col, query_vec) : 0.0;
        emit_row(v, dist);
        ++emitted;
      });
    } else if (st.limit >= 0) {
      size_t k = static_cast<size_t>(st.limit);
      if (k > 0) {
        std::priority_queue<SortRef, std::vector<SortRef>, decltype(less_ref)> heap(less_ref);
        ForEachRow(*t, [&](int64_t pk, const RowView& v) {
          if (!EvalPredicates(conds, v)) return;
          double dist = need_dist ? compute_dist(v, dist_col, query_vec) : 0.0;
          SortRef r{pk, dist, v};
          if (heap.size() < k) {
            heap.push(r);
          } else if (less_ref(r, heap.top())) {
            heap.pop();
            heap.push(r);
          }
        });
        std::vector<SortRef> refs;
        refs.reserve(heap.size());
        while (!heap.empty()) {
          refs.push_back(heap.top());
          heap.pop();
        }
        std::sort(refs.begin(), refs.end(), less_ref);
        for (const auto& r : refs) emit_row(r.view, r.dist);
      }
    } else {
      std::vector<SortRef> refs;
      ForEachRow(*t, [&](int64_t pk, const RowView& v) {
        if (!EvalPredicates(conds, v)) return;
        double dist = need_dist ? compute_dist(v, dist_col, query_vec) : 0.0;
        refs.push_back(SortRef{pk, dist, v});
      });
      std::sort(refs.begin(), refs.end(), less_ref);
      for (const auto& r : refs) emit_row(r.view, r.dist);
    }
    emit_tail();
  }
  return std::string();
}

std::string Engine::ExecUpdate(const Statement& st) {
  TableState* t = FindTable(LowerName(st.table));
  if (t == nullptr) return JsonError("unknown_table");
  const Schema& s = t->schema;
  struct SetBound {
    int col;
    ColumnValue val;
  };
  std::vector<SetBound> sets;
  for (const auto& [name, lit] : st.sets) {
    int idx = s.FindCol(LowerName(name));
    if (idx < 0) return JsonError("unknown_column");
    if (idx == s.pk) return JsonError("unsupported_sql");
    for (const auto& sb : sets) {
      if (sb.col == idx) return JsonError("syntax_error");
    }
    SetBound sb;
    sb.col = idx;
    std::string err;
    if (!BindLiteral(s.cols[idx], lit, &sb.val, err)) return JsonError(err);
    if (s.cols[idx].type.kind == ColumnType::VARCHAR &&
        Utf8CodePoints(sb.val.s) > s.cols[idx].type.param)
      return JsonError("value_too_long");
    sets.push_back(std::move(sb));
  }
  std::vector<CondBound> conds;
  std::string err;
  if (!BuildPredicates(s, st.where, &conds, err)) return JsonError(err);

  auto apply_sets = [&](std::vector<ColumnValue>& vals) {
    for (const auto& sb : sets) {
      if (s.cols[sb.col].type.kind == ColumnType::VECTOR) {
        vals[sb.col].v = sb.val.v;
      } else if (s.cols[sb.col].type.kind == ColumnType::VARCHAR) {
        vals[sb.col].s = sb.val.s;
      } else {
        vals[sb.col].i = sb.val.i;
      }
    }
  };
  if (!txn_active_) {
    // Autocommit: stream new row versions straight into a WAL transaction so a
    // bulk update never holds every changed body in memory. Each view is fully
    // decoded before its append because growing the WAL may remap it.
    uint32_t txn = next_txn_++;
    uint64_t wal_start = wal_size_;
    std::string empty;
    if (!AppendRecord(REC_BEGIN, txn, empty, nullptr)) return JsonError("io_error");
    struct UpdRef {
      int64_t pk;
      uint64_t off;
    };
    std::vector<UpdRef> changed;
    bool failed = false;
    ForEachRow(*t, [&](int64_t pk, const RowView& v) {
      if (failed || !EvalPredicates(conds, v)) return;
      std::vector<ColumnValue> vals = DecodeRow(s, v);
      apply_sets(vals);
      std::string payload;
      PutU32(payload, s.id);
      PutI64(payload, pk);
      payload += EncodeRow(s, vals);
      uint64_t off = 0;
      if (!AppendRecord(REC_PUT, txn, payload, &off)) {
        failed = true;
        return;
      }
      changed.push_back(UpdRef{pk, off});
    });
    if (failed || !AppendRecord(REC_COMMIT, txn, empty, nullptr) || !SyncWal()) {
      wal_size_ = wal_start;
      return JsonError("io_error");
    }
    for (const auto& c : changed) t->rows[c.pk] = RowRef{c.off, 1};
    t->scan_dirty = true;
      return JsonAffected(static_cast<int64_t>(changed.size()));
  }
  std::vector<std::pair<int64_t, std::string>> changed;
  ForEachRow(*t, [&](int64_t pk, const RowView& v) {
    if (!EvalPredicates(conds, v)) return;
    std::vector<ColumnValue> vals = DecodeRow(s, v);
    apply_sets(vals);
    changed.emplace_back(pk, EncodeRow(s, vals));
  });
  for (auto& [pk, bytes] : changed) {
    overlay_[s.id][pk] = OverlayRow{false, std::move(bytes)};
  }
  return JsonAffected(static_cast<int64_t>(changed.size()));
}

std::string Engine::ExecDelete(const Statement& st) {
  TableState* t = FindTable(LowerName(st.table));
  if (t == nullptr) return JsonError("unknown_table");
  const Schema& s = t->schema;
  std::vector<CondBound> conds;
  std::string err;
  if (!BuildPredicates(s, st.where, &conds, err)) return JsonError(err);
  if (!txn_active_) {
    // Autocommit: stream tombstones into a WAL transaction; only the primary
    // keys stay in memory until the commit is durable.
    uint32_t txn = next_txn_++;
    uint64_t wal_start = wal_size_;
    std::string empty;
    if (!AppendRecord(REC_BEGIN, txn, empty, nullptr)) return JsonError("io_error");
    std::vector<int64_t> deleted;
    bool failed = false;
    ForEachRow(*t, [&](int64_t pk, const RowView& v) {
      if (failed || !EvalPredicates(conds, v)) return;
      std::string payload;
      PutU32(payload, s.id);
      PutI64(payload, pk);
      if (!AppendRecord(REC_DEL, txn, payload, nullptr)) {
        failed = true;
        return;
      }
      deleted.push_back(pk);
    });
    if (failed || !AppendRecord(REC_COMMIT, txn, empty, nullptr) || !SyncWal()) {
      wal_size_ = wal_start;
      return JsonError("io_error");
    }
    for (int64_t pk : deleted) t->rows.erase(pk);
    t->scan_dirty = true;
      return JsonAffected(static_cast<int64_t>(deleted.size()));
  }
  std::vector<int64_t> deleted;
  ForEachRow(*t, [&](int64_t pk, const RowView& v) {
    if (EvalPredicates(conds, v)) deleted.push_back(pk);
  });
  for (int64_t pk : deleted) {
    OverlayRow row;
    row.del = true;
    overlay_[s.id][pk] = std::move(row);
  }
  return JsonAffected(static_cast<int64_t>(deleted.size()));
}

// Compaction runs as a session spread across statements. At session start the
// WAL ends at a complete-transaction boundary (the watermark); the new snapshot
// is written from the live committed rows while new transactions keep appending
// to the WAL and are copied into wal.tmp. Recovery invariant: snapshot plus a
// replay of every transaction after the watermark reproduces the exact state,
// because PUT/DEL are absolute per-primary-key operations. A crash at any point
// leaves either (old snapshot + full WAL) or (new snapshot + full WAL) or
// (new snapshot + WAL tail); all three recover correctly.
void Engine::MaybeCompactStep() {
  if (!compacting_) {
    if (wal_size_ <= kCompactWalBytes || txn_active_ || !overlay_.empty()) return;
    if (!StartCompact()) return;
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
  const uint64_t byte_limit = compact_off_ + (64u << 20);
  for (;;) {
    auto it = tables_.lower_bound(compact_next_tid_);
    if (it == tables_.end()) {
      FinishCompact();
      return;
    }
    TableState& ts = it->second;
    if (!compact_schema_done_) {
      if (!CompactEmit(REC_SCHEMA, 0, SerializeSchema(ts.schema))) {
        AbortCompact();
        return;
      }
      compact_schema_done_ = true;
    }
    auto rit = compact_have_last_ ? ts.rows.upper_bound(compact_last_pk_) : ts.rows.begin();
    while (rit != ts.rows.end()) {
      uint32_t len = 0;
      const uint8_t* payload = RowPayload(rit->second, &len);
      if (payload != nullptr) {
        std::string rec;
        PutU32(rec, it->first);
        PutI64(rec, rit->first);
        rec.append(reinterpret_cast<const char*>(payload), len);
        uint64_t this_off = compact_off_;
        if (!CompactEmit(REC_PUT, 0, rec)) {
          AbortCompact();
          return;
        }
        compact_refs_.push_back(CompactRef{it->first, rit->first, this_off, rit->second});
      }
      compact_last_pk_ = rit->first;
      compact_have_last_ = true;
      ++rit;
      if ((compact_refs_.size() & 255) == 0 &&
          (std::chrono::steady_clock::now() >= deadline || compact_off_ >= byte_limit)) {
        if (!CompactCopyTail()) AbortCompact();
        return;
      }
    }
    compact_next_tid_ = it->first + 1;
    compact_schema_done_ = false;
    compact_have_last_ = false;
    if (std::chrono::steady_clock::now() >= deadline || compact_off_ >= byte_limit) {
      if (!CompactCopyTail()) AbortCompact();
      return;
    }
  }
}

bool Engine::StartCompact() {
  std::string tmp = dir_ + "/snapshot.tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return false;
  std::string wtmp = dir_ + "/wal.tmp";
  int wfd = ::open(wtmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (wfd < 0) {
    ::close(fd);
    ::unlink(tmp.c_str());
    return false;
  }
  compact_fd_ = fd;
  compact_tail_fd_ = wfd;
  compact_watermark_ = wal_size_;
  compact_tail_copied_ = 0;
  compact_next_tid_ = tables_.empty() ? 1 : tables_.begin()->first;
  compact_schema_done_ = false;
  compact_have_last_ = false;
  compact_last_pk_ = 0;
  compact_off_ = 0;
  compact_buf_.clear();
  compact_refs_.clear();
  compacting_ = true;
  return true;
}

void Engine::AbortCompact() {
  if (compact_fd_ >= 0) {
    ::close(compact_fd_);
    compact_fd_ = -1;
  }
  if (compact_tail_fd_ >= 0) {
    ::close(compact_tail_fd_);
    compact_tail_fd_ = -1;
  }
  ::unlink((dir_ + "/snapshot.tmp").c_str());
  ::unlink((dir_ + "/wal.tmp").c_str());
  compact_buf_.clear();
  compact_refs_.clear();
  compacting_ = false;
}

bool Engine::CompactFlush() {
  if (!compact_buf_.empty()) {
    if (!WriteAll(compact_fd_, compact_buf_.data(), compact_buf_.size())) return false;
#ifdef __linux__
    // Kick background writeback for the range just written so the final fsync
    // does not have to flush the whole multi-GB snapshot in one response.
    ::sync_file_range(compact_fd_, static_cast<off_t>(compact_off_ - compact_buf_.size()),
                      static_cast<off_t>(compact_buf_.size()), SYNC_FILE_RANGE_WRITE);
#endif
    compact_buf_.clear();
  }
  return true;
}

bool Engine::CompactEmit(uint8_t type, uint32_t txn, const std::string& payload) {
  uint8_t hdr[kRecHeader];
  uint32_t len = static_cast<uint32_t>(payload.size());
  uint32_t crc = Crc32(payload.data(), payload.size());
  std::memcpy(hdr, &kMagic, 4);
  std::memcpy(hdr + 4, &len, 4);
  std::memcpy(hdr + 8, &crc, 4);
  hdr[12] = type;
  std::memcpy(hdr + 13, &txn, 4);
  compact_buf_.append(reinterpret_cast<char*>(hdr), kRecHeader);
  compact_buf_ += payload;
  compact_off_ += kRecHeader + payload.size();
  if (compact_buf_.size() > (4u << 20)) return CompactFlush();
  return true;
}

bool Engine::CompactCopyTail() {
  uint64_t pos = compact_watermark_ + compact_tail_copied_;
  while (pos < wal_size_) {
    size_t n = static_cast<size_t>(std::min<uint64_t>(wal_size_ - pos, 1u << 20));
    if (!WriteAll(compact_tail_fd_, wal_base_ + pos, n)) return false;
    pos += n;
  }
  compact_tail_copied_ = wal_size_ - compact_watermark_;
  return ::fdatasync(compact_tail_fd_) == 0;
}

bool Engine::FinishCompact() {
  const auto t_start = std::chrono::steady_clock::now();
  bool ok = CompactFlush();
  if (ok) ok = ::fsync(compact_fd_) == 0;
  ::close(compact_fd_);
  compact_fd_ = -1;
  const auto t_sync = std::chrono::steady_clock::now();
  if (ok) {
    ok = ::rename((dir_ + "/snapshot.tmp").c_str(), (dir_ + "/snapshot.dat").c_str()) == 0;
    if (ok) ok = FsyncDir(dir_);
  }
  if (ok) ok = CompactCopyTail();
  if (ok) {
    ::close(compact_tail_fd_);
    compact_tail_fd_ = -1;
    ok = ::rename((dir_ + "/wal.tmp").c_str(), (dir_ + "/wal.log").c_str()) == 0;
    if (ok) ok = FsyncDir(dir_);
  }
  if (!ok) {
    // The old snapshot mapping and the full WAL are still a consistent pair.
    AbortCompact();
    return false;
  }
  if (!OpenSnapshot()) {
    ok_ = false;
    init_error_ = "io_error";
    compacting_ = false;
    return false;
  }
  const auto t_swap = std::chrono::steady_clock::now();
  // Relocate rows whose version did not change during the session and rebase
  // the WAL references that survive the truncation. compact_refs_ is sorted by
  // (tid, pk), so this is a single ordered merge pass over the live indexes.
  size_t ri = 0;
  for (auto& [tid, ts] : tables_) {
    while (ri < compact_refs_.size() && compact_refs_[ri].tid < tid) ++ri;
    size_t rj = ri;
    for (auto& [pk, ref] : ts.rows) {
      while (rj < compact_refs_.size() && compact_refs_[rj].tid == tid &&
             compact_refs_[rj].pk < pk) {
        ++rj;
      }
      if (rj < compact_refs_.size() && compact_refs_[rj].tid == tid &&
          compact_refs_[rj].pk == pk) {
        const CompactRef& nr = compact_refs_[rj];
        if (ref.off == nr.src.off && ref.file == nr.src.file) ref = RowRef{nr.off, 0};
      }
      if (ref.file == 1 && ref.off >= compact_watermark_) ref.off -= compact_watermark_;
    }
    ts.scan_dirty = true;
    ts.cache_col = -1;
    ts.cache_ids.clear();
    ts.cache_dict.clear();
    ts.cache_int.clear();
  }
  compact_refs_.clear();
  uint64_t tail = compact_tail_copied_;
  if (wal_base_ != nullptr) {
    ::munmap(wal_base_, wal_cap_);
    wal_base_ = nullptr;
  }
  if (wal_fd_ >= 0) ::close(wal_fd_);
  wal_fd_ = ::open((dir_ + "/wal.log").c_str(), O_RDWR);
  size_t cap = static_cast<size_t>(tail) + kWalGrowBytes;
  if (wal_fd_ < 0 || ::ftruncate(wal_fd_, static_cast<off_t>(cap)) != 0) {
    ok_ = false;
    init_error_ = "io_error";
    compacting_ = false;
    return false;
  }
  void* p = ::mmap(nullptr, cap, PROT_READ | PROT_WRITE, MAP_SHARED, wal_fd_, 0);
  if (p == MAP_FAILED) {
    ok_ = false;
    init_error_ = "io_error";
    compacting_ = false;
    return false;
  }
  wal_base_ = static_cast<uint8_t*>(p);
  wal_size_ = static_cast<size_t>(tail);
  wal_cap_ = cap;
  compacting_ = false;
  const auto t_end = std::chrono::steady_clock::now();
  std::fprintf(stderr,
               "compact done: snapshot=%lluMB tail=%lluMB sync=%lldms swap=%lldms relocate=%lldms\n",
               static_cast<unsigned long long>(compact_off_ >> 20),
               static_cast<unsigned long long>(tail >> 20),
               static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(t_sync - t_start).count()),
               static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(t_swap - t_sync).count()),
               static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_swap).count()));
  std::fflush(stderr);
  return true;
}

void Engine::Shutdown() {
  if (compacting_) AbortCompact();
  overlay_.clear();
  txn_active_ = false;
  // No compaction here: the driver waits for the process to exit on --restart,
  // and rewriting a multi-GB snapshot would stall that wait. The next startup
  // replays the bounded WAL instead.
  if (wal_fd_ >= 0) ::fdatasync(wal_fd_);
  CloseAll();
}

}  // namespace vdb
