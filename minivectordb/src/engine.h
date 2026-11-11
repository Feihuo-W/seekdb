#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "sql.h"

namespace vdb {

struct Column {
  std::string name;   // as declared
  std::string lname;  // lowercase for resolution
  ColumnType type;
};

struct Schema {
  uint32_t id = 0;
  std::string name;  // lowercase
  std::vector<Column> cols;
  int pk = -1;
  int vec = -1;
  bool has_vec_index = false;

  // Runtime row layout: fixed-width columns (BIGINT/VECTOR) first, then one u32
  // byte-length per VARCHAR, then the VARCHAR bytes. Fixed columns therefore have
  // a constant offset independent of string lengths, which keeps the scan hot path
  // free of per-row column walking.
  std::vector<uint32_t> fixed_off;   // UINT32_MAX for VARCHAR columns
  std::vector<uint32_t> varlen_off;  // offset of the u32 length (VARCHAR only)
  uint32_t fixed_size = 0;
  int nvarchar = 0;

  int FindCol(const std::string& lname) const;
  void ComputeLayout();
  uint32_t FixedWidth(int col) const;
};

class Engine {
 public:
  explicit Engine(std::string dir);
  ~Engine();

  bool Ok() const { return ok_; }
  const std::string& InitError() const { return init_error_; }

  // Executes a single SQL statement. Non-query responses are returned as one
  // compact JSON object; query results are streamed through `sink` (without a
  // trailing newline) and an empty string is returned, so results larger than
  // RAM never need to be materialized.
  using OutSink = std::function<void(const char*, size_t)>;
  std::string Execute(const std::string& sql, const OutSink& sink);

  void SetAnnMode(bool on) { ann_ = on; }
  void Shutdown();

  struct RowView {
    const Schema* s = nullptr;
    const uint8_t* p = nullptr;
    size_t n = 0;
    bool Valid() const;
  };

 private:
  struct RowRef {
    uint64_t off = 0;
    uint8_t file = 0;  // 0 = snapshot, 1 = wal
  };
  struct TableState {
    Schema schema;
    std::map<int64_t, RowRef> rows;
    // Flat pk-ordered view of `rows` for sequential scans; rebuilt lazily after
    // mutations so the hot scan path avoids red-black tree pointer chasing.
    mutable std::vector<std::pair<int64_t, RowRef>> scan;
    mutable bool scan_dirty = true;
    // Lazily built scalar column cache aligned with `scan`, used to evaluate
    // equality filters without touching row pages. VARCHAR values are interned
    // to uint32 ids so the per-query filter loop compares integers. Reset
    // whenever `scan` is rebuilt; only populated for reasonably sized tables.
    mutable int cache_col = -1;
    mutable std::vector<uint32_t> cache_ids;
    mutable std::unordered_map<std::string, uint32_t> cache_dict;
    mutable std::vector<int64_t> cache_int;
  };
  struct OverlayRow {
    bool del = false;
    std::string bytes;  // encoded full row when !del
  };

  struct CondBound {
    int col = 0;
    CmpOp op = CmpOp::EQ;
    bool is_str = false;
    int64_t ival = 0;
    std::string sval;
  };

  struct ColumnValue {
    int64_t i = 0;
    std::string s;
    std::vector<float> v;
  };

  // storage
  bool Init();
  bool Recover();
  bool LoadSnapshot();
  void ReplayWal();
  bool OpenSnapshot();
  bool OpenWal();
  bool EnsureDir();
  bool AppendRecord(uint8_t type, uint32_t txn, const std::string& payload, uint64_t* off);
  bool SyncWal();
  bool Compact();
  void CloseAll();

  // schema helpers
  TableState* FindTable(const std::string& lname);
  int ResolveTableId(const std::string& lname) const;
  std::string SerializeSchema(const Schema& s) const;
  bool DeserializeSchema(const uint8_t* p, size_t n, Schema* out, size_t* used) const;
  void RegisterSchema(const Schema& s);

  // row helpers
  std::string EncodeRow(const Schema& s, const std::vector<ColumnValue>& vals) const;
  RowView MakeView(const RowRef& ref) const;
  std::vector<ColumnValue> DecodeRow(const Schema& s, const RowView& v) const;
  int64_t PkOf(const Schema& s, const RowView& v) const;
  const uint8_t* RowPayload(const RowRef& ref, uint32_t* len) const;

  // binding
  bool BindLiteral(const Column& col, const Literal& lit, ColumnValue* out,
                   std::string& err) const;
  bool BindQueryVector(const Column& col, const Literal& lit, std::vector<float>* out,
                       std::string& err) const;

  // iteration
  template <typename F>
  void ForEachRow(const TableState& t, F&& fn) const;
  void EnsureScan(const TableState& t) const;

  // statements
  std::string ExecCreateTable(const Statement& st);
  std::string ExecCreateIndex(const Statement& st);
  std::string ExecInsert(const Statement& st);
  std::string ExecSelect(const Statement& st, const OutSink& sink);
  std::string ExecUpdate(const Statement& st);
  std::string ExecDelete(const Statement& st);
  std::string ExecBegin();
  std::string ExecCommit();
  std::string ExecRollback();

  bool CommitOverlay();
  void DropOverlay();
  bool BuildPredicates(const Schema& s, const std::vector<Condition>& conds,
                       std::vector<CondBound>* out, std::string& err) const;
  bool EvalPredicates(const std::vector<CondBound>& conds, const RowView& row) const;
  double L2Squared(const float* a, const float* b, int dim) const;

  std::string dir_;
  bool ok_ = true;
  std::string init_error_;

  int snap_fd_ = -1;
  uint8_t* snap_base_ = nullptr;
  size_t snap_size_ = 0;

  int wal_fd_ = -1;
  uint8_t* wal_base_ = nullptr;
  size_t wal_size_ = 0;
  size_t wal_cap_ = 0;

  std::map<std::string, uint32_t> table_ids_;
  std::map<uint32_t, TableState> tables_;
  uint32_t next_table_id_ = 1;
  uint32_t next_txn_ = 1;

  bool txn_active_ = false;
  std::map<uint32_t, std::map<int64_t, OverlayRow>> overlay_;

  bool ann_ = false;
  bool dirty_since_compact_ = false;
};

}  // namespace vdb
