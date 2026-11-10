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
#include <limits>
#include <queue>
#include <utility>

#if defined(__x86_64__)
#include <immintrin.h>
#endif

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
constexpr size_t kCompactWalBytes = 128u << 20;
constexpr size_t kShutdownCompactBytes = 16u << 20;

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

bool ParseVecText(const std::string& text, std::vector<double>* out) {
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
    char* endp = nullptr;
    const char* start = text.c_str() + i;
    double v = std::strtod(start, &endp);
    if (endp == start) return false;
    i = static_cast<size_t>(endp - text.c_str());
    out->push_back(v);
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
      if (Utf8CodePoints(lit.s) > col.type.param) {
        err = "value_too_long";
        return false;
      }
      out->s = lit.s;
      return true;
    case ColumnType::VECTOR: {
      std::vector<double> nums;
      if (lit.kind == LitKind::Vec) {
        nums = lit.nums;
      } else if (lit.kind == LitKind::Str) {
        if (!ParseVecText(lit.s, &nums)) {
          err = "invalid_vector";
          return false;
        }
      } else {
        err = "type_mismatch";
        return false;
      }
      if (nums.size() != col.type.param) {
        err = "dimension_mismatch";
        return false;
      }
      out->v.resize(nums.size());
      for (size_t i = 0; i < nums.size(); ++i) {
        double d = nums[i];
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
  float lanes[8];
  _mm256_storeu_ps(lanes, s);
  float sum = ((lanes[0] + lanes[1]) + (lanes[2] + lanes[3])) +
              ((lanes[4] + lanes[5]) + (lanes[6] + lanes[7]));
  for (; i < dim; ++i) {
    float d = a[i] - b[i];
    sum += d * d;
  }
  return sum;
}
#endif

}  // namespace

double Engine::L2Squared(const float* a, const float* b, int dim) const {
#if defined(__x86_64__)
  static const bool avx2 = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
  if (avx2) return static_cast<double>(L2SqAvx2(a, b, dim));
#endif
  double sum = 0.0;
  for (int i = 0; i < dim; ++i) {
    double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    sum += d * d;
  }
  return sum;
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
  dirty_since_compact_ = true;
  if (wal_size_ > kCompactWalBytes) Compact();
  return true;
}

void Engine::DropOverlay() { overlay_.clear(); }

std::string Engine::Execute(const std::string& sql) {
  if (!ok_) return JsonError(init_error_.empty() ? "io_error" : init_error_);
  Statement st = Parse(sql);
  if (!st.error.empty()) return JsonError(st.error);
  switch (st.kind) {
    case StmtKind::Empty: return JsonOk();
    case StmtKind::CreateTable: return ExecCreateTable(st);
    case StmtKind::CreateVectorIndex: return ExecCreateIndex(st);
    case StmtKind::Insert: return ExecInsert(st);
    case StmtKind::Select: return ExecSelect(st);
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
  std::vector<int64_t> seen;
  for (const auto& row : st.rows) {
    if (row.size() != targets.size()) return JsonError("column_count_mismatch");
    std::vector<ColumnValue> vals(s.cols.size());
    std::vector<bool> present(s.cols.size(), false);
    for (size_t i = 0; i < targets.size(); ++i) {
      std::string err;
      if (!BindLiteral(s.cols[targets[i]], row[i], &vals[targets[i]], err))
        return JsonError(err);
      present[targets[i]] = true;
    }
    for (size_t c = 0; c < s.cols.size(); ++c) {
      if (!present[c]) return JsonError("missing_column_value");
    }
    int64_t pk = vals[s.pk].i;
    bool dup = std::find(seen.begin(), seen.end(), pk) != seen.end();
    if (!dup) {
      if (ov != nullptr) {
        auto it = ov->find(pk);
        if (it != ov->end() && !it->second.del) dup = true;
      }
      if (!dup && t->rows.count(pk)) dup = true;
    }
    if (dup) return JsonError("duplicate_key");
    seen.push_back(pk);
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

namespace {
struct KeyVal {
  uint8_t kind = 0;  // 0 int, 1 str, 2 float
  int64_t i = 0;
  double d = 0;
  std::string s;
};
}  // namespace

std::string Engine::ExecSelect(const Statement& st) {
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

  auto key_of_order = [&](const OrderBound& ob, const RowView& v, double dist) -> KeyVal {
    KeyVal k;
    if (ob.dist_expr) {
      const uint8_t* p = nullptr;
      uint32_t len = 0;
      if (!ColAt(v, s, ob.dcol, &p, &len)) {
        k.kind = 2;
        k.d = 0;
        return k;
      }
      const float* a = reinterpret_cast<const float*>(p);
      double sum = 0.0;
      for (size_t i = 0; i < ob.dqvec.size(); ++i) {
        double d = static_cast<double>(a[i]) - static_cast<double>(ob.dqvec[i]);
        sum += d * d;
      }
      k.kind = 2;
      k.d = sum;
      return k;
    }
    if (ob.item >= 0) {
      const Item& it = items[ob.item];
      if (it.kind == 2) {
        k.kind = 2;
        k.d = dist;
      } else {
        const uint8_t* p = nullptr;
        uint32_t len = 0;
        ColAt(v, s, it.col, &p, &len);
        if (s.cols[it.col].type.kind == ColumnType::BIGINT) {
          k.kind = 0;
          k.i = GetI64(p);
        } else {
          k.kind = 1;
          k.s.assign(reinterpret_cast<const char*>(p), len);
        }
      }
    } else {
      const uint8_t* p = nullptr;
      uint32_t len = 0;
      ColAt(v, s, ob.col, &p, &len);
      if (s.cols[ob.col].type.kind == ColumnType::BIGINT) {
        k.kind = 0;
        k.i = GetI64(p);
      } else {
        k.kind = 1;
        k.s.assign(reinterpret_cast<const char*>(p), len);
      }
    }
    return k;
  };

  struct RowRes {
    int64_t pk = 0;
    double dist = 0;
    RowView view;
    std::vector<KeyVal> keys;
    std::vector<OutVal> vals;
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

  auto build_vals = [&](const RowView& v, double dist) {
    std::vector<OutVal> vals;
    for (const auto& it : items) {
      if (it.kind == 0) {
        for (size_t c = 0; c < s.cols.size(); ++c) {
          Item tmp;
          tmp.kind = 1;
          tmp.col = static_cast<int>(c);
          vals.push_back(value_of_item(tmp, v, dist));
        }
      } else {
        vals.push_back(value_of_item(it, v, dist));
      }
    }
    return vals;
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

  std::vector<RowRes> results;
  auto pk_tie = [](const RowRes& a, const RowRes& b) { return a.pk < b.pk; };

  auto make_res = [&](int64_t pk, const RowView& v, double dist) {
    RowRes r;
    r.pk = pk;
    r.dist = dist;
    r.view = v;
    for (const auto& ob : orders) r.keys.push_back(key_of_order(ob, v, dist));
    r.vals = build_vals(v, dist);
    return r;
  };

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

  if (distance_order) {
    size_t k = static_cast<size_t>(st.limit);
    // Keep a margin of candidates selected with the fast float32 distance, then
    // re-rank them with the exact double distance and cut down to K. This keeps
    // exact mode exact at the K boundary without paying double cost per row.
    size_t k_candidates = k == 0 ? 0 : k + std::max<size_t>(k * 3, 64);
    // priority_queue keeps the "largest" element on top; with this ordering the
    // largest is the worst candidate (largest distance, then largest primary key).
    auto worse_first = [](const RowRes& a, const RowRes& b) {
      if (a.dist != b.dist) return a.dist < b.dist;
      return a.pk < b.pk;
    };
    std::priority_queue<RowRes, std::vector<RowRes>, decltype(worse_first)> heap(worse_first);
    const uint32_t vec_off = s.fixed_off[dist_col];
    const float* qptr = query_vec.data();
    const int qdim = static_cast<int>(query_vec.size());
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
          }
        } else {
          t->cache_int.assign(n, 0);
          for (size_t i = 0; i < n; ++i) {
            RowView v = MakeView(t->scan[i].second);
            v.s = &s;
            const uint8_t* p = nullptr;
            uint32_t l = 0;
            if (ColAt(v, s, cb.col, &p, &l)) t->cache_int[i] = GetI64(p);
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
        for (size_t i = 0; i < t->scan.size(); ++i) {
          bool match = is_str ? (t->cache_ids[i] == want_id) : (t->cache_int[i] == cb.ival);
          if (!match) continue;
          int64_t pk = t->scan[i].first;
          RowView v = MakeView(t->scan[i].second);
          v.s = &s;
          if (!v.Valid()) continue;
          double dist = L2Squared(reinterpret_cast<const float*>(v.p + vec_off), qptr, qdim);
          if (heap.size() < k_candidates) {
            heap.push(make_res(pk, v, dist));
          } else {
            const RowRes& top = heap.top();
            if (dist < top.dist || (dist == top.dist && pk < top.pk)) {
              RowRes r = make_res(pk, v, dist);
              heap.pop();
              heap.push(std::move(r));
            }
          }
        }
      }
    } else if (k_candidates > 0) {
      ForEachRow(*t, [&](int64_t pk, const RowView& v) {
        if (!EvalPredicates(conds, v)) return;
        double dist = L2Squared(reinterpret_cast<const float*>(v.p + vec_off), qptr, qdim);
        if (heap.size() < k_candidates) {
          heap.push(make_res(pk, v, dist));
        } else {
          const RowRes& top = heap.top();
          if (dist < top.dist || (dist == top.dist && pk < top.pk)) {
            RowRes r = make_res(pk, v, dist);
            heap.pop();
            heap.push(std::move(r));
          }
        }
      });
    }
    while (!heap.empty()) {
      results.push_back(heap.top());
      heap.pop();
    }
    for (auto& r : results) {
      r.dist = exact_distance(r.view);
      r.vals = build_vals(r.view, r.dist);
    }
    std::sort(results.begin(), results.end(), [](const RowRes& a, const RowRes& b) {
      if (a.dist != b.dist) return a.dist < b.dist;
      return a.pk < b.pk;
    });
    if (results.size() > k) results.resize(k);
  } else {
    bool limit_stop = orders.empty() && st.limit >= 0;
    ForEachRow(*t, [&](int64_t pk, const RowView& v) {
      if (limit_stop && static_cast<int64_t>(results.size()) >= st.limit) return;
      if (!EvalPredicates(conds, v)) return;
      double dist = need_dist ? compute_dist(v, dist_col, query_vec) : 0.0;
      results.push_back(make_res(pk, v, dist));
    });
    if (!orders.empty()) {
      std::sort(results.begin(), results.end(), [&](const RowRes& a, const RowRes& b) {
        size_t n = orders.size();
        for (size_t i = 0; i < n; ++i) {
          const KeyVal& ka = a.keys[i];
          const KeyVal& kb = b.keys[i];
          int cmp;
          if (ka.kind == 0) cmp = (ka.i < kb.i) ? -1 : (ka.i > kb.i ? 1 : 0);
          else if (ka.kind == 1) cmp = ka.s.compare(kb.s);
          else cmp = (ka.d < kb.d) ? -1 : (ka.d > kb.d ? 1 : 0);
          if (cmp != 0) return orders[i].desc ? cmp > 0 : cmp < 0;
        }
        return pk_tie(a, b);
      });
    }
    if (st.limit >= 0 && static_cast<int64_t>(results.size()) > st.limit) {
      results.resize(static_cast<size_t>(st.limit));
    }
  }

  std::vector<std::vector<OutVal>> rows;
  rows.reserve(results.size());
  for (auto& r : results) rows.push_back(std::move(r.vals));
  return JsonRows(out_columns(), rows);
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
    sets.push_back(std::move(sb));
  }
  std::vector<CondBound> conds;
  std::string err;
  if (!BuildPredicates(s, st.where, &conds, err)) return JsonError(err);

  std::vector<std::pair<int64_t, std::string>> changed;
  ForEachRow(*t, [&](int64_t pk, const RowView& v) {
    if (!EvalPredicates(conds, v)) return;
    std::vector<ColumnValue> vals = DecodeRow(s, v);
    for (const auto& sb : sets) {
      if (s.cols[sb.col].type.kind == ColumnType::VECTOR) {
        vals[sb.col].v = sb.val.v;
      } else if (s.cols[sb.col].type.kind == ColumnType::VARCHAR) {
        vals[sb.col].s = sb.val.s;
      } else {
        vals[sb.col].i = sb.val.i;
      }
    }
    changed.emplace_back(pk, EncodeRow(s, vals));
  });
  for (auto& [pk, bytes] : changed) {
    overlay_[s.id][pk] = OverlayRow{false, std::move(bytes)};
  }
  if (!txn_active_) {
    if (!CommitOverlay()) return JsonError("io_error");
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
  std::vector<int64_t> deleted;
  ForEachRow(*t, [&](int64_t pk, const RowView& v) {
    if (EvalPredicates(conds, v)) deleted.push_back(pk);
  });
  for (int64_t pk : deleted) {
    OverlayRow row;
    row.del = true;
    overlay_[s.id][pk] = std::move(row);
  }
  if (!txn_active_) {
    if (!CommitOverlay()) return JsonError("io_error");
  }
  return JsonAffected(static_cast<int64_t>(deleted.size()));
}

bool Engine::Compact() {
  std::string tmp = dir_ + "/snapshot.tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return false;
  std::map<uint32_t, std::map<int64_t, RowRef>> new_refs;
  std::string buf;
  buf.reserve(4u << 20);
  uint64_t off = 0;
  auto flush = [&]() {
    if (!buf.empty()) {
      if (!WriteAll(fd, buf.data(), buf.size())) return false;
      buf.clear();
    }
    return true;
  };
  auto emit = [&](uint8_t type, uint32_t txn, const std::string& payload) {
    uint8_t hdr[kRecHeader];
    uint32_t len = static_cast<uint32_t>(payload.size());
    uint32_t crc = Crc32(payload.data(), payload.size());
    std::memcpy(hdr, &kMagic, 4);
    std::memcpy(hdr + 4, &len, 4);
    std::memcpy(hdr + 8, &crc, 4);
    hdr[12] = type;
    std::memcpy(hdr + 13, &txn, 4);
    buf.append(reinterpret_cast<char*>(hdr), kRecHeader);
    buf += payload;
    off += kRecHeader + payload.size();
  };
  bool ok = true;
  for (auto& [tid, ts] : tables_) {
    emit(REC_SCHEMA, 0, SerializeSchema(ts.schema));
    for (auto& [pk, ref] : ts.rows) {
      uint32_t len = 0;
      const uint8_t* payload = RowPayload(ref, &len);
      if (payload == nullptr) continue;
      std::string rec;
      PutU32(rec, tid);
      PutI64(rec, pk);
      rec.append(reinterpret_cast<const char*>(payload), len);
      uint64_t this_off = off;
      emit(REC_PUT, 0, rec);
      new_refs[tid][pk] = RowRef{this_off, 0};
      if (buf.size() > (4u << 20)) {
        if (!flush()) { ok = false; break; }
      }
    }
    if (!ok) break;
  }
  if (ok) ok = flush();
  if (ok) ok = ::fsync(fd) == 0;
  ::close(fd);
  if (ok) {
    std::string final_path = dir_ + "/snapshot.dat";
    ok = ::rename(tmp.c_str(), final_path.c_str()) == 0;
    if (ok) ok = FsyncDir(dir_);
  }
  if (!ok) {
    ::unlink(tmp.c_str());
    return false;
  }
  if (!OpenSnapshot()) return false;
  for (auto& [tid, ts] : tables_) {
    auto it = new_refs.find(tid);
    if (it != new_refs.end()) {
      ts.rows = std::move(it->second);
      ts.scan_dirty = true;
    }
  }
  // Reset the WAL to a small empty file so a compacted database does not keep
  // the peak-sized mapping (and preallocated file) around.
  if (wal_base_ != nullptr) {
    ::munmap(wal_base_, wal_cap_);
    wal_base_ = nullptr;
    wal_cap_ = 0;
  }
  if (::ftruncate(wal_fd_, 0) != 0) return false;
  size_t cap = kWalGrowBytes;
  if (::ftruncate(wal_fd_, static_cast<off_t>(cap)) != 0) return false;
  void* fresh = ::mmap(nullptr, cap, PROT_READ | PROT_WRITE, MAP_SHARED, wal_fd_, 0);
  if (fresh == MAP_FAILED) return false;
  wal_base_ = static_cast<uint8_t*>(fresh);
  wal_cap_ = cap;
  wal_size_ = 0;
  dirty_since_compact_ = false;
  return true;
}

void Engine::Shutdown() {
  overlay_.clear();
  txn_active_ = false;
  if (wal_size_ > kShutdownCompactBytes) {
    Compact();
  }
  if (wal_fd_ >= 0) ::fdatasync(wal_fd_);
  CloseAll();
}

}  // namespace vdb
