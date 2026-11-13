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

#include "sql/engine/basic/ob_file_parquet_reader.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <set>
#include <string>
#include <vector>

#include "lib/ob_errno.h"
#include "lib/utility/ob_macro_utils.h"

namespace oceanbase
{
namespace sql
{

namespace
{

// ---------------------------------------------------------------------------
//  Thrift Compact Protocol
// ---------------------------------------------------------------------------

const int32_t THRIFT_BOOL_TRUE = 1;
const int32_t THRIFT_BOOL_FALSE = 2;
const int32_t THRIFT_BYTE = 3;
const int32_t THRIFT_I16 = 4;
const int32_t THRIFT_I32 = 5;
const int32_t THRIFT_I64 = 6;
const int32_t THRIFT_DOUBLE = 7;
const int32_t THRIFT_BINARY = 8;
const int32_t THRIFT_LIST = 9;
const int32_t THRIFT_SET = 10;
const int32_t THRIFT_MAP = 11;
const int32_t THRIFT_STRUCT = 12;

struct CompactReader
{
  const uint8_t *p_ = NULL;
  const uint8_t *end_ = NULL;
  bool error_ = false;

  uint8_t read_u8()
  {
    uint8_t value = 0;
    if (p_ < end_) {
      value = *p_++;
    } else {
      error_ = true;
    }
    return value;
  }

  uint64_t read_varint()
  {
    uint64_t value = 0;
    int shift = 0;
    bool done = false;
    while (!done && !error_ && shift < 64) {
      const uint8_t byte = read_u8();
      value |= static_cast<uint64_t>(byte & 0x7f) << shift;
      done = (0 == (byte & 0x80));
      shift += 7;
    }
    return value;
  }

  int64_t read_zigzag()
  {
    const uint64_t value = read_varint();
    return static_cast<int64_t>((value >> 1) ^ (~(value & 1) + 1));
  }

  std::string read_binary()
  {
    std::string result;
    const uint64_t len = read_varint();
    if (!error_ && p_ + len <= end_) {
      result.assign(reinterpret_cast<const char *>(p_), static_cast<size_t>(len));
      p_ += len;
    } else {
      error_ = true;
    }
    return result;
  }

  // 返回 false 表示 struct 结束；last_id 由调用方按 struct 作用域维护。
  bool next_field(int64_t &last_id, int32_t &type, int64_t &field_id)
  {
    bool has_field = false;
    const uint8_t byte = read_u8();
    if (!error_) {
      type = byte & 0x0f;
      if (0 == type) {
        has_field = false;
      } else {
        const int32_t delta = (byte >> 4) & 0x0f;
        if (0 == delta) {
          last_id = read_zigzag();
        } else {
          last_id += delta;
        }
        field_id = last_id;
        has_field = true;
      }
    }
    return has_field;
  }

  void skip(const int32_t type)
  {
    switch (type) {
      case THRIFT_BOOL_TRUE:
      case THRIFT_BOOL_FALSE:
        break;
      case THRIFT_BYTE:
        (void)read_u8();
        break;
      case THRIFT_I16:
      case THRIFT_I32:
      case THRIFT_I64:
        (void)read_varint();
        break;
      case THRIFT_DOUBLE: {
        for (int i = 0; i < 8; ++i) {
          (void)read_u8();
        }
        break;
      }
      case THRIFT_BINARY:
        (void)read_binary();
        break;
      case THRIFT_LIST:
      case THRIFT_SET: {
        const uint8_t header = read_u8();
        int32_t size = (header >> 4) & 0x0f;
        const int32_t elem_type = header & 0x0f;
        if (15 == size) {
          size = static_cast<int32_t>(read_varint());
        }
        for (int32_t i = 0; i < size && !error_; ++i) {
          if (THRIFT_STRUCT == elem_type) {
            int64_t last_id = 0;
            int32_t type = 0;
            int64_t field_id = 0;
            while (!error_ && next_field(last_id, type, field_id)) {
              skip(type);
            }
          } else {
            skip(elem_type);
          }
        }
        break;
      }
      case THRIFT_MAP: {
        const int32_t size = static_cast<int32_t>(read_varint());
        if (size > 0) {
          const uint8_t types = read_u8();
          const int32_t key_type = (types >> 4) & 0x0f;
          const int32_t value_type = types & 0x0f;
          for (int32_t i = 0; i < size && !error_; ++i) {
            skip(key_type);
            skip(value_type);
          }
        }
        break;
      }
      case THRIFT_STRUCT: {
        int64_t last_id = 0;
        int32_t type = 0;
        int64_t field_id = 0;
        while (!error_ && next_field(last_id, type, field_id)) {
          skip(type);
        }
        break;
      }
      default:
        error_ = true;
        break;
    }
  }
};

int32_t read_list_header(CompactReader &reader, int32_t &elem_type)
{
  const uint8_t header = reader.read_u8();
  int32_t size = (header >> 4) & 0x0f;
  elem_type = header & 0x0f;
  if (15 == size) {
    size = static_cast<int32_t>(reader.read_varint());
  }
  return size;
}

// ---------------------------------------------------------------------------
//  Parquet 元数据
// ---------------------------------------------------------------------------

const int32_t PQ_BOOLEAN = 0;
const int32_t PQ_INT32 = 1;
const int32_t PQ_INT64 = 2;
const int32_t PQ_INT96 = 3;
const int32_t PQ_FLOAT = 4;
const int32_t PQ_DOUBLE = 5;
const int32_t PQ_BYTE_ARRAY = 6;
const int32_t PQ_FIXED_LEN_BYTE_ARRAY = 7;

const int32_t PQ_CONVERTED_DATE = 6;
const int32_t PQ_CONVERTED_TIMESTAMP_MILLIS = 9;

const int32_t PQ_ENCODING_PLAIN = 0;
const int32_t PQ_ENCODING_PLAIN_DICTIONARY = 2;
const int32_t PQ_ENCODING_RLE = 3;
const int32_t PQ_ENCODING_RLE_DICTIONARY = 8;

const int32_t PQ_PAGE_DATA = 0;
const int32_t PQ_PAGE_DICTIONARY = 2;
const int32_t PQ_PAGE_DATA_V2 = 3;

const int32_t PQ_CODEC_UNCOMPRESSED = 0;
const int32_t PQ_CODEC_SNAPPY = 1;

struct PqSchemaElement
{
  int32_t type_ = -1;  // 物理类型；-1 表示 group
  int32_t type_length_ = 0;
  int32_t repetition_ = 0;  // 0 required / 1 optional / 2 repeated
  std::string name_;
  int32_t num_children_ = 0;
  int32_t converted_type_ = -1;
};

struct PqColumnChunk
{
  int32_t physical_type_ = -1;
  int32_t codec_ = 0;
  int64_t num_values_ = 0;
  int64_t total_compressed_size_ = 0;
  int64_t data_page_offset_ = -1;
  int64_t dictionary_page_offset_ = -1;
};

struct PqRowGroup
{
  int64_t num_rows_ = 0;
  std::vector<PqColumnChunk> columns_;
};

struct PqFileMeta
{
  int64_t num_rows_ = 0;
  std::vector<PqSchemaElement> schema_;
  std::vector<PqRowGroup> row_groups_;
};

int parse_schema_element(CompactReader &reader, PqSchemaElement &element)
{
  int ret = OB_SUCCESS;
  int64_t last_id = 0;
  int32_t type = 0;
  int64_t field_id = 0;
  while (OB_SUCC(ret) && reader.next_field(last_id, type, field_id)) {
    switch (field_id) {
      case 1:
        element.type_ = static_cast<int32_t>(reader.read_zigzag());
        break;
      case 2:
        element.type_length_ = static_cast<int32_t>(reader.read_zigzag());
        break;
      case 3:
        element.repetition_ = static_cast<int32_t>(reader.read_zigzag());
        break;
      case 4:
        element.name_ = reader.read_binary();
        break;
      case 5:
        element.num_children_ = static_cast<int32_t>(reader.read_zigzag());
        break;
      case 6:
        element.converted_type_ = static_cast<int32_t>(reader.read_zigzag());
        break;
      default:
        reader.skip(type);
        break;
    }
    if (reader.error_) {
      ret = OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

int parse_column_metadata(CompactReader &reader, PqColumnChunk &chunk)
{
  int ret = OB_SUCCESS;
  int64_t last_id = 0;
  int32_t type = 0;
  int64_t field_id = 0;
  while (OB_SUCC(ret) && reader.next_field(last_id, type, field_id)) {
    switch (field_id) {
      case 1:
        chunk.physical_type_ = static_cast<int32_t>(reader.read_zigzag());
        break;
      case 4:
        chunk.codec_ = static_cast<int32_t>(reader.read_zigzag());
        break;
      case 5:
        chunk.num_values_ = reader.read_zigzag();
        break;
      case 7:
        chunk.total_compressed_size_ = reader.read_zigzag();
        break;
      case 9:
        chunk.data_page_offset_ = reader.read_zigzag();
        break;
      case 11:
        chunk.dictionary_page_offset_ = reader.read_zigzag();
        break;
      default:
        reader.skip(type);
        break;
    }
    if (reader.error_) {
      ret = OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

int parse_column_chunk(CompactReader &reader, PqColumnChunk &chunk)
{
  int ret = OB_SUCCESS;
  int64_t last_id = 0;
  int32_t type = 0;
  int64_t field_id = 0;
  while (OB_SUCC(ret) && reader.next_field(last_id, type, field_id)) {
    if (3 == field_id && THRIFT_STRUCT == type) {
      ret = parse_column_metadata(reader, chunk);
    } else {
      reader.skip(type);
    }
    if (reader.error_ && OB_SUCC(ret)) {
      ret = OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

int parse_row_group(CompactReader &reader, PqRowGroup &group)
{
  int ret = OB_SUCCESS;
  int64_t last_id = 0;
  int32_t type = 0;
  int64_t field_id = 0;
  while (OB_SUCC(ret) && reader.next_field(last_id, type, field_id)) {
    if (1 == field_id && THRIFT_LIST == type) {
      int32_t elem_type = 0;
      const int32_t count = read_list_header(reader, elem_type);
      for (int32_t i = 0; OB_SUCC(ret) && i < count; ++i) {
        PqColumnChunk chunk;
        ret = parse_column_chunk(reader, chunk);
        if (OB_SUCC(ret)) {
          group.columns_.push_back(chunk);
        }
      }
    } else if (3 == field_id) {
      group.num_rows_ = reader.read_zigzag();
    } else {
      reader.skip(type);
    }
    if (reader.error_ && OB_SUCC(ret)) {
      ret = OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

int parse_file_metadata(const uint8_t *data, const int64_t size, PqFileMeta &meta)
{
  int ret = OB_SUCCESS;
  CompactReader reader;
  reader.p_ = data;
  reader.end_ = data + size;
  int64_t last_id = 0;
  int32_t type = 0;
  int64_t field_id = 0;
  while (OB_SUCC(ret) && reader.next_field(last_id, type, field_id)) {
    if (1 == field_id) {
      (void)reader.read_zigzag();
    } else if (2 == field_id && THRIFT_LIST == type) {
      int32_t elem_type = 0;
      const int32_t count = read_list_header(reader, elem_type);
      for (int32_t i = 0; OB_SUCC(ret) && i < count; ++i) {
        PqSchemaElement element;
        ret = parse_schema_element(reader, element);
        if (OB_SUCC(ret)) {
          meta.schema_.push_back(element);
        }
      }
    } else if (3 == field_id) {
      meta.num_rows_ = reader.read_zigzag();
    } else if (4 == field_id && THRIFT_LIST == type) {
      int32_t elem_type = 0;
      const int32_t count = read_list_header(reader, elem_type);
      for (int32_t i = 0; OB_SUCC(ret) && i < count; ++i) {
        PqRowGroup group;
        ret = parse_row_group(reader, group);
        if (OB_SUCC(ret)) {
          meta.row_groups_.push_back(group);
        }
      }
    } else {
      reader.skip(type);
    }
    if (reader.error_ && OB_SUCC(ret)) {
      ret = OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

// ---------------------------------------------------------------------------
//  文件访问
// ---------------------------------------------------------------------------

struct FileHandle
{
  FILE *fp_ = NULL;
  int64_t size_ = 0;

  int open_file(const std::string &path)
  {
    int ret = OB_SUCCESS;
    close_file();
    fp_ = fopen(path.c_str(), "rb");
    if (NULL == fp_) {
      ret = (ENOENT == errno) ? OB_FILE_NOT_EXIST : OB_IO_ERROR;
    } else if (0 != fseek(fp_, 0, SEEK_END)) {
      ret = OB_IO_ERROR;
    } else {
      size_ = static_cast<int64_t>(ftell(fp_));
    }
    return ret;
  }

  int read_at(const int64_t offset, const int64_t length, std::string &buffer)
  {
    int ret = OB_SUCCESS;
    buffer.clear();
    if (NULL == fp_ || offset < 0 || length < 0 || offset + length > size_) {
      ret = OB_INVALID_ARGUMENT;
    } else if (0 != fseek(fp_, static_cast<long>(offset), SEEK_SET)) {
      ret = OB_IO_ERROR;
    } else {
      buffer.resize(static_cast<size_t>(length));
      if (length > 0 && 1 != fread(&buffer[0], static_cast<size_t>(length), 1, fp_)) {
        ret = OB_IO_ERROR;
      }
    }
    return ret;
  }

  void close_file()
  {
    if (NULL != fp_) {
      fclose(fp_);
      fp_ = NULL;
    }
  }
};

int load_file_metadata(FileHandle &file, PqFileMeta &meta, std::string &err_msg)
{
  int ret = OB_SUCCESS;
  std::string tail;
  if (file.size_ < 12) {
    ret = OB_INVALID_ARGUMENT;
    err_msg = "not a parquet file: file is too small";
  } else if (OB_FAIL(file.read_at(file.size_ - 8, 8, tail))) {
    err_msg = "failed to read parquet footer";
  } else if (0 != memcmp(tail.data() + 4, "PAR1", 4)) {
    ret = OB_INVALID_ARGUMENT;
    err_msg = "not a parquet file: missing magic in footer";
  } else {
    int64_t footer_len = 0;
    for (int i = 0; i < 4; ++i) {
      footer_len |= static_cast<int64_t>(static_cast<uint8_t>(tail[static_cast<size_t>(i)]))
                    << (i * 8);
    }
    std::string footer;
    if (footer_len <= 0 || footer_len > file.size_ - 8) {
      ret = OB_INVALID_ARGUMENT;
      err_msg = "invalid parquet footer length";
    } else if (OB_FAIL(file.read_at(file.size_ - 8 - footer_len, footer_len, footer))) {
      err_msg = "failed to read parquet metadata";
    } else if (OB_FAIL(parse_file_metadata(
                   reinterpret_cast<const uint8_t *>(footer.data()), footer_len, meta))) {
      err_msg = "failed to parse parquet metadata";
    }
  }
  return ret;
}

// ---------------------------------------------------------------------------
//  Snappy 解压（raw block 格式）
// ---------------------------------------------------------------------------

int snappy_decompress(const uint8_t *input,
                      const int64_t input_size,
                      std::string &output,
                      std::string &err_msg)
{
  int ret = OB_SUCCESS;
  output.clear();
  const uint8_t *p = input;
  const uint8_t *end = input + input_size;
  uint64_t expected = 0;
  int shift = 0;
  bool length_done = false;
  while (!length_done && p < end) {
    const uint8_t byte = *p++;
    expected |= static_cast<uint64_t>(byte & 0x7f) << shift;
    if (0 == (byte & 0x80)) {
      length_done = true;
    }
    shift += 7;
  }
  if (!length_done || expected > (static_cast<uint64_t>(1) << 31)) {
    ret = OB_INVALID_ARGUMENT;
    err_msg = "invalid snappy stream";
  } else {
    output.reserve(static_cast<size_t>(expected));
    while (OB_SUCC(ret) && p < end && output.size() < expected) {
      const uint8_t tag = *p++;
      const uint32_t type = tag & 0x03;
      if (0 == type) {
        uint32_t len = (tag >> 2);
        if (len < 60) {
          len += 1;
        } else {
          const uint32_t extra = len - 59;
          if (extra > 4 || p + extra > end) {
            ret = OB_INVALID_ARGUMENT;
            break;
          }
          len = 0;
          for (uint32_t i = 0; i < extra; ++i) {
            len |= static_cast<uint32_t>(p[i]) << (8 * i);
          }
          len += 1;
          p += extra;
        }
        if (p + len > end) {
          ret = OB_INVALID_ARGUMENT;
          break;
        }
        output.append(reinterpret_cast<const char *>(p), len);
        p += len;
      } else {
        uint32_t length = 0;
        uint32_t offset = 0;
        if (1 == type) {
          if (p + 1 > end) {
            ret = OB_INVALID_ARGUMENT;
            break;
          }
          length = ((tag >> 2) & 0x07) + 4;
          offset = ((static_cast<uint32_t>(tag) >> 5) << 8) | static_cast<uint32_t>(p[0]);
          p += 1;
        } else if (2 == type) {
          if (p + 2 > end) {
            ret = OB_INVALID_ARGUMENT;
            break;
          }
          length = (tag >> 2) + 1;
          offset = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8);
          p += 2;
        } else {
          if (p + 4 > end) {
            ret = OB_INVALID_ARGUMENT;
            break;
          }
          length = (tag >> 2) + 1;
          offset = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                   (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
          p += 4;
        }
        if (0 == offset || offset > output.size()) {
          ret = OB_INVALID_ARGUMENT;
          err_msg = "invalid snappy copy offset";
          break;
        }
        const size_t start = output.size() - offset;
        for (uint32_t i = 0; i < length; ++i) {
          output.push_back(output[start + i]);
        }
      }
    }
    if (OB_SUCC(ret) && output.size() != expected) {
      ret = OB_INVALID_ARGUMENT;
      err_msg = "snappy decompressed size mismatch";
    }
  }
  return ret;
}

// ---------------------------------------------------------------------------
//  RLE / Bit-Packed 解码
// ---------------------------------------------------------------------------

uint32_t read_varint32(const uint8_t *&p, const uint8_t *end)
{
  uint32_t value = 0;
  int shift = 0;
  while (p < end && shift < 35) {
    const uint8_t byte = *p++;
    value |= static_cast<uint32_t>(byte & 0x7f) << shift;
    if (0 == (byte & 0x80)) {
      break;
    }
    shift += 7;
  }
  return value;
}

int unpack_bits(const uint8_t *data,
                const int64_t data_len,
                const int32_t bit_width,
                const int64_t count,
                std::vector<uint32_t> &values)
{
  int ret = OB_SUCCESS;
  int64_t bit_pos = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
    uint32_t value = 0;
    for (int32_t bit = 0; bit < bit_width; ++bit) {
      const int64_t bit_index = bit_pos + bit;
      const int64_t byte_index = bit_index >> 3;
      if (byte_index >= data_len) {
        ret = OB_INVALID_ARGUMENT;
        break;
      }
      if (data[byte_index] & (1 << (bit_index & 7))) {
        value |= (1u << bit);
      }
    }
    bit_pos += bit_width;
    if (OB_SUCC(ret)) {
      values.push_back(value);
    }
  }
  return ret;
}

int decode_rle_hybrid(const uint8_t *data,
                      const int64_t data_len,
                      const int32_t bit_width,
                      const int64_t count,
                      std::vector<uint32_t> &values)
{
  int ret = OB_SUCCESS;
  values.clear();
  const uint8_t *p = data;
  const uint8_t *end = data + data_len;
  while (OB_SUCC(ret) && static_cast<int64_t>(values.size()) < count && p < end) {
    const uint32_t header = read_varint32(p, end);
    if (0 == (header & 1)) {
      const int64_t run = header >> 1;
      const int32_t byte_width = (bit_width + 7) / 8;
      if (p + byte_width > end) {
        ret = OB_INVALID_ARGUMENT;
        break;
      }
      uint32_t value = 0;
      for (int32_t i = 0; i < byte_width; ++i) {
        value |= static_cast<uint32_t>(p[i]) << (8 * i);
      }
      p += byte_width;
      for (int64_t i = 0; i < run && static_cast<int64_t>(values.size()) < count; ++i) {
        values.push_back(value);
      }
    } else {
      const int64_t groups = header >> 1;
      const int64_t total = groups * 8;
      const int64_t byte_len = (total * bit_width + 7) / 8;
      if (p + byte_len > end) {
        ret = OB_INVALID_ARGUMENT;
        break;
      }
      std::vector<uint32_t> unpacked;
      if (OB_FAIL(unpack_bits(p, byte_len, bit_width, total, unpacked))) {
      } else {
        for (size_t i = 0;
             i < unpacked.size() && static_cast<int64_t>(values.size()) < count;
             ++i) {
          values.push_back(unpacked[i]);
        }
      }
      p += byte_len;
    }
  }
  return ret;
}

int32_t bit_width_for(const int32_t max_value)
{
  int32_t width = 0;
  while ((1 << width) <= max_value && width < 32) {
    ++width;
  }
  return width;
}

// ---------------------------------------------------------------------------
//  值格式化
// ---------------------------------------------------------------------------

void civil_from_days(const int64_t days, int64_t &year, int64_t &month, int64_t &day)
{
  const int64_t z = days + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t y = yoe + era * 400;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  day = doy - (153 * mp + 2) / 5 + 1;
  month = (mp < 10 ? mp + 3 : mp - 9);
  year = (month <= 2 ? y + 1 : y);
}

std::string format_date(const int64_t days)
{
  int64_t year = 0;
  int64_t month = 0;
  int64_t day = 0;
  civil_from_days(days, year, month, day);
  char buffer[48];
  snprintf(buffer, sizeof(buffer), "%04lld-%02lld-%02lld",
           static_cast<long long>(year), static_cast<long long>(month),
           static_cast<long long>(day));
  return std::string(buffer);
}

std::string format_datetime(const int64_t seconds)
{
  int64_t days = seconds / 86400;
  int64_t remain = seconds % 86400;
  if (remain < 0) {
    remain += 86400;
    --days;
  }
  int64_t year = 0;
  int64_t month = 0;
  int64_t day = 0;
  civil_from_days(days, year, month, day);
  char buffer[64];
  snprintf(buffer, sizeof(buffer), "%04lld-%02lld-%02lld %02lld:%02lld:%02lld",
           static_cast<long long>(year), static_cast<long long>(month),
           static_cast<long long>(day), static_cast<long long>(remain / 3600),
           static_cast<long long>((remain % 3600) / 60),
           static_cast<long long>(remain % 60));
  return std::string(buffer);
}

std::string format_double(const double value)
{
  char buffer[64];
  snprintf(buffer, sizeof(buffer), "%.15g", value);
  if (strtod(buffer, NULL) != value) {
    snprintf(buffer, sizeof(buffer), "%.17g", value);
  }
  return std::string(buffer);
}

int32_t read_int32_le(const uint8_t *data)
{
  return static_cast<int32_t>(static_cast<uint32_t>(data[0]) |
                              (static_cast<uint32_t>(data[1]) << 8) |
                              (static_cast<uint32_t>(data[2]) << 16) |
                              (static_cast<uint32_t>(data[3]) << 24));
}

int64_t read_int64_le(const uint8_t *data)
{
  uint64_t value = 0;
  for (int i = 7; i >= 0; --i) {
    value = (value << 8) | static_cast<uint8_t>(data[i]);
  }
  return static_cast<int64_t>(value);
}

// ---------------------------------------------------------------------------
//  页头
// ---------------------------------------------------------------------------

struct PageHeader
{
  int32_t type_ = -1;
  int32_t uncompressed_size_ = 0;
  int32_t compressed_size_ = 0;
  int32_t num_values_ = 0;
  int32_t encoding_ = PQ_ENCODING_PLAIN;
  int32_t def_levels_byte_len_ = 0;
  int32_t rep_levels_byte_len_ = 0;
  bool is_compressed_ = true;
  int32_t dict_num_values_ = 0;
};

int parse_data_page_header_v1(CompactReader &reader, PageHeader &header)
{
  int ret = OB_SUCCESS;
  int64_t last_id = 0;
  int32_t type = 0;
  int64_t field_id = 0;
  while (OB_SUCC(ret) && reader.next_field(last_id, type, field_id)) {
    if (1 == field_id) {
      header.num_values_ = static_cast<int32_t>(reader.read_zigzag());
    } else if (2 == field_id) {
      header.encoding_ = static_cast<int32_t>(reader.read_zigzag());
    } else {
      reader.skip(type);
    }
    if (reader.error_) {
      ret = OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

int parse_data_page_header_v2(CompactReader &reader, PageHeader &header)
{
  int ret = OB_SUCCESS;
  int64_t last_id = 0;
  int32_t type = 0;
  int64_t field_id = 0;
  while (OB_SUCC(ret) && reader.next_field(last_id, type, field_id)) {
    if (1 == field_id) {
      header.num_values_ = static_cast<int32_t>(reader.read_zigzag());
    } else if (4 == field_id) {
      header.encoding_ = static_cast<int32_t>(reader.read_zigzag());
    } else if (5 == field_id) {
      header.def_levels_byte_len_ = static_cast<int32_t>(reader.read_zigzag());
    } else if (6 == field_id) {
      header.rep_levels_byte_len_ = static_cast<int32_t>(reader.read_zigzag());
    } else if (7 == field_id) {
      header.is_compressed_ = (THRIFT_BOOL_TRUE == type);
    } else {
      reader.skip(type);
    }
    if (reader.error_) {
      ret = OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

int parse_dictionary_page_header(CompactReader &reader, PageHeader &header)
{
  int ret = OB_SUCCESS;
  int64_t last_id = 0;
  int32_t type = 0;
  int64_t field_id = 0;
  while (OB_SUCC(ret) && reader.next_field(last_id, type, field_id)) {
    if (1 == field_id) {
      header.dict_num_values_ = static_cast<int32_t>(reader.read_zigzag());
    } else {
      reader.skip(type);
    }
    if (reader.error_) {
      ret = OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

int parse_page_header(CompactReader &reader, PageHeader &header, int64_t &consumed)
{
  int ret = OB_SUCCESS;
  const uint8_t *begin = reader.p_;
  int64_t last_id = 0;
  int32_t type = 0;
  int64_t field_id = 0;
  while (OB_SUCC(ret) && reader.next_field(last_id, type, field_id)) {
    if (1 == field_id) {
      header.type_ = static_cast<int32_t>(reader.read_zigzag());
    } else if (2 == field_id) {
      header.uncompressed_size_ = static_cast<int32_t>(reader.read_zigzag());
    } else if (3 == field_id) {
      header.compressed_size_ = static_cast<int32_t>(reader.read_zigzag());
    } else if (5 == field_id && THRIFT_STRUCT == type) {
      ret = parse_data_page_header_v1(reader, header);
    } else if (7 == field_id && THRIFT_STRUCT == type) {
      ret = parse_dictionary_page_header(reader, header);
    } else if (8 == field_id && THRIFT_STRUCT == type) {
      ret = parse_data_page_header_v2(reader, header);
    } else {
      reader.skip(type);
    }
    if (reader.error_ && OB_SUCC(ret)) {
      ret = OB_INVALID_ARGUMENT;
    }
  }
  consumed = reader.p_ - begin;
  return ret;
}

// 解码一列在某个 row group 内的全部取值。
int decode_column_chunk(FileHandle &file,
                        const PqColumnChunk &chunk,
                        const PqSchemaElement &leaf,
                        std::vector<std::string> &values,
                        std::vector<bool> &nulls,
                        std::string &err_msg)
{
  int ret = OB_SUCCESS;
  values.clear();
  nulls.clear();
  const int32_t physical_type = leaf.type_;
  const int32_t converted_type = leaf.converted_type_;
  const int32_t max_def_level = (1 == leaf.repetition_) ? 1 : 0;
  if (2 == leaf.repetition_) {
    ret = OB_NOT_SUPPORTED;
    err_msg = "repeated parquet column is not supported";
  } else if (PQ_CODEC_UNCOMPRESSED != chunk.codec_ && PQ_CODEC_SNAPPY != chunk.codec_) {
    ret = OB_NOT_SUPPORTED;
    err_msg = "parquet compression codec is not supported";
  } else {
    int64_t offset = (chunk.dictionary_page_offset_ > 0) ? chunk.dictionary_page_offset_
                                                         : chunk.data_page_offset_;
    int64_t remaining_values = chunk.num_values_;
    std::vector<std::string> dictionary;
    bool done = false;
    while (OB_SUCC(ret) && !done && remaining_values > 0) {
      if (offset <= 0 || offset >= file.size_) {
        ret = OB_INVALID_ARGUMENT;
        err_msg = "invalid parquet page offset";
        break;
      }
      const int64_t header_len =
          std::min(static_cast<int64_t>(1 << 16), file.size_ - offset);
      std::string header_buffer;
      if (OB_FAIL(file.read_at(offset, header_len, header_buffer))) {
        err_msg = "failed to read parquet page header";
        break;
      }
      CompactReader reader;
      reader.p_ = reinterpret_cast<const uint8_t *>(header_buffer.data());
      reader.end_ = reader.p_ + header_buffer.size();
      PageHeader header;
      int64_t consumed = 0;
      if (OB_FAIL(parse_page_header(reader, header, consumed)) || reader.error_) {
        ret = OB_INVALID_ARGUMENT;
        err_msg = "failed to parse parquet page header";
        break;
      }
      const int64_t data_offset = offset + consumed;
      std::string payload;
      if (header.compressed_size_ <= 0 ||
          OB_FAIL(file.read_at(data_offset, header.compressed_size_, payload))) {
        ret = OB_INVALID_ARGUMENT;
        err_msg = "failed to read parquet page payload";
        break;
      }
      offset = data_offset + header.compressed_size_;

      if (PQ_PAGE_DICTIONARY == header.type_) {
        std::string raw;
        if (PQ_CODEC_UNCOMPRESSED == chunk.codec_) {
          raw.swap(payload);
        } else if (OB_FAIL(snappy_decompress(
                       reinterpret_cast<const uint8_t *>(payload.data()), payload.size(),
                       raw, err_msg))) {
          break;
        }
        const uint8_t *p = reinterpret_cast<const uint8_t *>(raw.data());
        const uint8_t *end = p + raw.size();
        for (int32_t i = 0; OB_SUCC(ret) && i < header.dict_num_values_; ++i) {
          std::string value;
          switch (physical_type) {
            case PQ_INT32:
              if (p + 4 > end) {
                ret = OB_INVALID_ARGUMENT;
              } else {
                const int32_t raw_value = read_int32_le(p);
                p += 4;
                value = (PQ_CONVERTED_DATE == converted_type)
                            ? format_date(raw_value)
                            : std::to_string(raw_value);
              }
              break;
            case PQ_INT64:
              if (p + 8 > end) {
                ret = OB_INVALID_ARGUMENT;
              } else {
                const int64_t raw_value = read_int64_le(p);
                p += 8;
                value = (PQ_CONVERTED_TIMESTAMP_MILLIS == converted_type)
                            ? format_datetime(raw_value / 1000)
                            : std::to_string(raw_value);
              }
              break;
            case PQ_FLOAT:
              if (p + 4 > end) {
                ret = OB_INVALID_ARGUMENT;
              } else {
                const int32_t raw_value = read_int32_le(p);
                float float_value = 0;
                memcpy(&float_value, &raw_value, 4);
                p += 4;
                value = format_double(static_cast<double>(float_value));
              }
              break;
            case PQ_DOUBLE:
              if (p + 8 > end) {
                ret = OB_INVALID_ARGUMENT;
              } else {
                const int64_t raw_value = read_int64_le(p);
                double double_value = 0;
                memcpy(&double_value, &raw_value, 8);
                p += 8;
                value = format_double(double_value);
              }
              break;
            case PQ_BYTE_ARRAY:
              if (p + 4 > end) {
                ret = OB_INVALID_ARGUMENT;
              } else {
                const int32_t len = read_int32_le(p);
                p += 4;
                if (len < 0 || p + len > end) {
                  ret = OB_INVALID_ARGUMENT;
                } else {
                  value.assign(reinterpret_cast<const char *>(p), static_cast<size_t>(len));
                  p += len;
                }
              }
              break;
            default:
              ret = OB_NOT_SUPPORTED;
              err_msg = "unsupported parquet dictionary physical type";
              break;
          }
          if (OB_SUCC(ret)) {
            dictionary.push_back(value);
          }
        }
        if (OB_FAIL(ret)) {
          err_msg = err_msg.empty() ? "failed to decode parquet dictionary" : err_msg;
          break;
        }
      } else if (PQ_PAGE_DATA == header.type_ || PQ_PAGE_DATA_V2 == header.type_) {
        const bool is_v2 = (PQ_PAGE_DATA_V2 == header.type_);
        std::string raw;
        const uint8_t *page_data = reinterpret_cast<const uint8_t *>(payload.data());
        int64_t page_size = static_cast<int64_t>(payload.size());
        if (!is_v2 && PQ_CODEC_SNAPPY == chunk.codec_) {
          if (OB_FAIL(snappy_decompress(
                  reinterpret_cast<const uint8_t *>(payload.data()), payload.size(),
                  raw, err_msg))) {
            break;
          }
          page_data = reinterpret_cast<const uint8_t *>(raw.data());
          page_size = static_cast<int64_t>(raw.size());
        }
        int64_t pos = 0;
        int64_t rep_len = 0;
        int64_t def_len = 0;
        if (is_v2) {
          rep_len = header.rep_levels_byte_len_;
          def_len = header.def_levels_byte_len_;
        } else {
          if (max_def_level > 0) {
            if (pos + 4 > page_size) {
              ret = OB_INVALID_ARGUMENT;
              err_msg = "invalid parquet definition level header";
              break;
            }
            def_len = read_int32_le(page_data + pos);
            pos += 4;
          }
        }
        const int64_t def_offset = pos;
        pos += rep_len;
        const int64_t def_len_pos = pos;
        pos += def_len;
        if (pos > page_size || def_len < 0 || rep_len < 0) {
          ret = OB_INVALID_ARGUMENT;
          err_msg = "invalid parquet data page layout";
          break;
        }
        // definition levels
        std::vector<uint32_t> def_levels;
        if (max_def_level > 0) {
          if (is_v2) {
            if (OB_FAIL(decode_rle_hybrid(page_data + def_len_pos, def_len,
                                          bit_width_for(max_def_level),
                                          header.num_values_, def_levels))) {
              err_msg = "failed to decode parquet definition levels";
              break;
            }
          } else if (OB_FAIL(decode_rle_hybrid(page_data + def_len_pos, def_len,
                                               bit_width_for(max_def_level),
                                               header.num_values_, def_levels))) {
            err_msg = "failed to decode parquet definition levels";
            break;
          }
        }
        (void)def_offset;
        int64_t present_count = header.num_values_;
        if (max_def_level > 0) {
          present_count = 0;
          for (size_t i = 0; i < def_levels.size(); ++i) {
            if (static_cast<int32_t>(def_levels[i]) == max_def_level) {
              ++present_count;
            }
          }
        }
        // values
        std::string raw_values;
        const uint8_t *vp = page_data + pos;
        int64_t values_len = page_size - pos;
        if (is_v2 && PQ_CODEC_SNAPPY == chunk.codec_ && header.is_compressed_) {
          if (OB_FAIL(snappy_decompress(vp, values_len, raw_values, err_msg))) {
            break;
          }
          vp = reinterpret_cast<const uint8_t *>(raw_values.data());
          values_len = static_cast<int64_t>(raw_values.size());
        }
        const uint8_t *vstart = vp;
        const uint8_t *vend = vp + values_len;
        std::vector<std::string> page_values;
        page_values.reserve(static_cast<size_t>(present_count));
        if (PQ_ENCODING_PLAIN_DICTIONARY == header.encoding_ ||
            PQ_ENCODING_RLE_DICTIONARY == header.encoding_) {
          if (dictionary.empty() || vp >= vend) {
            ret = OB_INVALID_ARGUMENT;
            err_msg = "missing parquet dictionary";
            break;
          }
          const int32_t dict_bit_width = *vp++;
          std::vector<uint32_t> indices;
          if (OB_FAIL(decode_rle_hybrid(vp, vend - vp, dict_bit_width, present_count,
                                        indices))) {
            err_msg = "failed to decode parquet dictionary indices";
            break;
          }
          for (size_t i = 0; i < indices.size(); ++i) {
            if (indices[i] >= dictionary.size()) {
              ret = OB_INVALID_ARGUMENT;
              err_msg = "invalid parquet dictionary index";
              break;
            }
            page_values.push_back(dictionary[indices[i]]);
          }
        } else if (PQ_BOOLEAN == physical_type) {
          std::vector<uint32_t> bits;
          if (PQ_ENCODING_RLE == header.encoding_) {
            if (OB_FAIL(decode_rle_hybrid(vp, vend - vp, 1, present_count, bits))) {
              err_msg = "failed to decode parquet boolean values";
              break;
            }
          } else {
            const int64_t bytes = (present_count + 7) / 8;
            if (vp + bytes > vend) {
              ret = OB_INVALID_ARGUMENT;
              err_msg = "invalid parquet boolean page";
              break;
            }
            if (OB_FAIL(unpack_bits(vp, bytes, 1, present_count, bits))) {
              err_msg = "failed to decode parquet boolean values";
              break;
            }
          }
          for (size_t i = 0; i < bits.size(); ++i) {
            page_values.push_back(bits[i] ? "true" : "false");
          }
        } else if (PQ_INT32 == physical_type) {
          for (int64_t i = 0; OB_SUCC(ret) && i < present_count; ++i) {
            if (vstart + (i + 1) * 4 > vend) {
              ret = OB_INVALID_ARGUMENT;
              err_msg = "invalid parquet int32 page";
            } else {
              const int32_t raw_value = read_int32_le(vstart + i * 4);
              page_values.push_back((PQ_CONVERTED_DATE == converted_type)
                                        ? format_date(raw_value)
                                        : std::to_string(raw_value));
            }
          }
        } else if (PQ_INT64 == physical_type) {
          for (int64_t i = 0; OB_SUCC(ret) && i < present_count; ++i) {
            if (vstart + (i + 1) * 8 > vend) {
              ret = OB_INVALID_ARGUMENT;
              err_msg = "invalid parquet int64 page";
            } else {
              const int64_t raw_value = read_int64_le(vstart + i * 8);
              page_values.push_back((PQ_CONVERTED_TIMESTAMP_MILLIS == converted_type)
                                        ? format_datetime(raw_value / 1000)
                                        : std::to_string(raw_value));
            }
          }
        } else if (PQ_FLOAT == physical_type) {
          for (int64_t i = 0; OB_SUCC(ret) && i < present_count; ++i) {
            if (vstart + (i + 1) * 4 > vend) {
              ret = OB_INVALID_ARGUMENT;
              err_msg = "invalid parquet float page";
            } else {
              const int32_t raw_value = read_int32_le(vstart + i * 4);
              float float_value = 0;
              memcpy(&float_value, &raw_value, 4);
              page_values.push_back(format_double(static_cast<double>(float_value)));
            }
          }
        } else if (PQ_DOUBLE == physical_type) {
          for (int64_t i = 0; OB_SUCC(ret) && i < present_count; ++i) {
            if (vstart + (i + 1) * 8 > vend) {
              ret = OB_INVALID_ARGUMENT;
              err_msg = "invalid parquet double page";
            } else {
              const int64_t raw_value = read_int64_le(vstart + i * 8);
              double double_value = 0;
              memcpy(&double_value, &raw_value, 8);
              page_values.push_back(format_double(double_value));
            }
          }
        } else if (PQ_BYTE_ARRAY == physical_type) {
          const uint8_t *p = vstart;
          for (int64_t i = 0; OB_SUCC(ret) && i < present_count; ++i) {
            if (p + 4 > vend) {
              ret = OB_INVALID_ARGUMENT;
              err_msg = "invalid parquet byte array page";
            } else {
              const int32_t len = read_int32_le(p);
              p += 4;
              if (len < 0 || p + len > vend) {
                ret = OB_INVALID_ARGUMENT;
                err_msg = "invalid parquet byte array length";
              } else {
                page_values.push_back(
                    std::string(reinterpret_cast<const char *>(p), static_cast<size_t>(len)));
                p += len;
              }
            }
          }
        } else if (PQ_FIXED_LEN_BYTE_ARRAY == physical_type) {
          const int32_t fixed_len = leaf.type_length_;
          for (int64_t i = 0; OB_SUCC(ret) && i < present_count; ++i) {
            if (fixed_len <= 0 || vstart + (i + 1) * fixed_len > vend) {
              ret = OB_INVALID_ARGUMENT;
              err_msg = "invalid parquet fixed length page";
            } else {
              page_values.push_back(std::string(
                  reinterpret_cast<const char *>(vstart + i * fixed_len),
                  static_cast<size_t>(fixed_len)));
            }
          }
        } else {
          ret = OB_NOT_SUPPORTED;
          err_msg = "unsupported parquet physical type";
        }
        if (OB_FAIL(ret)) {
          break;
        }
        size_t value_pos = 0;
        for (int32_t i = 0; i < header.num_values_; ++i) {
          const bool is_null =
              (max_def_level > 0 &&
               (i >= static_cast<int32_t>(def_levels.size()) ||
                static_cast<int32_t>(def_levels[static_cast<size_t>(i)]) != max_def_level));
          if (is_null) {
            values.push_back(std::string());
            nulls.push_back(true);
          } else if (value_pos < page_values.size()) {
            values.push_back(page_values[value_pos]);
            nulls.push_back(false);
            ++value_pos;
          } else {
            ret = OB_INVALID_ARGUMENT;
            err_msg = "parquet page value count mismatch";
            break;
          }
        }
        if (OB_FAIL(ret)) {
          break;
        }
        remaining_values -= header.num_values_;
      } else {
        // 其它页（index page）直接跳过
      }
      if (header.compressed_size_ <= 0) {
        ret = OB_INVALID_ARGUMENT;
        err_msg = "invalid parquet page size";
        break;
      }
      if (offset >= file.size_) {
        done = true;
      }
    }
  }
  return ret;
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
//  对外接口
// ---------------------------------------------------------------------------

int ParquetFileReader::read_schema(const std::string &path,
                                   std::vector<FileColumnMeta> &columns,
                                   std::string &err_msg)
{
  int ret = OB_SUCCESS;
  columns.clear();
  FileHandle file;
  if (OB_FAIL(file.open_file(path))) {
    err_msg = "cannot open file: " + path;
  } else {
    PqFileMeta meta;
    if (OB_FAIL(load_file_metadata(file, meta, err_msg))) {
    } else {
      for (size_t i = 0; i < meta.schema_.size(); ++i) {
        const PqSchemaElement &element = meta.schema_[i];
        if (element.type_ >= 0) {
          FileColumnMeta column;
          column.name_ = element.name_;
          column.nullable_ = (1 == element.repetition_);
          switch (element.type_) {
            case PQ_BOOLEAN:
              column.type_ = FileValueType::BOOLEAN;
              break;
            case PQ_INT32:
              column.type_ = (PQ_CONVERTED_DATE == element.converted_type_)
                                 ? FileValueType::DATE
                                 : FileValueType::BIGINT;
              break;
            case PQ_INT64:
              column.type_ = (PQ_CONVERTED_TIMESTAMP_MILLIS == element.converted_type_)
                                 ? FileValueType::DATETIME
                                 : FileValueType::BIGINT;
              break;
            case PQ_FLOAT:
            case PQ_DOUBLE:
              column.type_ = FileValueType::DOUBLE;
              break;
            default:
              column.type_ = FileValueType::VARCHAR;
              break;
          }
          columns.push_back(column);
        }
      }
      if (columns.empty()) {
        ret = OB_INVALID_ARGUMENT;
        err_msg = "parquet file has no leaf column";
      }
    }
  }
  file.close_file();
  return ret;
}

struct ParquetBatchReader::Impl
{
  FileHandle file_;
  PqFileMeta meta_;
  std::vector<FileColumnMeta> schema_;
  std::vector<int64_t> projection_;
  std::vector<PqSchemaElement> leaves_;
  int64_t next_row_group_ = 0;
  int64_t row_in_group_ = 0;
  int64_t group_rows_ = 0;
  std::vector<std::vector<std::string>> column_values_;
  std::vector<std::vector<bool>> column_nulls_;
  std::string path_;
};

ParquetBatchReader::ParquetBatchReader() : impl_(new (std::nothrow) Impl())
{
}

ParquetBatchReader::~ParquetBatchReader()
{
  close();
  delete impl_;
  impl_ = NULL;
}

void ParquetBatchReader::close()
{
  if (NULL != impl_) {
    impl_->file_.close_file();
    impl_->column_values_.clear();
    impl_->column_nulls_.clear();
    impl_->leaves_.clear();
    impl_->next_row_group_ = 0;
    impl_->row_in_group_ = 0;
    impl_->group_rows_ = 0;
  }
}

int ParquetBatchReader::open(const std::string &path,
                             const std::vector<FileColumnMeta> &schema,
                             const std::vector<int64_t> &projection,
                             const int64_t batch_rows)
{
  int ret = OB_SUCCESS;
  (void)batch_rows;
  std::string err_msg;
  if (NULL == impl_) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    close();
    impl_->path_ = path;
    impl_->schema_ = schema;
    impl_->projection_ = projection;
    if (OB_FAIL(impl_->file_.open_file(path))) {
      err_msg = "cannot open file: " + path;
    } else if (OB_FAIL(load_file_metadata(impl_->file_, impl_->meta_, err_msg))) {
    } else {
      for (size_t i = 0; i < impl_->meta_.schema_.size(); ++i) {
        if (impl_->meta_.schema_[i].type_ >= 0) {
          impl_->leaves_.push_back(impl_->meta_.schema_[i]);
        }
      }
      if (impl_->leaves_.empty()) {
        ret = OB_INVALID_ARGUMENT;
        err_msg = "parquet file has no leaf column";
      } else if (static_cast<int64_t>(schema.size()) !=
                 static_cast<int64_t>(impl_->leaves_.size())) {
        ret = OB_INVALID_ARGUMENT;
        err_msg = "parquet schema does not match resolved columns";
      }
    }
  }
  return ret;
}

int ParquetBatchReader::next_batch(const int64_t batch_rows,
                                   std::vector<FileScanRow> &rows,
                                   bool &eof,
                                   std::string &err_msg)
{
  int ret = OB_SUCCESS;
  rows.clear();
  eof = false;
  if (NULL == impl_) {
    ret = OB_NOT_INIT;
  } else {
    while (OB_SUCC(ret) && static_cast<int64_t>(rows.size()) < batch_rows) {
      if (impl_->row_in_group_ >= impl_->group_rows_) {
        if (impl_->next_row_group_ >=
            static_cast<int64_t>(impl_->meta_.row_groups_.size())) {
          eof = true;
          break;
        }
        const PqRowGroup &group =
            impl_->meta_.row_groups_[static_cast<size_t>(impl_->next_row_group_)];
        impl_->column_values_.clear();
        impl_->column_nulls_.clear();
        impl_->column_values_.resize(impl_->projection_.size());
        impl_->column_nulls_.resize(impl_->projection_.size());
        impl_->row_in_group_ = 0;
        impl_->group_rows_ = group.num_rows_;
        for (size_t i = 0; OB_SUCC(ret) && i < impl_->projection_.size(); ++i) {
          const int64_t column_index = impl_->projection_[i];
          if (column_index < 0 ||
              column_index >= static_cast<int64_t>(impl_->leaves_.size()) ||
              column_index >= static_cast<int64_t>(group.columns_.size())) {
            ret = OB_INVALID_ARGUMENT;
            err_msg = "invalid parquet column index";
          } else {
            ret = decode_column_chunk(impl_->file_,
                                      group.columns_[static_cast<size_t>(column_index)],
                                      impl_->leaves_[static_cast<size_t>(column_index)],
                                      impl_->column_values_[i],
                                      impl_->column_nulls_[i],
                                      err_msg);
          }
        }
        if (OB_SUCC(ret)) {
          ++impl_->next_row_group_;
        }
      } else {
        FileScanRow row;
        for (size_t i = 0; i < impl_->projection_.size(); ++i) {
          const size_t index = static_cast<size_t>(impl_->row_in_group_);
          if (index < impl_->column_values_[i].size()) {
            row.values_.push_back(impl_->column_values_[i][index]);
            row.nulls_.push_back(impl_->column_nulls_[i][index]);
          } else {
            row.values_.push_back(std::string());
            row.nulls_.push_back(true);
          }
        }
        rows.push_back(row);
        ++impl_->row_in_group_;
      }
    }
  }
  return ret;
}

int ParquetBatchReader::reset()
{
  int ret = OB_SUCCESS;
  if (NULL == impl_) {
    ret = OB_NOT_INIT;
  } else {
    std::vector<FileColumnMeta> schema = impl_->schema_;
    std::vector<int64_t> projection = impl_->projection_;
    const std::string path = impl_->path_;
    ret = open(path, schema, projection, 1024);
  }
  return ret;
}

}  // namespace sql
}  // namespace oceanbase
