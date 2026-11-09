#include <unistd.h>

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
  bool in_str = false;
  for (size_t i = 0; i < line.size(); ++i) {
    char c = line[i];
    if (in_str) {
      cur.push_back(c);
      if (c == '\\' && i + 1 < line.size()) {
        cur.push_back(line[++i]);
      } else if (c == '\'') {
        if (i + 1 < line.size() && line[i + 1] == '\'') {
          cur.push_back(line[++i]);
        } else {
          in_str = false;
        }
      }
      continue;
    }
    if (c == '\'') {
      in_str = true;
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

  std::string line;
  bool stop = false;
  while (!stop && std::getline(std::cin, line)) {
    std::string stmt = Trim(line);
    if (stmt.empty()) continue;
    if (stmt.rfind("--", 0) == 0) {
      std::string rest = Lower(Trim(stmt.substr(2)));
      if (rest == "restart") break;
      if (rest.rfind("mode", 0) == 0) {
        std::string mode = Trim(rest.substr(4));
        engine.SetAnnMode(mode == "ann");
        continue;
      }
      continue;
    }
    std::string low = Lower(stmt);
    if (low == "quit" || low == "exit") break;

    std::vector<std::string> stmts;
    SplitStatements(stmt, &stmts);
    for (const auto& s : stmts) {
      std::string out = engine.Execute(s);
      std::cout << out << '\n';
      std::cout.flush();
    }
  }

  engine.Shutdown();
  g_engine = nullptr;
  return 0;
}
