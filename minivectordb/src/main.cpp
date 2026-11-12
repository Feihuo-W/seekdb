#include <unistd.h>

#include <cerrno>
#include <sys/select.h>
#include <sys/time.h>

#include <algorithm>

#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "engine.h"
#include "json.h"

namespace {

vdb::Engine* g_engine = nullptr;

void OnSignal(int) {
  if (g_engine != nullptr) {
    g_engine->Shutdown();
    g_engine = nullptr;
  }
  _exit(0);
}

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string Trim(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return std::string();
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

// Splits a line on semicolons outside single-quoted strings.
void SplitStatements(const std::string& line, std::vector<std::string>* out) {
  std::string cur;
  char quote = 0;
  for (size_t i = 0; i < line.size(); ++i) {
    char c = line[i];
    if (quote != 0) {
      cur.push_back(c);
      if (c == '\\' && i + 1 < line.size()) {
        cur.push_back(line[++i]);
      } else if (c == quote) {
        if (i + 1 < line.size() && line[i + 1] == quote) {
          cur.push_back(line[++i]);
        } else {
          quote = 0;
        }
      }
      continue;
    }
    if (c == '-' && i + 1 < line.size() && line[i + 1] == '-') {
      while (i < line.size() && line[i] != '\n') ++i;
      continue;
    }
    if (c == '#') {
      while (i < line.size() && line[i] != '\n') ++i;
      continue;
    }
    if (c == '/' && i + 1 < line.size() && line[i + 1] == '*') {
      i += 2;
      while (i + 1 < line.size() && !(line[i] == '*' && line[i + 1] == '/')) ++i;
      ++i;
      continue;
    }
    if (c == '\'' || c == '"') {
      quote = c;
      cur.push_back(c);
      continue;
    }
    if (c == ';') {
      std::string stmt = Trim(cur);
      if (!stmt.empty()) out->push_back(std::move(stmt));
      cur.clear();
      continue;
    }
    cur.push_back(c);
  }
  std::string stmt = Trim(cur);
  if (!stmt.empty()) out->push_back(std::move(stmt));
}

// Escapes a statement for single-line stderr diagnostics.
std::string LogEscape(const std::string& s, size_t limit = 400) {
  std::string out;
  out.reserve(std::min(s.size(), limit) + 8);
  for (size_t i = 0; i < s.size() && out.size() < limit; ++i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    switch (c) {
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20 || c == 0x7f) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\x%02x", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  if (s.size() > limit) out += "...";
  return out;
}

// ---- JSONL request parsing (STDIO_PROTOCOL.md) ----

void AppendUtf8(std::string* out, uint32_t cp) {
  if (cp < 0x80) {
    out->push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

struct JsonReq {
  const char* p;
  const char* end;

  void Ws() {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
  }
  bool Lit(const char* s) {
    size_t n = std::strlen(s);
    if (static_cast<size_t>(end - p) < n || std::memcmp(p, s, n) != 0) return false;
    p += n;
    return true;
  }
  bool Hex4(uint32_t* v) {
    if (end - p < 4) return false;
    uint32_t x = 0;
    for (int i = 0; i < 4; ++i) {
      char c = *p++;
      x <<= 4;
      if (c >= '0' && c <= '9') x |= static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f') x |= static_cast<uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') x |= static_cast<uint32_t>(c - 'A' + 10);
      else return false;
    }
    *v = x;
    return true;
  }
  bool Str(std::string* out) {
    if (p >= end || *p != '"') return false;
    ++p;
    // 快路径：整段没有转义时一次性拷贝。
    // 官方负载的 SQL 基本不含反斜杠，而一条批量 INSERT 的 sql 字段可达 249 KB，
    // 逐字符 push_back 会产生几十万个字符级操作与多次扩容拷贝（实测 ~0.9 ms/请求）。
    {
      // 快路径：memchr（SIMD）定位收尾引号 + memchr 检查区间里有没有反斜杠。
      // 官方负载的 sql 字段可达 249 KB，逐字节找引号是写入期最大的一笔纯 CPU 开销。
      const char* seg = p;
      const void* hit = memchr(seg, '"', static_cast<size_t>(end - seg));
      if (hit != nullptr) {
        const char* q = static_cast<const char*>(hit);
        const void* esc = memchr(seg, '\\', static_cast<size_t>(q - seg));
        if (esc == nullptr) {
          out->assign(seg, static_cast<size_t>(q - seg));
          p = q + 1;
          return true;
        }
      }
      p = seg;  // 有转义或未闭合：回到起点走逐字符慢路径（语义完全一致）
    }
    out->clear();
    while (p < end) {
      unsigned char c = static_cast<unsigned char>(*p++);
      if (c == '"') return true;
      if (c < 0x20) return false;
      if (c != '\\') {
        out->push_back(static_cast<char>(c));
        continue;
      }
      if (p >= end) return false;
      char e = *p++;
      switch (e) {
        case '"': out->push_back('"'); break;
        case '\\': out->push_back('\\'); break;
        case '/': out->push_back('/'); break;
        case 'b': out->push_back('\b'); break;
        case 'f': out->push_back('\f'); break;
        case 'n': out->push_back('\n'); break;
        case 'r': out->push_back('\r'); break;
        case 't': out->push_back('\t'); break;
        case 'u': {
          uint32_t cp = 0;
          if (!Hex4(&cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (end - p < 6 || p[0] != '\\' || p[1] != 'u') return false;
            p += 2;
            uint32_t lo = 0;
            if (!Hex4(&lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return false;
          }
          AppendUtf8(out, cp);
          break;
        }
        default: return false;
      }
    }
    return false;
  }
  // Skips any value; when raw != nullptr, captures its exact source text so
  // the id can be echoed back with its original JSON type.
  bool Skip(std::string* raw) {
    Ws();
    const char* start = p;
    if (p >= end) return false;
    char c = *p;
    if (c == '"') {
      std::string tmp;
      if (!Str(&tmp)) return false;
    } else if (c == '{' || c == '[') {
      char close = (c == '{') ? '}' : ']';
      ++p;
      int depth = 1;
      while (p < end && depth > 0) {
        char d = *p++;
        if (d == '"') {
          --p;
          std::string tmp;
          if (!Str(&tmp)) return false;
        } else if (d == c) {
          ++depth;
        } else if (d == close) {
          --depth;
        }
      }
      if (depth != 0) return false;
    } else if (c == 't') {
      if (!Lit("true")) return false;
    } else if (c == 'f') {
      if (!Lit("false")) return false;
    } else if (c == 'n') {
      if (!Lit("null")) return false;
    } else if (c == '-' || (c >= '0' && c <= '9')) {
      if (c == '-') ++p;
      if (p >= end || *p < '0' || *p > '9') return false;
      while (p < end && *p >= '0' && *p <= '9') ++p;
      if (p < end && *p == '.') {
        ++p;
        if (p >= end || *p < '0' || *p > '9') return false;
        while (p < end && *p >= '0' && *p <= '9') ++p;
      }
      if (p < end && (*p == 'e' || *p == 'E')) {
        ++p;
        if (p < end && (*p == '+' || *p == '-')) ++p;
        if (p >= end || *p < '0' || *p > '9') return false;
        while (p < end && *p >= '0' && *p <= '9') ++p;
      }
    } else {
      return false;
    }
    if (raw != nullptr) raw->assign(start, static_cast<size_t>(p - start));
    return true;
  }
};

bool ParseRequest(const std::string& line, std::string* idraw, std::string* op,
                  std::string* sql, std::string* mode) {
  JsonReq ps{line.data(), line.data() + line.size()};
  ps.Ws();
  if (!ps.Lit("{")) return false;
  ps.Ws();
  if (!ps.Lit("}")) {
    for (;;) {
      ps.Ws();
      std::string key;
      if (!ps.Str(&key)) return false;
      ps.Ws();
      if (!ps.Lit(":")) return false;
      ps.Ws();
      if (key == "id") {
        if (!ps.Skip(idraw)) return false;
      } else if (key == "op") {
        if (!ps.Str(op)) return false;
      } else if (key == "sql") {
        if (!ps.Str(sql)) return false;
      } else if (key == "search_mode") {
        if (!ps.Str(mode)) return false;
      } else {
        if (!ps.Skip(nullptr)) return false;
      }
      ps.Ws();
      if (ps.Lit("}")) break;
      if (!ps.Lit(",")) return false;
    }
  }
  ps.Ws();
  return ps.p == ps.end;
}


// Reads newline-delimited input with an optional idle timeout. A timeout lets
// the caller answer a statement that will never complete instead of hanging.
class LineReader {
 public:
  explicit LineReader(int fd) : fd_(fd) {}

  // 1 = line produced, 0 = idle timeout, -1 = EOF
  int ReadLine(std::string* out, int timeout_ms) {
    for (;;) {
      size_t nl = buf_.find('\n');
      if (nl != std::string::npos) {
        *out = buf_.substr(0, nl);
        buf_.erase(0, nl + 1);
        return 1;
      }
      fd_set rfds;
      FD_ZERO(&rfds);
      FD_SET(fd_, &rfds);
      struct timeval tv;
      tv.tv_sec = timeout_ms < 0 ? 0 : timeout_ms / 1000;
      tv.tv_usec = timeout_ms < 0 ? 0 : (timeout_ms % 1000) * 1000;
      int r = select(fd_ + 1, &rfds, nullptr, nullptr, timeout_ms < 0 ? nullptr : &tv);
      if (r < 0) {
        if (errno == EINTR) continue;
        return -1;
      }
      if (r == 0) return 0;
      char chunk[65536];
      ssize_t n = ::read(fd_, chunk, sizeof(chunk));
      if (n == 0) {
        if (buf_.empty()) return -1;
        *out = buf_;
        buf_.clear();
        return 1;
      }
      if (n < 0) {
        if (errno == EINTR) continue;
        return -1;
      }
      buf_.append(chunk, static_cast<size_t>(n));
    }
  }

 private:
  int fd_;
  std::string buf_;
};


}  // namespace

int main(int argc, char** argv) {
  std::ios::sync_with_stdio(false);
  std::string dir = "/data";
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--data" && i + 1 < argc) {
      dir = argv[++i];
    } else if (a.rfind("--data=", 0) == 0) {
      dir = a.substr(7);
    }
  }
  if (const char* env = std::getenv("MINIVECTORDB_DATA")) {
    if (*env) dir = env;
  }

  vdb::Engine engine(dir);
  if (!engine.Ok()) {
    std::fprintf(stderr, "minivectordb: failed to open data dir %s\n", dir.c_str());
    return 1;
  }
  g_engine = &engine;
  std::signal(SIGTERM, OnSignal);
  std::signal(SIGINT, OnSignal);
  std::signal(SIGPIPE, SIG_IGN);

  std::fprintf(stderr, "minivectordb: ready (%s)\n", dir.c_str());
  std::fflush(stderr);
  // STDIO_PROTOCOL.md handshake: exactly this object on stdout, flushed,
  // after recovery and before reading the first request.
  std::cout << "{\"type\":\"ready\"}\n";
  std::cout.flush();

  const bool trace_all = std::getenv("MVDB_TRACE") != nullptr;
  // Query results are streamed through this buffer so a result larger than RAM
  // never has to be materialized as one string.
  std::string obuf;
  obuf.reserve(1 << 16);
  auto sink = [&](const char* p, size_t n) {
    obuf.append(p, n);
    if (obuf.size() >= (1 << 16)) {
      std::cout.write(obuf.data(), static_cast<std::streamsize>(obuf.size()));
      obuf.clear();
    }
  };
  auto finish = [&]() {
    if (!obuf.empty()) {
      std::cout.write(obuf.data(), static_cast<std::streamsize>(obuf.size()));
      obuf.clear();
    }
    std::cout.put('\n');
    std::cout.flush();
  };
  auto emit = [&](const std::string& out) {
    sink(out.data(), out.size());
    finish();
  };
  // JSONL execute: the engine payload starts with '{' and carries ok/error;
  // the echoed request id is spliced in right after the opening brace.
  // Streaming (sink) and a returned payload are mutually exclusive.
  auto run_json = [&](const std::string& sql, const std::string& idraw) {
    const std::string prefix = "{\"id\":" + idraw + ",";
    bool started = false;
    auto esink = [&](const char* p, size_t n) {
      if (!started) {
        started = true;
        obuf.append(prefix);
        obuf.append(p + 1, n - 1);
      } else {
        obuf.append(p, n);
      }
      if (obuf.size() >= (1 << 16)) {
        std::cout.write(obuf.data(), static_cast<std::streamsize>(obuf.size()));
        obuf.clear();
      }
    };
    std::string out = engine.Execute(sql, esink);
    if (!out.empty()) {
      obuf.append(prefix);
      obuf.append(out.data() + 1, out.size() - 1);
      started = true;
    }
    if (!started) {
      obuf.append("{\"id\":").append(idraw).append(",\"ok\":true}");
    }
    if (trace_all) {
      std::fprintf(stderr, "req id=%s sql=<%s>\n", idraw.c_str(), LogEscape(sql).c_str());
      std::fflush(stderr);
    }
    finish();
  };
  auto run_stmt = [&](const std::string& s) {
    std::string out = engine.Execute(s, sink);
    if (!out.empty()) {
      if (trace_all || out.rfind("{\"ok\":false", 0) == 0) {
        std::fprintf(stderr, "stmt input=<%s> resp=<%s>\n", LogEscape(s).c_str(),
                     LogEscape(out, 200).c_str());
        std::fflush(stderr);
      }
      emit(out);
    } else {
      if (trace_all) {
        std::fprintf(stderr, "stmt input=<%s> resp=<streamed>\n", LogEscape(s).c_str());
        std::fflush(stderr);
      }
      finish();
    }
  };
  auto run_stmts = [&](const std::string& text) {
    std::vector<std::string> stmts;
    SplitStatements(text, &stmts);
    if (stmts.empty()) {
      // A line that carries no statement (e.g. ";" or a lone comment) still
      // gets exactly one answer so the driver never waits forever.
      std::string out = vdb::JsonError("syntax_error");
      if (trace_all) {
        std::fprintf(stderr, "stmt input=<%s> resp=<%s>\n", LogEscape(text).c_str(),
                     out.c_str());
        std::fflush(stderr);
      }
      emit(out);
      return;
    }
    for (const auto& s : stmts) run_stmt(s);
  };

  std::string line;
  bool first_line = true;
  LineReader reader(STDIN_FILENO);
  // STDIO_PROTOCOL.md: one JSON request per line, answered immediately with
  // exactly one response. There is deliberately no cross-line buffering:
  // waiting for a continuation that never arrives is exactly what produces a
  // "JSON response timeout".
  for (;;) {
    int st = reader.ReadLine(&line, -1);
    if (st < 0) break;
    if (first_line) {
      first_line = false;
      if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
          static_cast<unsigned char>(line[1]) == 0xBB &&
          static_cast<unsigned char>(line[2]) == 0xBF) {
        line.erase(0, 3);
      }
    }
    std::string stmt = Trim(line);
    if (stmt.empty()) continue;
    if (stmt[0] == '{') {
      std::string idraw = "null";
      std::string op;
      std::string sql;
      std::string mode = "exact";
      if (!ParseRequest(stmt, &idraw, &op, &sql, &mode)) {
        emit("{\"id\":" + idraw +
             ",\"ok\":false,\"error\":{\"code\":\"invalid_request\","
             "\"message\":\"malformed JSON request\"}}");
        continue;
      }
      if (op == "shutdown") {
        engine.Shutdown();
        g_engine = nullptr;
        emit("{\"id\":" + idraw + ",\"ok\":true}");
        return 0;
      }
      if (op != "execute") {
        emit("{\"id\":" + idraw +
             ",\"ok\":false,\"error\":{\"code\":\"unsupported_op\","
             "\"message\":\"unsupported op\"}}");
        continue;
      }
      engine.SetAnnMode(mode == "ann");
      run_json(sql, idraw);
      continue;
    }
    // Legacy raw-SQL lines (local self-test tools); the official judge only
    // sends JSONL requests, handled above.
    if (stmt[0] == '#') continue;
    if (stmt.rfind("--", 0) == 0) {
      std::string rest = Lower(Trim(stmt.substr(2)));
      while (!rest.empty() && rest.back() == ';') rest = Trim(rest.substr(0, rest.size() - 1));
      std::string word = rest.substr(0, rest.find_first_of(" \t"));
      if (word == "restart") {
        std::fprintf(stderr, "directive restart\n");
        std::fflush(stderr);
        break;
      }
      if (word == "mode") {
        std::string mode = Trim(rest.substr(4));
        if (!mode.empty() && mode[0] == '=') mode = Trim(mode.substr(1));
        mode = mode.substr(0, mode.find_first_of(" \t"));
        engine.SetAnnMode(mode == "ann");
        std::fprintf(stderr, "directive mode %s\n", mode.c_str());
        std::fflush(stderr);
        continue;
      }
      continue;
    }
    std::string low = Lower(stmt);
    if (low == "quit" || low == "exit") break;
    run_stmts(stmt);
  }

  engine.Shutdown();
  g_engine = nullptr;
  return 0;
}
