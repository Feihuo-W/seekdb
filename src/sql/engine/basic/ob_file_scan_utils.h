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
 * 文件即数据库：本地 CSV / JSONL / Parquet 文件的流式读取与 Schema 推断。
 *
 * 本模块只依赖 C++ 标准库与 ob_errno，方便单独编译与单测；
 * SQL 层负责把扫描出的字符串按列类型转换为 ObObj。
 */

#pragma once

#include <stdint.h>

#include <string>
#include <vector>

namespace oceanbase
{
namespace sql
{

// 文件列类型，与 SQL 层对外暴露的类型一一对应。
enum class FileValueType : int32_t
{
  INVALID = 0,
  BIGINT,    // 整数
  DOUBLE,    // 浮点
  BOOLEAN,   // true / false
  DATE,      // YYYY-MM-DD
  DATETIME,  // YYYY-MM-DD HH:MM:SS
  VARCHAR    // 其余按文本
};

const char *file_value_type_name(const FileValueType type);

// 支持的文件格式。
enum class FileFormatType : int32_t
{
  CSV = 0,
  JSONL = 1,
  PARQUET = 2
};

// 把格式名（csv/json/newline_delimited/ndjson/parquet）转换成枚举，非法返回 INVALID。
FileFormatType file_format_from_name(const std::string &name);

struct FileColumnMeta
{
  std::string name_;
  FileValueType type_ = FileValueType::VARCHAR;
  bool nullable_ = true;
  int64_t max_length_ = 0;  // 非空值的最大字节长度，用于设置 VARCHAR 精度
};

struct FileScanOptions
{
  FileFormatType format_ = FileFormatType::CSV;
  bool header_ = true;          // CSV 首行是否为表头
  int64_t sample_size_ = -1;    // 仅支持 -1：全量推断
  char field_delimiter_ = ',';  // CSV 字段分隔符
  std::string null_str_;        // 文本形式的 NULL，空串表示空字段即 NULL
};

struct FileStat
{
  int64_t size_ = -1;
  int64_t mtime_sec_ = 0;
};

// 一行数据：values_ 与 nulls_ 等长。
struct FileScanRow
{
  std::vector<std::string> values_;
  std::vector<bool> nulls_;
};

class FileScanUtils
{
public:
  // 获取文件大小与修改时间，失败返回 OB_FILE_NOT_EXIST / OB_IO_ERROR。
  static int get_file_stat(const std::string &path, FileStat &stat);

  // 全量推断 Schema。CSV/JSONL 需要完整扫描文件，Parquet 直接读取 footer。
  // 返回错误码，诊断信息写入 err_msg。
  static int infer_schema(const std::string &path,
                          const FileScanOptions &options,
                          std::vector<FileColumnMeta> &columns,
                          std::string &err_msg);

  // glob：仅文件，返回按名字升序排列的路径；支持 "*"、"?" 与 "**"。
  static int glob(const std::string &pattern,
                  std::vector<std::string> &files,
                  std::string &err_msg);
};

// 流式批量扫描器：一次只保留一个批次的解析结果，不加载全量数据。
class FileBatchScanner
{
public:
  FileBatchScanner();
  ~FileBatchScanner();

  // schema 为推断/解析阶段得到的列信息；projection 为需要读取的列下标（按输出顺序）。
  int open(const std::string &path,
           const FileScanOptions &options,
           const std::vector<FileColumnMeta> &schema,
           const std::vector<int64_t> &projection,
           const int64_t batch_rows);

  // 读取下一批数据。eof 为 true 表示已到文件末尾（rows 仍可能非空）。
  int next_batch(std::vector<FileScanRow> &rows, bool &eof, std::string &err_msg);

  // 重扫：回到文件开头。
  int reset();

  void close();

private:
  struct Impl;
  Impl *impl_;

  FileBatchScanner(const FileBatchScanner &) = delete;
  FileBatchScanner &operator=(const FileBatchScanner &) = delete;
};

// 供测试与 SQL 层复用的纯函数。
FileValueType classify_text_value(const std::string &value);
FileValueType merge_file_type(const FileValueType left, const FileValueType right);

// 判断字面量是否为合法的日期 / 时间，用于类型推断的边界校验。
bool is_valid_date_text(const std::string &value);
bool is_valid_datetime_text(const std::string &value);

}  // namespace sql
}  // namespace oceanbase
