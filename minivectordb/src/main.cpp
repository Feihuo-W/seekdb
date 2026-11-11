#include <unistd.h>

#include <cerrno>
#include <chrono>
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

int Balance(const std::string& s) {
  char quote = 0;
  bool in_tick = false;
  int depth = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (quote != 0) {
      if (c == '\\' && i + 1 < s.size()) {
        ++i;
      } else if (c == quote) {
        if (i + 1 < s.size() && s[i + 1] == quote) {
          ++i;
        } else {
          quote = 0;
        }
      }
      continue;
    }
    if (in_tick) {
      if (c == '`') in_tick = false;
      continue;
    }
    if (c == '-' && i + 1 < s.size() && s[i + 1] == '-') {
      while (i < s.size() && s[i] != '\n') ++i;
      continue;
    }
    if (c == '#') {
      while (i < s.size() && s[i] != '\n') ++i;
      continue;
    }
    if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
      i += 2;
      while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) ++i;
      ++i;
      continue;
    }
    if (c == '\'' || c == '"') {
      quote = c;
    } else if (c == '`') {
      in_tick = true;
    } else if (c == '(') {
      ++depth;
    } else if (c == ')') {
      if (--depth < 0) return -1;
    }
  }
  return (quote != 0 || in_tick || depth > 0) ? 1 : 0;
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


// Delay before answering a driver directive that expects a response. The answer
// is only emitted when the driver stays idle, so pipelining drivers are not
// disturbed.
int DirectiveAckDelayMs() {
  static const int v = []() {
    const char* e = std::getenv("MINIVECTORDB_ACK_MS");
    return e ? std::atoi(e) : 1200;
  }();
  return v;
}

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
  auto run_stmt = [&](const std::string& s) {
    std::string out = engine.Execute(s);
    if (trace_all || out.rfind("{\"error\"", 0) == 0) {
      std::fprintf(stderr, "stmt input=<%s> resp=<%s>\n", LogEscape(s).c_str(),
                   LogEscape(out, 200).c_str());
      std::fflush(stderr);
    }
    std::cout << out << '\n';
    std::cout.flush();
  };

  std::string line;
  std::string pending;
  bool stop = false;
  bool first_line = true;
  LineReader reader(STDIN_FILENO);
  auto flush_pending = [&]() {
    if (pending.empty()) return;
    std::vector<std::string> stmts;
    SplitStatements(pending, &stmts);
    pending.clear();
    for (const auto& s : stmts) run_stmt(s);
  };
  auto ack_deadline = std::chrono::steady_clock::time_point::max();
  while (!stop) {
    // When a statement is pending, wait only briefly for a continuation line so
    // that an unterminated statement still produces an answer instead of a hang.
    int timeout = -1;
    if (!pending.empty()) {
      timeout = 300;
    } else if (ack_deadline != std::chrono::steady_clock::time_point::max()) {
      auto now = std::chrono::steady_clock::now();
      if (now >= ack_deadline) {
        ack_deadline = std::chrono::steady_clock::time_point::max();
        std::cout << "{}\n";
        std::cout.flush();
        std::fprintf(stderr, "directive ack emitted\n");
        std::fflush(stderr);
        continue;
      }
      timeout = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    ack_deadline - now).count());
      if (timeout < 0) timeout = 0;
    }
    int st = reader.ReadLine(&line, timeout);
    if (st < 0) break;
    if (st == 0) {
      if (!pending.empty()) flush_pending();
      continue;
    }
    ack_deadline = std::chrono::steady_clock::time_point::max();
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
    if (pending.empty() && stmt[0] == '#') continue;
    if (pending.empty() && stmt.rfind("--", 0) == 0) {
      std::string rest = Lower(Trim(stmt.substr(2)));
      while (!rest.empty() && rest.back() == ';') rest = Trim(rest.substr(0, rest.size() - 1));
      if (rest == "restart") {
        std::fprintf(stderr, "directive restart\n");
        std::fflush(stderr);
        break;
      }
      if (rest.rfind("mode", 0) == 0) {
        std::string mode = Trim(rest.substr(4));
        if (!mode.empty() && mode[0] == '=') mode = Trim(mode.substr(1));
        engine.SetAnnMode(mode == "ann");
        std::fprintf(stderr, "directive mode %s\n", mode.c_str());
        std::fflush(stderr);
        ack_deadline = DirectiveAckDelayMs() > 0
                           ? std::chrono::steady_clock::now() +
                                 std::chrono::milliseconds(DirectiveAckDelayMs())
                           : std::chrono::steady_clock::time_point::max();
        continue;
      }
      continue;
    }
    std::string low = Lower(stmt);
    if (pending.empty() && (low == "quit" || low == "exit")) break;

    pending += pending.empty() ? stmt : " " + stmt;
    if (Balance(pending) == 1) continue;

    std::vector<std::string> stmts;
    SplitStatements(pending, &stmts);
    pending.clear();
    for (const auto& s : stmts) run_stmt(s);
  }
  flush_pending();

  engine.Shutdown();
  g_engine = nullptr;
  return 0;
}
