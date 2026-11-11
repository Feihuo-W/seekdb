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
  auto run_stmt = [&](const std::string& s) {
    std::string out = engine.Execute(s, sink);
    if (!out.empty()) {
      if (trace_all || out.rfind("{\"error\"", 0) == 0) {
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
  // The driver protocol delivers one complete SQL statement (or directive) per
  // line, so every line is answered immediately. There is deliberately no
  // cross-line buffering: waiting for a continuation that never arrives is
  // exactly what produces a "JSON response timeout".
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
