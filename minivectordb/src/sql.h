#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vdb {

enum class StmtKind {
  Empty,
  CreateTable,
  CreateVectorIndex,
  Insert,
  Select,
  Update,
  Delete,
  Begin,
  Commit,
  Rollback,
};

enum class LitKind { Int, Str, Vec };

struct Literal {
  LitKind kind = LitKind::Int;
  int64_t i = 0;
  std::string s;             // Str payload (raw UTF-8 bytes)
  std::vector<double> nums;  // Vec numeric payload
};

struct ColumnType {
  enum Kind { BIGINT, VARCHAR, VECTOR } kind = BIGINT;
  uint32_t param = 0;  // VARCHAR max code points / VECTOR dimension
};

struct ColumnDef {
  std::string name;
  ColumnType type;
  bool primary = false;
};

enum class CmpOp { EQ, LT, LE, GT, GE };

struct Condition {
  std::string col;
  CmpOp op = CmpOp::EQ;
  Literal lit;
};

struct SelectItem {
  bool star = false;
  bool is_distance = false;
  std::string col;
  Literal qvec;
  std::string alias;
};

struct OrderItem {
  std::string name;   // column name or select alias
  bool desc = false;
  bool is_distance = false;  // ORDER BY L2_DISTANCE(col, <vector>)
  std::string col;
  Literal qvec;
};

struct Statement {
  StmtKind kind = StmtKind::Empty;
  std::string error;  // non-empty when the statement is rejected

  std::string table;
  bool if_not_exists = false;
  std::vector<ColumnDef> columns;

  std::string index_name;
  std::string index_col;

  std::vector<std::string> insert_cols;
  std::vector<std::vector<Literal>> rows;

  std::vector<SelectItem> select_items;
  std::vector<Condition> where;
  std::vector<OrderItem> order;
  int64_t limit = -1;

  std::vector<std::pair<std::string, Literal>> sets;
};

Statement Parse(const std::string& sql);

}  // namespace vdb
