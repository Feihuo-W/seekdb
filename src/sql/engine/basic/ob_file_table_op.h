/*
 * Copyright (c) 2026 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * 文件即数据库的物理算子：把 read_csv/read_json/read_parquet 解析成数据行。
 */

#pragma once

#include <vector>

#include "sql/engine/basic/ob_file_scan_utils.h"
#include "sql/engine/ob_operator.h"

namespace oceanbase
{
namespace sql
{

class ObFileTableSpec : public ObOpSpec
{
OB_UNIS_VERSION_V(1);
public:
  ObFileTableSpec(common::ObIAllocator &alloc, const ObPhyOperatorType type)
      : ObOpSpec(alloc, type),
        path_(),
        format_(0),
        header_(true),
        sample_size_(-1),
        file_size_(-1),
        file_mtime_(0),
        column_exprs_(alloc),
        column_indexes_(alloc)
  {
  }
  virtual ~ObFileTableSpec() {}

  // 文件描述
  common::ObString path_;
  int64_t format_;
  bool header_;
  int64_t sample_size_;
  int64_t file_size_;
  int64_t file_mtime_;
  // 需要输出的列表达式，及每个表达式对应的文件列下标
  common::ObFixedArray<ObExpr *, common::ObIAllocator> column_exprs_;
  common::ObFixedArray<int64_t, common::ObIAllocator> column_indexes_;
};

class ObFileTableOp : public ObOperator
{
public:
  ObFileTableOp(ObExecContext &exec_ctx, const ObOpSpec &spec, ObOpInput *input)
      : ObOperator(exec_ctx, spec, input),
        scanner_(),
        rows_(),
        glob_files_(),
        glob_pos_(0),
        row_idx_(0),
        eof_(false),
        opened_(false),
        is_glob_(false)
  {
  }
  virtual ~ObFileTableOp() {}

  virtual int inner_open() override;
  virtual int inner_rescan() override;
  virtual int inner_get_next_row() override;
  virtual int inner_close() override;
  virtual void destroy() override;

private:
  int build_scan_options(FileScanOptions &options) const;
  int fetch_next_batch();
  int set_column_datum(ObExpr &expr, const std::string &value);

private:
  FileBatchScanner scanner_;
  std::vector<FileScanRow> rows_;
  std::vector<std::string> glob_files_;
  int64_t glob_pos_;
  int64_t row_idx_;
  bool eof_;
  bool opened_;
  bool is_glob_;
};

}  // namespace sql
}  // namespace oceanbase
