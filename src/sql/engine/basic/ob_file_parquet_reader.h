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
 * Parquet 读取器内部接口：解析 footer schema，并按 row group 流式解码数据页。
 */

#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "sql/engine/basic/ob_file_scan_utils.h"

namespace oceanbase
{
namespace sql
{

class ParquetFileReader
{
public:
  // 读取 footer 中的 Schema，不做数据解码。
  static int read_schema(const std::string &path,
                         std::vector<FileColumnMeta> &columns,
                         std::string &err_msg);
};

class ParquetBatchReader
{
public:
  ParquetBatchReader();
  ~ParquetBatchReader();

  int open(const std::string &path,
           const std::vector<FileColumnMeta> &schema,
           const std::vector<int64_t> &projection,
           const int64_t batch_rows);

  int next_batch(const int64_t batch_rows,
                 std::vector<FileScanRow> &rows,
                 bool &eof,
                 std::string &err_msg);

  int reset();
  void close();

private:
  struct Impl;
  Impl *impl_;

  ParquetBatchReader(const ParquetBatchReader &) = delete;
  ParquetBatchReader &operator=(const ParquetBatchReader &) = delete;
};

}  // namespace sql
}  // namespace oceanbase
