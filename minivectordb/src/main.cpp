#include <unistd.h>

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

// Returns 0 when quotes/parens are balanced, 1 when more input is needed,
// -1 when a closing paren appears without an opener.
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
  while (!stop && std::getline(std::cin, line)) {
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
  if (!pending.empty()) {
    std::vector<std::string> stmts;
    SplitStatements(pending, &stmts);
    for (const auto& s : stmts) run_stmt(s);
  }

  engine.Shutdown();
  g_engine = nullptr;
  return 0;
}
