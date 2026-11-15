#pragma once

#include <cstdint>
#include <cmath>
#include <charconv>
#include <system_error>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace vdb {

inline void JsonEscapeTo(std::string& out, const std::string& s) {
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
}

inline std::string FmtDouble(double v) {
  char buf[64];
  for (int prec = 15; prec <= 17; ++prec) {
    std::snprintf(buf, sizeof(buf), "%.*g", prec, v);
    if (std::strtod(buf, nullptr) == v) return buf;
  }
  return buf;
}

// Standard shortest-roundtrip conversion, independent of locale. Preserve
// legacy formatting fallback if the toolchain lacks floating-point to_chars.
inline void AppendRoundtripDouble(std::string& out, double value) {
#if defined(__GLIBCXX__) && __GNUC__ >= 11
  char buf[64];
  const auto result = std::to_chars(buf, buf + sizeof(buf), value, std::chars_format::general);
  if (result.ec == std::errc{}) { out.append(buf, static_cast<size_t>(result.ptr - buf)); return; }
#endif
  out += FmtDouble(value);
}

// VECTOR values are stored float32. Format the float overload directly so
// its shortest decimal round-trips to the same float32 without double expansion.
inline void AppendRoundtripFloat(std::string& out, float value) {
  // JSON consumers may parse bare -0 as integer zero and lose its sign.
  if (value == 0.0f && std::signbit(value)) { out.append("-0.0"); return; }
#if defined(__GLIBCXX__) && __GNUC__ >= 11
  char buf[64];
  const auto result = std::to_chars(buf, buf + sizeof(buf), value, std::chars_format::general);
  if (result.ec == std::errc{}) { out.append(buf, static_cast<size_t>(result.ptr - buf)); return; }
#endif
  // Keep legacy fallback for toolchains without floating-point to_chars.
  out += FmtDouble(static_cast<double>(value));
}

struct OutVal {
  enum Kind { INT, STR, FLT, VEC } kind = INT;
  int64_t i = 0;
  double f = 0;
  std::string s;
  std::vector<float> v;
};

inline void OutValToJson(std::string& out, const OutVal& v) {
  switch (v.kind) {
    case OutVal::INT: out += std::to_string(v.i); break;
    case OutVal::STR: out += '"'; JsonEscapeTo(out, v.s); out += '"'; break;
    case OutVal::FLT: AppendRoundtripDouble(out, v.f); break;
    case OutVal::VEC:
      out += '[';
      for (size_t i = 0; i < v.v.size(); ++i) {
        if (i) out += ',';
        AppendRoundtripFloat(out, v.v[i]);
      }
      out += ']';
      break;
  }
}

// Response payloads follow STDIO_PROTOCOL.md; main.cpp prepends the request
// id. Errors carry a code (checked by the judge) and a message (ignored).
inline std::string JsonError(const std::string& err) {
  std::string out = "{\"ok\":false,\"error\":{\"code\":\"";
  JsonEscapeTo(out, err);
  out += "\",\"message\":\"";
  JsonEscapeTo(out, err);
  out += "\"}}";
  return out;
}

inline std::string JsonOk() { return "{\"ok\":true}"; }

inline std::string JsonAffected(int64_t n) {
  return "{\"ok\":true,\"affected_rows\":" + std::to_string(n) + "}";
}

inline std::string JsonRows(const std::vector<std::string>& cols,
                            const std::vector<std::vector<OutVal>>& rows) {
  std::string out;
  out.reserve(64 + rows.size() * 32);
  out += "{\"ok\":true,\"columns\":[";
  for (size_t i = 0; i < cols.size(); ++i) {
    if (i) out += ',';
    out += '"';
    JsonEscapeTo(out, cols[i]);
    out += '"';
  }
  out += "],\"rows\":[";
  for (size_t r = 0; r < rows.size(); ++r) {
    if (r) out += ',';
    out += '[';
    for (size_t c = 0; c < rows[r].size(); ++c) {
      if (c) out += ',';
      OutValToJson(out, rows[r][c]);
    }
    out += ']';
  }
  out += "]}";
  return out;
}

}  // namespace vdb
