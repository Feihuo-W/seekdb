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
 */

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/ob_file_table_op.h"

#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <string>
#include <vector>

#include "sql/engine/ob_exec_context.h"

namespace oceanbase
{
using namespace common;
namespace sql
{

OB_SERIALIZE_MEMBER((ObFileTableSpec, ObOpSpec), path_, format_, header_, sample_size_,
                    file_size_, file_mtime_, column_exprs_, column_indexes_);

namespace
{

// 计算 "YYYY-MM-DD" 对应的 OceanBase date 编码（YYYYMMDD）。
int32_t text_to_ob_date(const std::string &text)
{
  int32_t value = 0;
  if (text.size() >= 10 && '-' == text[4] && '-' == text[7]) {
    const int32_t year = (text[0] - '0') * 1000 + (text[1] - '0') * 100 +
                         (text[2] - '0') * 10 + (text[3] - '0');
    const int32_t month = (text[5] - '0') * 10 + (text[6] - '0');
    const int32_t day = (text[8] - '0') * 10 + (text[9] - '0');
    value = year * 10000 + month * 100 + day;
  }
  return value;
}

int64_t days_from_civil(int64_t year, const int64_t month, const int64_t day)
{
  year -= (month <= 2) ? 1 : 0;
  const int64_t era = (year >= 0 ? year : year - 399) / 400;
  const int64_t yoe = year - era * 400;
  const int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

// 计算 "YYYY-MM-DD HH:MM:SS" 对应的微秒时间戳。
int64_t text_to_ob_datetime(const std::string &text)
{
  int64_t result = 0;
  if (text.size() >= 19) {
    const int64_t year = (text[0] - '0') * 1000 + (text[1] - '0') * 100 +
                         (text[2] - '0') * 10 + (text[3] - '0');
    const int64_t month = (text[5] - '0') * 10 + (text[6] - '0');
    const int64_t day = (text[8] - '0') * 10 + (text[9] - '0');
    const int64_t hour = (text[11] - '0') * 10 + (text[12] - '0');
    const int64_t minute = (text[14] - '0') * 10 + (text[15] - '0');
    const int64_t second = (text[17] - '0') * 10 + (text[18] - '0');
    const int64_t days = days_from_civil(year, month, day);
    result = ((days * 86400) + hour * 3600 + minute * 60 + second) * 1000000;
  }
  return result;
}

}  // anonymous namespace

int ObFileTableOp::build_scan_options(FileScanOptions &options) const
{
  options.format_ = static_cast<FileFormatType>(MY_SPEC.format_);
  options.header_ = MY_SPEC.header_;
  options.sample_size_ = MY_SPEC.sample_size_;
  return OB_SUCCESS;
}

int ObFileTableOp::inner_open()
{
  int ret = OB_SUCCESS;
  FileScanOptions options;
  const std::string path(MY_SPEC.path_.ptr(), MY_SPEC.path_.length());
  is_glob_ = (3 == MY_SPEC.format_);
  if (OB_FAIL(build_scan_options(options))) {
  } else if (is_glob_) {
    std::string err_msg;
    if (OB_FAIL(FileScanUtils::glob(path, glob_files_, err_msg))) {
      LOG_WARN("failed to list glob files", K(ret), K(err_msg.c_str()));
    } else {
      glob_pos_ = 0;
      row_idx_ = 0;
      eof_ = false;
      opened_ = true;
    }
  } else {
    std::vector<FileColumnMeta> schema;
    std::vector<int64_t> projection;
    const int64_t column_count = MY_SPEC.column_indexes_.count();
    for (int64_t i = 0; i < column_count; ++i) {
      schema.push_back(FileColumnMeta());
      projection.push_back(MY_SPEC.column_indexes_.at(i));
    }
    // 变更检测：解析阶段与执行阶段文件必须一致
    FileStat stat;
    if (OB_FAIL(FileScanUtils::get_file_stat(path, stat))) {
    } else if (MY_SPEC.file_size_ >= 0 &&
               (stat.size_ != MY_SPEC.file_size_ ||
                stat.mtime_sec_ != MY_SPEC.file_mtime_)) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("file changed after schema inference", K(stat.size_), K(MY_SPEC.file_size_),
               K(stat.mtime_sec_), K(MY_SPEC.file_mtime_));
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file was modified after schema inference");
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(scanner_.open(path, options, schema, projection,
                                MY_SPEC.max_batch_size_ > 0 ? MY_SPEC.max_batch_size_ : 1024))) {
      } else {
        opened_ = true;
        row_idx_ = 0;
        eof_ = false;
      }
    }
  }
  return ret;
}

int ObFileTableOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else if (is_glob_) {
    glob_pos_ = 0;
  } else if (opened_ && OB_FAIL(scanner_.reset())) {
  }
  if (OB_SUCC(ret)) {
    rows_.clear();
    row_idx_ = 0;
    eof_ = false;
  }
  return ret;
}

int ObFileTableOp::fetch_next_batch()
{
  int ret = OB_SUCCESS;
  std::string err_msg;
  rows_.clear();
  row_idx_ = 0;
  if (eof_) {
    ret = OB_ITER_END;
  } else if (is_glob_) {
    const int64_t batch = (MY_SPEC.max_batch_size_ > 0) ? MY_SPEC.max_batch_size_ : 1024;
    for (int64_t i = 0; i < batch && glob_pos_ < static_cast<int64_t>(glob_files_.size()); ++i) {
      FileScanRow row;
      row.values_.push_back(glob_files_[static_cast<size_t>(glob_pos_)]);
      row.nulls_.push_back(false);
      rows_.push_back(row);
      ++glob_pos_;
    }
    if (rows_.empty()) {
      ret = OB_ITER_END;
    }
  } else if (OB_FAIL(scanner_.next_batch(rows_, eof_, err_msg))) {
    LOG_WARN("failed to scan file batch", K(ret), K(err_msg.c_str()));
  } else if (rows_.empty() && eof_) {
    ret = OB_ITER_END;
  }
  return ret;
}

int ObFileTableOp::set_column_datum(ObExpr &expr, const std::string &value)
{
  int ret = OB_SUCCESS;
  ObObj obj;
  const ObObjType type = expr.datum_meta_.type_;
  switch (type) {
    case ObTinyIntType:
      obj.set_tinyint(static_cast<int8_t>(
          (0 == strcasecmp(value.c_str(), "true") || value == "1") ? 1 : 0));
      break;
    case ObSmallIntType:
    case ObMediumIntType:
    case ObInt32Type:
      obj.set_int32(static_cast<int32_t>(strtol(value.c_str(), NULL, 10)));
      break;
    case ObIntType:
      obj.set_int(static_cast<int64_t>(strtoll(value.c_str(), NULL, 10)));
      break;
    case ObUInt64Type:
      obj.set_uint64(static_cast<uint64_t>(strtoull(value.c_str(), NULL, 10)));
      break;
    case ObFloatType:
      obj.set_float(static_cast<float>(strtod(value.c_str(), NULL)));
      break;
    case ObDoubleType:
      obj.set_double(strtod(value.c_str(), NULL));
      break;
    case ObNumberType:
    case ObUNumberType:
      obj.set_double(strtod(value.c_str(), NULL));
      break;
    case ObDateType:
      obj.set_date(text_to_ob_date(value));
      break;
    case ObDateTimeType:
    case ObTimestampType:
      obj.set_datetime(text_to_ob_datetime(value));
      break;
    default:
      obj.set_varchar(value.data(), static_cast<int32_t>(value.size()));
      obj.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
      break;
  }
  ObDatum &datum = expr.locate_datum_for_write(eval_ctx_);
  if (OB_FAIL(datum.from_obj(obj, expr.obj_datum_map_))) {
  }
  return ret;
}

int ObFileTableOp::inner_get_next_row()
{
  int ret = OB_SUCCESS;
  clear_evaluated_flag();
  if (OB_FAIL(ctx_.check_status())) {
  } else if (row_idx_ >= static_cast<int64_t>(rows_.size())) {
    if (OB_FAIL(fetch_next_batch())) {
      if (OB_ITER_END != ret) {
        LOG_WARN("failed to fetch next batch", K(ret));
      }
    }
  }
  if (OB_SUCC(ret)) {
    const FileScanRow &row = rows_[static_cast<size_t>(row_idx_)];
    for (int64_t i = 0; OB_SUCC(ret) && i < MY_SPEC.column_exprs_.count(); ++i) {
      ObExpr *expr = MY_SPEC.column_exprs_.at(i);
      if (OB_ISNULL(expr)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
        if (i >= static_cast<int64_t>(row.nulls_.size()) || row.nulls_[static_cast<size_t>(i)]) {
          datum.set_null();
        } else if (OB_FAIL(set_column_datum(*expr, row.values_[static_cast<size_t>(i)]))) {
        }
        expr->set_evaluated_projected(eval_ctx_);
      }
    }
    ++row_idx_;
  }
  return ret;
}

int ObFileTableOp::inner_close()
{
  scanner_.close();
  opened_ = false;
  rows_.clear();
  row_idx_ = 0;
  eof_ = false;
  return OB_SUCCESS;
}

void ObFileTableOp::destroy()
{
  inner_close();
  ObOperator::destroy();
}

}  // namespace sql
}  // namespace oceanbase
