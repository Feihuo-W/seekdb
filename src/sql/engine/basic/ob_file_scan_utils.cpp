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

#include "sql/engine/basic/ob_file_scan_utils.h"
#include "sql/engine/basic/ob_file_parquet_reader.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <vector>

#include "lib/ob_errno.h"
#include "lib/utility/ob_macro_utils.h"

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace oceanbase
{
namespace sql
{

namespace
{

const int64_t FILE_SCAN_BUFFER_SIZE = 1 << 20;      // 1MB 读缓冲
const int64_t FILE_SCAN_MAX_CELL_SIZE = 64 << 20;   // 单字段最大 64MB
const int64_t FILE_SCAN_MAX_LINE_SIZE = 128 << 20;  // 单行最大 128MB

bool is_digit(const char c) { return c >= '0' && c <= '9'; }

bool is_space_char(const char c)
{
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

void trim_text(std::string &str)
{
  int64_t begin = 0;
  int64_t end = static_cast<int64_t>(str.size());
  while (begin < end && is_space_char(str[static_cast<size_t>(begin)])) {
    ++begin;
  }
  while (end > begin && is_space_char(str[static_cast<size_t>(end - 1)])) {
    --end;
  }
  if (begin > 0 || end < static_cast<int64_t>(str.size())) {
    str = str.substr(static_cast<size_t>(begin), static_cast<size_t>(end - begin));
  }
}

bool all_digits(const std::string &str, const int64_t begin, const int64_t end)
{
  bool result = (begin < end);
  for (int64_t i = begin; result && i < end; ++i) {
    result = is_digit(str[static_cast<size_t>(i)]);
  }
  return result;
}

int64_t parse_digits(const std::string &str, const int64_t begin, const int64_t end)
{
  int64_t value = 0;
  for (int64_t i = begin; i < end; ++i) {
    value = value * 10 + (str[static_cast<size_t>(i)] - '0');
  }
  return value;
}

bool is_leap_year(const int64_t year)
{
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int64_t days_in_month(const int64_t year, const int64_t month)
{
  static const int32_t DAYS[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int64_t days = (month >= 1 && month <= 12) ? DAYS[month - 1] : 0;
  if (2 == month && is_leap_year(year)) {
    days = 29;
  }
  return days;
}

bool is_bool_text(const std::string &value)
{
  return 0 == strcasecmp(value.c_str(), "true") || 0 == strcasecmp(value.c_str(), "false");
}

bool is_int64_text(const std::string &value, int64_t &out)
{
  bool valid = !value.empty();
  size_t begin = 0;
  if (valid && ('-' == value[0] || '+' == value[0])) {
    begin = 1;
  }
  if (begin >= value.size()) {
    valid = false;
  }
  for (size_t i = begin; valid && i < value.size(); ++i) {
    valid = is_digit(value[i]);
  }
  if (valid) {
    errno = 0;
    char *end_ptr = NULL;
    const long long parsed = strtoll(value.c_str(), &end_ptr, 10);
    if (0 != errno || NULL == end_ptr || '\0' != *end_ptr) {
      valid = false;
    } else {
      out = static_cast<int64_t>(parsed);
    }
  }
  return valid;
}

bool is_double_text(const std::string &value)
{
  bool valid = !value.empty();
  if (valid) {
    errno = 0;
    char *end_ptr = NULL;
    (void)strtod(value.c_str(), &end_ptr);
    valid = (0 == errno && NULL != end_ptr && '\0' == *end_ptr);
  }
  return valid;
}

bool wildcard_match(const std::string &text,
                    const size_t text_pos,
                    const std::string &pattern,
                    const size_t pattern_pos,
                    const bool path_mode)
{
  size_t ti = text_pos;
  size_t pi = pattern_pos;
  while (pi < pattern.size()) {
    const char pc = pattern[pi];
    if ('*' == pc) {
      bool cross_dir = false;
      size_t next_pi = pi + 1;
      if (path_mode && next_pi < pattern.size() && '*' == pattern[next_pi]) {
        cross_dir = true;
        ++next_pi;
      }
      if (next_pi >= pattern.size()) {
        return cross_dir ? true : (std::string::npos == text.find('/', ti));
      }
      for (size_t k = ti; k <= text.size(); ++k) {
        if (!cross_dir && k > ti && '/' == text[static_cast<size_t>(k - 1)]) {
          break;
        }
        if (wildcard_match(text, k, pattern, next_pi, path_mode)) {
          return true;
        }
      }
      return false;
    } else if ('?' == pc) {
      if (ti >= text.size() || '/' == text[ti]) {
        return false;
      }
      ++ti;
      ++pi;
    } else {
      if (ti >= text.size() || text[ti] != pc) {
        return false;
      }
      ++ti;
      ++pi;
    }
  }
  return ti == text.size();
}

std::string join_path(const std::string &dir, const std::string &name)
{
  std::string result;
  if (dir.empty()) {
    result = name;
  } else {
    const char last = dir[static_cast<size_t>(dir.size() - 1)];
    result = ('/' == last || '\\' == last) ? (dir + name) : (dir + "/" + name);
  }
  return result;
}

std::string dir_name(const std::string &path)
{
  const size_t pos = path.find_last_of('/');
  std::string result;
  if (std::string::npos == pos) {
    result = "";
  } else if (0 == pos) {
    result = "/";
  } else {
    result = path.substr(0, pos);
  }
  return result;
}

std::string base_name(const std::string &path)
{
  const size_t pos = path.find_last_of('/');
  return (std::string::npos == pos) ? path : path.substr(pos + 1);
}

bool path_is_directory(const std::string &path)
{
  bool result = false;
#ifdef _WIN32
  const DWORD attr = GetFileAttributesA(path.c_str());
  result = (INVALID_FILE_ATTRIBUTES != attr) && (attr & FILE_ATTRIBUTE_DIRECTORY);
#else
  struct stat st;
  result = (0 == stat(path.c_str(), &st)) && S_ISDIR(st.st_mode);
#endif
  return result;
}

int list_directory(const std::string &dir, std::vector<std::string> &names, std::string &err_msg)
{
  int ret = OB_SUCCESS;
  names.clear();
#ifdef _WIN32
  std::string query = dir.empty() ? std::string("*") : (dir + "\\*");
  WIN32_FIND_DATAA data;
  HANDLE handle = FindFirstFileA(query.c_str(), &data);
  if (INVALID_HANDLE_VALUE != handle) {
    do {
      std::string name = data.cFileName;
      if ("." != name && ".." != name) {
        names.push_back(name);
      }
    } while (FindNextFileA(handle, &data));
    FindClose(handle);
  }
#else
  DIR *directory = opendir(dir.empty() ? "." : dir.c_str());
  if (NULL == directory) {
    ret = OB_FILE_NOT_EXIST;
    err_msg = "cannot list directory: " + dir;
  } else {
    struct dirent *entry = NULL;
    while (NULL != (entry = readdir(directory))) {
      std::string name = entry->d_name;
      if ("." != name && ".." != name) {
        names.push_back(name);
      }
    }
    closedir(directory);
  }
#endif
  std::sort(names.begin(), names.end());
  return ret;
}

// 带缓冲的顺序字符读取器，支撑 CSV/JSONL 的流式解析。
class BufferedCharReader
{
public:
  BufferedCharReader() : fp_(NULL), pos_(0), len_(0), eof_(false), failed_(false)
  {
    buffer_.resize(static_cast<size_t>(FILE_SCAN_BUFFER_SIZE));
  }
  ~BufferedCharReader() { close(); }

  int open(const std::string &path)
  {
    close();
    fp_ = fopen(path.c_str(), "rb");
    if (NULL == fp_) {
      return (ENOENT == errno) ? OB_FILE_NOT_EXIST : OB_IO_ERROR;
    }
    return OB_SUCCESS;
  }

  void close()
  {
    if (NULL != fp_) {
      fclose(fp_);
      fp_ = NULL;
    }
    pos_ = 0;
    len_ = 0;
    eof_ = false;
  }

  bool failed() const { return failed_; }

  int peek()
  {
    const int c = get();
    if (-1 != c && pos_ > 0) {
      --pos_;
    }
    return c;
  }

  // 返回 0..255，文件末尾返回 -1。
  int get()
  {
    int c = -1;
    if (pos_ < len_) {
      c = static_cast<unsigned char>(buffer_[pos_++]);
    } else if (!eof_) {
      len_ = fread(&buffer_[0], 1, buffer_.size(), fp_);
      pos_ = 0;
      if (len_ < buffer_.size()) {
        eof_ = true;
        if (0 != ferror(fp_) && 0 == len_) {
          failed_ = true;
        }
      }
      if (len_ > 0) {
        c = static_cast<unsigned char>(buffer_[pos_++]);
      }
    }
    return c;
  }

private:
  FILE *fp_;
  std::vector<char> buffer_;
  size_t pos_;
  size_t len_;
  bool eof_;
  bool failed_;
};

// 逐记录解析 CSV：支持引号包裹、"" 转义、引号内换行与 CRLF。
class CsvRecordParser
{
public:
  explicit CsvRecordParser(BufferedCharReader &reader) : reader_(reader), line_no_(1) {}

  int64_t get_line_no() const { return line_no_; }

  // 返回 OB_SUCCESS / OB_ITER_END / 其它错误。
  int next_record(std::vector<std::string> &fields,
                  std::vector<bool> &quoted,
                  std::string &err_msg)
  {
    int ret = OB_SUCCESS;
    fields.clear();
    quoted.clear();
    bool in_quotes = false;
    bool field_started = false;
    bool quoted_field = false;
    bool record_started = false;
    std::string field;
    bool done = false;

    while (!done) {
      const int c = reader_.get();
      if (-1 == c) {
        if (reader_.failed()) {
          ret = OB_IO_ERROR;
          err_msg = "failed to read csv file";
        } else if (in_quotes) {
          ret = OB_INVALID_ARGUMENT;
          err_msg = "unterminated quoted field at end of csv file";
        } else if (!record_started && fields.empty() && field.empty() && !quoted_field) {
          ret = OB_ITER_END;
        } else {
          fields.push_back(field);
          quoted.push_back(quoted_field);
        }
        done = true;
        continue;
      }
      const char ch = static_cast<char>(c);
      if (in_quotes) {
        if ('"' == ch) {
          if ('"' == reader_.peek()) {
            (void)reader_.get();
            field.push_back('"');
          } else {
            in_quotes = false;
          }
        } else {
          if ('\n' == ch) {
            ++line_no_;
          }
          field.push_back(ch);
        }
        record_started = true;
      } else if ('"' == ch && !field_started) {
        in_quotes = true;
        quoted_field = true;
        field_started = true;
        record_started = true;
      } else if (',' == ch) {
        fields.push_back(field);
        quoted.push_back(quoted_field);
        field.clear();
        field_started = false;
        quoted_field = false;
        record_started = true;
      } else if ('\n' == ch || '\r' == ch) {
        if ('\r' == ch && '\n' == reader_.peek()) {
          (void)reader_.get();
        }
        ++line_no_;
        fields.push_back(field);
        quoted.push_back(quoted_field);
        done = true;
      } else {
        field.push_back(ch);
        field_started = true;
        record_started = true;
      }
      if (!done && static_cast<int64_t>(field.size()) > FILE_SCAN_MAX_CELL_SIZE) {
        ret = OB_SIZE_OVERFLOW;
        err_msg = "csv field is too large";
        done = true;
      }
    }
    return ret;
  }

private:
  BufferedCharReader &reader_;
  int64_t line_no_;
};

bool is_empty_record(const std::vector<std::string> &fields, const std::vector<bool> &quoted)
{
  return 1 == fields.size() && fields[0].empty() && !quoted[0];
}

bool is_null_field(const std::string &value, const bool quoted, const std::string &null_str)
{
  bool is_null = false;
  if (quoted) {
    is_null = false;
  } else if (!null_str.empty()) {
    is_null = (value == null_str);
  } else {
    is_null = value.empty();
  }
  return is_null;
}

// ---------------------------- JSONL ----------------------------

enum class JsonValueKind
{
  STRING = 0,
  INTEGER,
  NUMBER,
  BOOLEAN,
  NUL
};

struct JsonPair
{
  std::string key_;
  std::string text_;
  JsonValueKind kind_ = JsonValueKind::NUL;
};

void append_utf8(std::string &output, const uint32_t code_point)
{
  if (code_point <= 0x7F) {
    output.push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7FF) {
    output.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
    output.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else if (code_point <= 0xFFFF) {
    output.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
    output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    output.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else {
    output.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
    output.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
    output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    output.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  }
}

int hex_value(const char c)
{
  int value = -1;
  if (c >= '0' && c <= '9') {
    value = c - '0';
  } else if (c >= 'a' && c <= 'f') {
    value = c - 'a' + 10;
  } else if (c >= 'A' && c <= 'F') {
    value = c - 'A' + 10;
  }
  return value;
}

// 解析单行 JSON object，只支持一层标量字段；嵌套对象/数组直接报错。
class JsonLineParser
{
public:
  explicit JsonLineParser(const std::string &line) : line_(line), pos_(0) {}

  size_t get_error_pos() const { return pos_; }

  int parse(std::vector<JsonPair> &pairs, std::string &err_msg)
  {
    int ret = OB_SUCCESS;
    pairs.clear();
    skip_space();
    if (pos_ >= line_.size() || '{' != line_[pos_]) {
      ret = OB_INVALID_ARGUMENT;
      err_msg = "jsonl line must be a json object";
    } else {
      ++pos_;
      skip_space();
      if (pos_ < line_.size() && '}' == line_[pos_]) {
        ++pos_;
      } else {
        bool finished = false;
        while (OB_SUCC(ret) && !finished) {
          std::string key;
          if (OB_FAIL(parse_string(key, err_msg))) {
          } else {
            skip_space();
            if (pos_ >= line_.size() || ':' != line_[pos_]) {
              ret = OB_INVALID_ARGUMENT;
              err_msg = "expect ':' after json key";
            } else {
              ++pos_;
              JsonPair pair;
              pair.key_ = key;
              if (OB_FAIL(parse_value(pair, err_msg))) {
              } else {
                pairs.push_back(pair);
              }
            }
          }
          if (OB_FAIL(ret)) {
          } else {
            skip_space();
            if (pos_ < line_.size() && ',' == line_[pos_]) {
              ++pos_;
              skip_space();
            } else if (pos_ < line_.size() && '}' == line_[pos_]) {
              ++pos_;
              finished = true;
            } else {
              ret = OB_INVALID_ARGUMENT;
              err_msg = "expect ',' or '}' in json object";
            }
          }
        }
      }
      if (OB_SUCC(ret)) {
        skip_space();
        if (pos_ != line_.size()) {
          ret = OB_INVALID_ARGUMENT;
          err_msg = "unexpected trailing characters after json object";
        }
      }
    }
    return ret;
  }

private:
  void skip_space()
  {
    while (pos_ < line_.size() && is_space_char(line_[pos_])) {
      ++pos_;
    }
  }

  int parse_string(std::string &output, std::string &err_msg)
  {
    int ret = OB_SUCCESS;
    output.clear();
    if (pos_ >= line_.size() || '"' != line_[pos_]) {
      ret = OB_INVALID_ARGUMENT;
      err_msg = "expect json string";
    } else {
      ++pos_;
      bool closed = false;
      while (OB_SUCC(ret) && pos_ < line_.size()) {
        const char c = line_[pos_++];
        if ('"' == c) {
          closed = true;
          break;
        } else if ('\\' == c) {
          if (pos_ >= line_.size()) {
            break;
          }
          const char esc = line_[pos_++];
          if ('"' == esc || '\\' == esc || '/' == esc) {
            output.push_back(esc);
          } else if ('b' == esc) {
            output.push_back('\b');
          } else if ('f' == esc) {
            output.push_back('\f');
          } else if ('n' == esc) {
            output.push_back('\n');
          } else if ('r' == esc) {
            output.push_back('\r');
          } else if ('t' == esc) {
            output.push_back('\t');
          } else if ('u' == esc) {
            if (pos_ + 4 > line_.size()) {
              ret = OB_INVALID_ARGUMENT;
              err_msg = "invalid unicode escape in json string";
            } else {
              uint32_t code_point = 0;
              bool valid = true;
              for (int i = 0; i < 4; ++i) {
                const int hv = hex_value(line_[pos_ + static_cast<size_t>(i)]);
                if (hv < 0) {
                  valid = false;
                  break;
                }
                code_point = (code_point << 4) | static_cast<uint32_t>(hv);
              }
              if (!valid) {
                ret = OB_INVALID_ARGUMENT;
                err_msg = "invalid unicode escape in json string";
              } else {
                pos_ += 4;
                append_utf8(output, code_point);
              }
            }
          } else {
            ret = OB_INVALID_ARGUMENT;
            err_msg = "invalid escape character in json string";
          }
        } else {
          output.push_back(c);
        }
      }
      if (OB_SUCC(ret) && !closed) {
        ret = OB_INVALID_ARGUMENT;
        err_msg = "unterminated json string";
      }
    }
    return ret;
  }

  int parse_value(JsonPair &pair, std::string &err_msg)
  {
    int ret = OB_SUCCESS;
    pair.kind_ = JsonValueKind::NUL;
    if (pos_ >= line_.size()) {
      ret = OB_INVALID_ARGUMENT;
      err_msg = "expect json value";
    } else if ('"' == line_[pos_]) {
      pair.kind_ = JsonValueKind::STRING;
      ret = parse_string(pair.text_, err_msg);
    } else if ('{' == line_[pos_] || '[' == line_[pos_]) {
      ret = OB_NOT_SUPPORTED;
      err_msg = "nested json object or array is not supported";
    } else {
      const size_t begin = pos_;
      while (pos_ < line_.size() && ',' != line_[pos_] && '}' != line_[pos_]) {
        ++pos_;
      }
      std::string literal = line_.substr(begin, pos_ - begin);
      trim_text(literal);
      if ("null" == literal) {
        pair.kind_ = JsonValueKind::NUL;
      } else if ("true" == literal || "false" == literal) {
        pair.kind_ = JsonValueKind::BOOLEAN;
        pair.text_ = literal;
      } else {
        int64_t int_value = 0;
        if (is_int64_text(literal, int_value)) {
          pair.kind_ = JsonValueKind::INTEGER;
          pair.text_ = literal;
        } else if (is_double_text(literal)) {
          pair.kind_ = JsonValueKind::NUMBER;
          pair.text_ = literal;
        } else {
          ret = OB_INVALID_ARGUMENT;
          err_msg = "invalid json value: " + literal;
        }
      }
    }
    return ret;
  }

private:
  const std::string &line_;
  size_t pos_;
};

FileValueType json_kind_to_type(const JsonValueKind kind)
{
  FileValueType type = FileValueType::VARCHAR;
  switch (kind) {
    case JsonValueKind::INTEGER:
      type = FileValueType::BIGINT;
      break;
    case JsonValueKind::NUMBER:
      type = FileValueType::DOUBLE;
      break;
    case JsonValueKind::BOOLEAN:
      type = FileValueType::BOOLEAN;
      break;
    default:
      type = FileValueType::VARCHAR;
      break;
  }
  return type;
}

class JsonLineRecordReader
{
public:
  explicit JsonLineRecordReader(BufferedCharReader &reader) : reader_(reader), line_no_(0) {}

  int64_t get_line_no() const { return line_no_; }

  // 返回 OB_SUCCESS / OB_ITER_END / 错误。
  int next_line(std::string &line)
  {
    int ret = OB_SUCCESS;
    line.clear();
    bool done = false;
    while (!done) {
      const int c = reader_.get();
      if (-1 == c) {
        if (reader_.failed()) {
          ret = OB_IO_ERROR;
        } else if (line.empty()) {
          ret = OB_ITER_END;
        } else {
          ++line_no_;
        }
        done = true;
      } else if ('\n' == c) {
        ++line_no_;
        done = true;
      } else if ('\r' == c) {
        if ('\n' == reader_.peek()) {
          (void)reader_.get();
        }
        ++line_no_;
        done = true;
      } else {
        line.push_back(static_cast<char>(c));
        if (static_cast<int64_t>(line.size()) > FILE_SCAN_MAX_LINE_SIZE) {
          ret = OB_SIZE_OVERFLOW;
          done = true;
        }
      }
    }
    return ret;
  }

private:
  BufferedCharReader &reader_;
  int64_t line_no_;
};

int infer_csv_schema(const std::string &path,
                     const FileScanOptions &options,
                     std::vector<FileColumnMeta> &columns,
                     std::string &err_msg)
{
  int ret = OB_SUCCESS;
  BufferedCharReader reader;
  columns.clear();
  if (OB_FAIL(reader.open(path))) {
    err_msg = "cannot open file: " + path;
  } else {
    CsvRecordParser parser(reader);
    std::vector<std::string> fields;
    std::vector<bool> quoted;
    int record_ret = parser.next_record(fields, quoted, err_msg);
    int64_t column_count = 0;
    if (OB_FAIL(record_ret)) {
      if (OB_ITER_END != record_ret) {
        ret = record_ret;
        err_msg = "read_csv: " + err_msg;
      }
    } else {
      column_count = static_cast<int64_t>(fields.size());
      if (options.header_) {
        std::map<std::string, int32_t> name_count;
        for (int64_t i = 0; i < column_count; ++i) {
          std::string name = fields[static_cast<size_t>(i)];
          trim_text(name);
          if (name.empty()) {
            name = "column" + std::to_string(i);
          }
          int32_t &count = name_count[name];
          ++count;
          FileColumnMeta column;
          column.name_ = (count > 1) ? (name + "__" + std::to_string(count)) : name;
          column.type_ = FileValueType::INVALID;
          column.nullable_ = false;
          columns.push_back(column);
        }
      } else {
        for (int64_t i = 0; i < column_count; ++i) {
          FileColumnMeta column;
          column.name_ = "column" + std::to_string(i);
          column.type_ = FileValueType::INVALID;
          column.nullable_ = false;
          columns.push_back(column);
        }
      }
      while (OB_SUCC(ret) && OB_SUCCESS == (record_ret = parser.next_record(fields, quoted, err_msg))) {
        if (is_empty_record(fields, quoted)) {
          continue;
        }
        if (static_cast<int64_t>(fields.size()) > column_count) {
          ret = OB_INVALID_ARGUMENT;
          err_msg = "read_csv: line " + std::to_string(parser.get_line_no() - 1) + " has " +
                    std::to_string(fields.size()) + " fields, but the csv has " +
                    std::to_string(column_count) + " columns";
          break;
        }
        for (int64_t i = 0; i < column_count; ++i) {
          FileColumnMeta &column = columns[static_cast<size_t>(i)];
          if (i >= static_cast<int64_t>(fields.size())) {
            column.nullable_ = true;
          } else {
            const std::string &value = fields[static_cast<size_t>(i)];
            if (is_null_field(value, quoted[static_cast<size_t>(i)], options.null_str_)) {
              column.nullable_ = true;
            } else {
              column.type_ = merge_file_type(column.type_, classify_text_value(value));
              if (static_cast<int64_t>(value.size()) > column.max_length_) {
                column.max_length_ = static_cast<int64_t>(value.size());
              }
            }
          }
        }
      }
      if (OB_SUCCESS == ret && OB_SUCCESS != record_ret && OB_ITER_END != record_ret) {
        ret = record_ret;
        err_msg = "read_csv: " + err_msg;
      }
      if (OB_SUCC(ret)) {
        for (size_t i = 0; i < columns.size(); ++i) {
          if (FileValueType::INVALID == columns[i].type_) {
            columns[i].type_ = FileValueType::VARCHAR;
            columns[i].nullable_ = true;
          }
        }
      }
    }
  }
  return ret;
}

int infer_jsonl_schema(const std::string &path,
                       std::vector<FileColumnMeta> &columns,
                       std::string &err_msg)
{
  int ret = OB_SUCCESS;
  BufferedCharReader reader;
  columns.clear();
  if (OB_FAIL(reader.open(path))) {
    err_msg = "cannot open file: " + path;
  } else {
    JsonLineRecordReader records(reader);
    std::map<std::string, int64_t> index_map;
    std::vector<bool> has_null;
    std::string line;
    int line_ret = OB_SUCCESS;
    while (OB_SUCC(ret) && OB_SUCCESS == (line_ret = records.next_line(line))) {
      if (line.empty()) {
        continue;
      }
      std::vector<JsonPair> pairs;
      JsonLineParser parser(line);
      std::string parse_err;
      const int parse_ret = parser.parse(pairs, parse_err);
      if (OB_FAIL(parse_ret)) {
        ret = parse_ret;
        err_msg = "read_json: line " + std::to_string(records.get_line_no()) + ": " + parse_err;
        break;
      }
      std::set<std::string> seen;
      for (size_t i = 0; i < pairs.size(); ++i) {
        const JsonPair &pair = pairs[i];
        std::map<std::string, int64_t>::iterator it = index_map.find(pair.key_);
        if (it == index_map.end()) {
          FileColumnMeta column;
          column.name_ = pair.key_;
          column.type_ = json_kind_to_type(pair.kind_);
          column.max_length_ = static_cast<int64_t>(pair.text_.size());
          column.nullable_ = (JsonValueKind::NUL == pair.kind_);
          index_map[pair.key_] = static_cast<int64_t>(columns.size());
          columns.push_back(column);
          has_null.push_back(column.nullable_);
        } else {
          FileColumnMeta &column = columns[static_cast<size_t>(it->second)];
          if (JsonValueKind::NUL != pair.kind_) {
            column.type_ = merge_file_type(column.type_, json_kind_to_type(pair.kind_));
            if (static_cast<int64_t>(pair.text_.size()) > column.max_length_) {
              column.max_length_ = static_cast<int64_t>(pair.text_.size());
            }
          } else {
            has_null[static_cast<size_t>(it->second)] = true;
          }
        }
        seen.insert(pair.key_);
      }
      for (size_t i = 0; i < columns.size(); ++i) {
        if (seen.end() == seen.find(columns[i].name_)) {
          has_null[i] = true;
        }
      }
    }
    if (OB_SUCCESS == ret && OB_SUCCESS != line_ret && OB_ITER_END != line_ret) {
      ret = line_ret;
      err_msg = "read_json: failed to read file";
    }
    if (OB_SUCC(ret)) {
      for (size_t i = 0; i < columns.size(); ++i) {
        if (FileValueType::INVALID == columns[i].type_) {
          columns[i].type_ = FileValueType::VARCHAR;
        }
        columns[i].nullable_ = has_null[i];
      }
    }
  }
  return ret;
}

}  // anonymous namespace

const char *file_value_type_name(const FileValueType type)
{
  const char *name = "VARCHAR";
  switch (type) {
    case FileValueType::BIGINT:
      name = "BIGINT";
      break;
    case FileValueType::DOUBLE:
      name = "DOUBLE";
      break;
    case FileValueType::BOOLEAN:
      name = "BOOLEAN";
      break;
    case FileValueType::DATE:
      name = "DATE";
      break;
    case FileValueType::DATETIME:
      name = "DATETIME";
      break;
    default:
      name = "VARCHAR";
      break;
  }
  return name;
}

FileFormatType file_format_from_name(const std::string &name)
{
  FileFormatType format = FileFormatType::CSV;
  std::string lower;
  lower.reserve(name.size());
  for (size_t i = 0; i < name.size(); ++i) {
    lower.push_back(static_cast<char>(tolower(static_cast<unsigned char>(name[i]))));
  }
  if ("csv" == lower) {
    format = FileFormatType::CSV;
  } else if ("json" == lower || "jsonl" == lower || "ndjson" == lower ||
             "newline_delimited" == lower) {
    format = FileFormatType::JSONL;
  } else {
    format = FileFormatType::PARQUET;
  }
  return format;
}

bool is_valid_date_text(const std::string &value)
{
  bool valid = (10 == value.size() && all_digits(value, 0, 4) && '-' == value[4] &&
                all_digits(value, 5, 7) && '-' == value[7] && all_digits(value, 8, 10));
  if (valid) {
    const int64_t year = parse_digits(value, 0, 4);
    const int64_t month = parse_digits(value, 5, 7);
    const int64_t day = parse_digits(value, 8, 10);
    valid = (year >= 1 && month >= 1 && month <= 12 && day >= 1 &&
             day <= days_in_month(year, month));
  }
  return valid;
}

bool is_valid_datetime_text(const std::string &value)
{
  bool valid = (19 == value.size() && (' ' == value[10] || 'T' == value[10]) &&
                all_digits(value, 0, 4) && '-' == value[4] && all_digits(value, 5, 7) &&
                '-' == value[7] && all_digits(value, 8, 10) && all_digits(value, 11, 13) &&
                ':' == value[13] && all_digits(value, 14, 16) && ':' == value[16] &&
                all_digits(value, 17, 19));
  if (valid) {
    const int64_t hour = parse_digits(value, 11, 13);
    const int64_t minute = parse_digits(value, 14, 16);
    const int64_t second = parse_digits(value, 17, 19);
    valid = (is_valid_date_text(value.substr(0, 10)) && hour <= 23 && minute <= 59 &&
             second <= 59);
  }
  return valid;
}

FileValueType classify_text_value(const std::string &value)
{
  FileValueType type = FileValueType::VARCHAR;
  int64_t int_value = 0;
  if (is_bool_text(value)) {
    type = FileValueType::BOOLEAN;
  } else if (is_int64_text(value, int_value)) {
    type = FileValueType::BIGINT;
  } else if (is_double_text(value)) {
    type = FileValueType::DOUBLE;
  } else if (is_valid_datetime_text(value)) {
    type = FileValueType::DATETIME;
  } else if (is_valid_date_text(value)) {
    type = FileValueType::DATE;
  } else {
    type = FileValueType::VARCHAR;
  }
  return type;
}

FileValueType merge_file_type(const FileValueType left, const FileValueType right)
{
  FileValueType merged = FileValueType::VARCHAR;
  if (FileValueType::INVALID == left) {
    merged = right;
  } else if (FileValueType::INVALID == right) {
    merged = left;
  } else if (left == right) {
    merged = left;
  } else if ((FileValueType::BIGINT == left && FileValueType::DOUBLE == right) ||
             (FileValueType::DOUBLE == left && FileValueType::BIGINT == right)) {
    merged = FileValueType::DOUBLE;
  } else if ((FileValueType::DATE == left && FileValueType::DATETIME == right) ||
             (FileValueType::DATETIME == left && FileValueType::DATE == right)) {
    merged = FileValueType::DATETIME;
  } else {
    merged = FileValueType::VARCHAR;
  }
  return merged;
}

int FileScanUtils::get_file_stat(const std::string &path, FileStat &stat)
{
  int ret = OB_SUCCESS;
#ifdef _WIN32
  struct _stat64 st;
  if (0 != ::_stat64(path.c_str(), &st)) {
    ret = (ENOENT == errno) ? OB_FILE_NOT_EXIST : OB_IO_ERROR;
  } else {
    stat.size_ = static_cast<int64_t>(st.st_size);
    stat.mtime_sec_ = static_cast<int64_t>(st.st_mtime);
  }
#else
  struct stat st;
  if (0 != ::stat(path.c_str(), &st)) {
    ret = (ENOENT == errno) ? OB_FILE_NOT_EXIST : OB_IO_ERROR;
  } else if (!S_ISREG(st.st_mode)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    stat.size_ = static_cast<int64_t>(st.st_size);
    stat.mtime_sec_ = static_cast<int64_t>(st.st_mtime);
  }
#endif
  return ret;
}

int FileScanUtils::glob(const std::string &pattern,
                        std::vector<std::string> &files,
                        std::string &err_msg)
{
  int ret = OB_SUCCESS;
  files.clear();
  if (pattern.empty()) {
    ret = OB_INVALID_ARGUMENT;
    err_msg = "glob pattern is empty";
  } else if (std::string::npos == pattern.find('*') && std::string::npos == pattern.find('?')) {
    if (path_is_directory(pattern)) {
      ret = OB_INVALID_ARGUMENT;
      err_msg = "glob pattern matches a directory: " + pattern;
    } else {
      files.push_back(pattern);
    }
  } else if (std::string::npos != pattern.find("**")) {
    // 递归匹配：从 pattern 的静态前缀目录开始遍历
    const size_t star_pos = pattern.find("**");
    const std::string prefix = pattern.substr(0, star_pos);
    std::string start_dir = dir_name(prefix);
    if (start_dir.empty()) {
      start_dir = ".";
    }
    std::vector<std::string> stack;
    stack.push_back(start_dir);
    while (!stack.empty() && OB_SUCC(ret)) {
      const std::string current = stack.back();
      stack.pop_back();
      std::vector<std::string> names;
      if (OB_FAIL(list_directory(current, names, err_msg))) {
      } else {
        for (size_t i = 0; OB_SUCC(ret) && i < names.size(); ++i) {
          const std::string full = join_path(current, names.at(i));
          std::string relative = full;
          if (0 == relative.compare(0, 2, "./")) {
            relative = relative.substr(2);
          }
          if (path_is_directory(full)) {
            stack.push_back(full);
          } else if (wildcard_match(relative, 0, pattern, 0, true)) {
            files.push_back(full);
          }
        }
      }
    }
  } else {
    const std::string dir = dir_name(pattern);
    const std::string name_pattern = base_name(pattern);
    std::vector<std::string> names;
    if (OB_FAIL(list_directory(dir, names, err_msg))) {
    } else {
      for (size_t i = 0; i < names.size(); ++i) {
        if (wildcard_match(names.at(i), 0, name_pattern, 0, false)) {
          const std::string full = join_path(dir, names.at(i));
          if (!path_is_directory(full)) {
            files.push_back(full);
          }
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    std::sort(files.begin(), files.end());
  }
  return ret;
}

int FileScanUtils::infer_schema(const std::string &path,
                                const FileScanOptions &options,
                                std::vector<FileColumnMeta> &columns,
                                std::string &err_msg)
{
  int ret = OB_SUCCESS;
  columns.clear();
  err_msg.clear();
  if (-1 != options.sample_size_) {
    ret = OB_NOT_SUPPORTED;
    err_msg = "read file with sample_size other than -1 is not supported";
  } else if (FileFormatType::PARQUET == options.format_) {
    ret = ParquetFileReader::read_schema(path, columns, err_msg);
  } else if (FileFormatType::JSONL == options.format_) {
    ret = infer_jsonl_schema(path, columns, err_msg);
  } else {
    ret = infer_csv_schema(path, options, columns, err_msg);
  }
  return ret;
}

// ===========================================================================
//  FileBatchScanner
// ===========================================================================

struct FileBatchScanner::Impl
{
  FileScanOptions options_;
  std::vector<FileColumnMeta> schema_;
  std::vector<int64_t> projection_;
  int64_t batch_rows_ = 1024;
  std::string path_;

  BufferedCharReader csv_reader_;
  std::unique_ptr<CsvRecordParser> csv_parser_;
  bool csv_skip_header_ = false;
  bool csv_header_pending_ = false;

  BufferedCharReader json_reader_;
  std::unique_ptr<JsonLineRecordReader> json_records_;
  std::map<std::string, int64_t> json_index_map_;

  std::unique_ptr<ParquetBatchReader> parquet_reader_;
};

FileBatchScanner::FileBatchScanner() : impl_(new (std::nothrow) Impl())
{
}

FileBatchScanner::~FileBatchScanner()
{
  close();
  delete impl_;
  impl_ = NULL;
}

int FileBatchScanner::open(const std::string &path,
                           const FileScanOptions &options,
                           const std::vector<FileColumnMeta> &schema,
                           const std::vector<int64_t> &projection,
                           const int64_t batch_rows)
{
  int ret = OB_SUCCESS;
  if (NULL == impl_) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    close();
    impl_->options_ = options;
    impl_->schema_ = schema;
    impl_->projection_ = projection;
    impl_->batch_rows_ = (batch_rows > 0) ? batch_rows : 1024;
    impl_->path_ = path;
    if (FileFormatType::CSV == options.format_) {
      if (OB_FAIL(impl_->csv_reader_.open(path))) {
      } else {
        impl_->csv_parser_.reset(new (std::nothrow) CsvRecordParser(impl_->csv_reader_));
        if (NULL == impl_->csv_parser_) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          impl_->csv_header_pending_ = options.header_;
        }
      }
    } else if (FileFormatType::JSONL == options.format_) {
      if (OB_FAIL(impl_->json_reader_.open(path))) {
      } else {
        impl_->json_records_.reset(new (std::nothrow) JsonLineRecordReader(impl_->json_reader_));
        if (NULL == impl_->json_records_) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          for (size_t i = 0; i < schema.size(); ++i) {
            impl_->json_index_map_[schema[i].name_] = static_cast<int64_t>(i);
          }
        }
      }
    } else {
      impl_->parquet_reader_.reset(new (std::nothrow) ParquetBatchReader());
      if (NULL == impl_->parquet_reader_) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        ret = impl_->parquet_reader_->open(path, schema, projection, impl_->batch_rows_);
      }
    }
  }
  return ret;
}

int FileBatchScanner::next_batch(std::vector<FileScanRow> &rows, bool &eof, std::string &err_msg)
{
  int ret = OB_SUCCESS;
  rows.clear();
  eof = false;
  err_msg.clear();
  if (NULL == impl_) {
    ret = OB_NOT_INIT;
  } else if (impl_->batch_rows_ <= 0) {
    ret = OB_ERR_UNEXPECTED;
  } else if (FileFormatType::PARQUET == impl_->options_.format_) {
    ret = impl_->parquet_reader_->next_batch(impl_->batch_rows_, rows, eof, err_msg);
  } else {
    const int64_t column_count = static_cast<int64_t>(impl_->schema_.size());
    const int64_t batch = impl_->batch_rows_;
    std::vector<std::string> fields;
    std::vector<bool> quoted;
    bool done = false;
    while (OB_SUCC(ret) && !done && static_cast<int64_t>(rows.size()) < batch) {
      if (impl_->csv_parser_ != NULL) {
        if (impl_->csv_header_pending_) {
          int header_ret = impl_->csv_parser_->next_record(fields, quoted, err_msg);
          impl_->csv_header_pending_ = false;
          if (OB_SUCCESS != header_ret) {
            if (OB_ITER_END == header_ret) {
              eof = true;
              done = true;
              err_msg.clear();
            } else {
              ret = header_ret;
            }
            continue;
          }
        }
        int record_ret = impl_->csv_parser_->next_record(fields, quoted, err_msg);
        if (OB_ITER_END == record_ret) {
          eof = true;
          done = true;
        } else if (OB_FAIL(record_ret)) {
          ret = record_ret;
        } else if (is_empty_record(fields, quoted)) {
          continue;
        } else if (static_cast<int64_t>(fields.size()) > column_count) {
          ret = OB_INVALID_ARGUMENT;
          err_msg = "read_csv: line " + std::to_string(impl_->csv_parser_->get_line_no() - 1) +
                    " has " + std::to_string(fields.size()) + " fields, but the csv has " +
                    std::to_string(column_count) + " columns";
        } else {
          FileScanRow row;
          for (size_t i = 0; i < impl_->projection_.size(); ++i) {
            const int64_t index = impl_->projection_[i];
            if (index < 0 || index >= static_cast<int64_t>(fields.size())) {
              row.values_.push_back("");
              row.nulls_.push_back(true);
            } else {
              const std::string &value = fields[static_cast<size_t>(index)];
              const bool null_value =
                  is_null_field(value, quoted[static_cast<size_t>(index)], impl_->options_.null_str_);
              row.values_.push_back(null_value ? std::string() : value);
              row.nulls_.push_back(null_value);
            }
          }
          rows.push_back(row);
        }
      } else {
        std::string line;
        int line_ret = impl_->json_records_->next_line(line);
        if (OB_ITER_END == line_ret) {
          eof = true;
          done = true;
        } else if (OB_FAIL(line_ret)) {
          ret = line_ret;
          err_msg = "read_json: failed to read file";
        } else if (line.empty()) {
          continue;
        } else {
          std::vector<JsonPair> pairs;
          JsonLineParser parser(line);
          std::string parse_err;
          const int parse_ret = parser.parse(pairs, parse_err);
          if (OB_FAIL(parse_ret)) {
            ret = parse_ret;
            err_msg = "read_json: line " + std::to_string(impl_->json_records_->get_line_no()) +
                      ": " + parse_err;
          } else {
            std::map<std::string, size_t> pair_index;
            for (size_t i = 0; i < pairs.size(); ++i) {
              pair_index[pairs[i].key_] = i;
            }
            FileScanRow row;
            for (size_t i = 0; i < impl_->projection_.size(); ++i) {
              const int64_t index = impl_->projection_[i];
              const std::string &column_name =
                  impl_->schema_[static_cast<size_t>(index)].name_;
              std::map<std::string, size_t>::iterator it = pair_index.find(column_name);
              if (pair_index.end() == it || JsonValueKind::NUL == pairs[it->second].kind_) {
                row.values_.push_back("");
                row.nulls_.push_back(true);
              } else {
                row.values_.push_back(pairs[it->second].text_);
                row.nulls_.push_back(false);
              }
            }
            rows.push_back(row);
          }
        }
      }
    }
  }
  return ret;
}

int FileBatchScanner::reset()
{
  int ret = OB_SUCCESS;
  if (NULL == impl_) {
    ret = OB_NOT_INIT;
  } else if (FileFormatType::PARQUET == impl_->options_.format_) {
    ret = impl_->parquet_reader_->reset();
  } else if (impl_->csv_parser_ != NULL) {
    if (OB_FAIL(impl_->csv_reader_.open(impl_->path_))) {
    } else {
      impl_->csv_parser_.reset(new (std::nothrow) CsvRecordParser(impl_->csv_reader_));
      if (NULL == impl_->csv_parser_) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        impl_->csv_header_pending_ = impl_->options_.header_;
      }
    }
  } else if (impl_->json_records_ != NULL) {
    if (OB_FAIL(impl_->json_reader_.open(impl_->path_))) {
    } else {
      impl_->json_records_.reset(new (std::nothrow) JsonLineRecordReader(impl_->json_reader_));
      if (NULL == impl_->json_records_) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      }
    }
  }
  return ret;
}

void FileBatchScanner::close()
{
  if (NULL != impl_) {
    impl_->csv_reader_.close();
    impl_->json_reader_.close();
    impl_->csv_parser_.reset();
    impl_->json_records_.reset();
    if (impl_->parquet_reader_ != NULL) {
      impl_->parquet_reader_->close();
      impl_->parquet_reader_.reset();
    }
    impl_->json_index_map_.clear();
  }
}

}  // namespace sql
}  // namespace oceanbase
