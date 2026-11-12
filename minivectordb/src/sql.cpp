#include "sql.h"

#include "fastfloat.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace vdb {
namespace {

struct ParseFail {
  std::string name;
};

[[noreturn]] void Fail(const char* name) { throw ParseFail{name}; }

struct Token {
  enum Kind { Ident, Number, String, Punct, Vec, End } kind = End;
  std::string text;  // Ident/Punct text, String payload
  bool is_int = false;
  int64_t inum = 0;
  double num = 0;
  std::vector<double> nums;
};

std::string Lower(const std::string& s) {
  std::string r = s;
  for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return r;
}

bool IsIdentStart(char c) {
  return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}
bool IsIdentChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

class Lexer {
 public:
  explicit Lexer(const std::string& s) : s_(s) {}

  std::vector<Token> Run() {
    std::vector<Token> out;
    while (true) {
      SkipSpace();
      if (pos_ >= s_.size()) break;
      char c = s_[pos_];
      if (c == '-' && pos_ + 1 < s_.size() && s_[pos_ + 1] == '-') {
        while (pos_ < s_.size() && s_[pos_] != '\n') ++pos_;
        continue;
      }
      if (c == '#') {
        while (pos_ < s_.size() && s_[pos_] != '\n') ++pos_;
        continue;
      }
      if (c == '/' && pos_ + 1 < s_.size() && s_[pos_ + 1] == '*') {
        pos_ += 2;
        while (pos_ + 1 < s_.size() && !(s_[pos_] == '*' && s_[pos_ + 1] == '/')) ++pos_;
        pos_ = (pos_ + 1 < s_.size()) ? pos_ + 2 : pos_;
        continue;
      }
      if (IsIdentStart(c) || c == '`') {
        out.push_back(Ident());
      } else if (std::isdigit(static_cast<unsigned char>(c)) ||
                 ((c == '-' || c == '+') && pos_ + 1 < s_.size() &&
                  std::isdigit(static_cast<unsigned char>(s_[pos_ + 1]))) ||
                 (c == '.' && pos_ + 1 < s_.size() &&
                  std::isdigit(static_cast<unsigned char>(s_[pos_ + 1])))) {
        out.push_back(Number());
      } else if (c == '\'' || c == '"') {
        out.push_back(String(c));
      } else if (c == '[') {
        out.push_back(VectorOrPunct());
      } else {
        out.push_back(Punct());
      }
    }
    Token end;
    end.kind = Token::End;
    out.push_back(end);
    return out;
  }

 private:
  void SkipSpace() {
    while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_;
  }

  Token Ident() {
    if (!s_.empty() && s_[pos_] == '`') {
      ++pos_;
      std::string out;
      while (pos_ < s_.size()) {
        char c = s_[pos_];
        if (c == '`') {
          if (pos_ + 1 < s_.size() && s_[pos_ + 1] == '`') {
            out.push_back('`');
            pos_ += 2;
            continue;
          }
          ++pos_;
          Token t;
          t.kind = Token::Ident;
          t.text = std::move(out);
          return t;
        }
        out.push_back(c);
        ++pos_;
      }
      Fail("syntax_error");
    }
    size_t b = pos_;
    while (pos_ < s_.size() && IsIdentChar(s_[pos_])) ++pos_;
    Token t;
    t.kind = Token::Ident;
    t.text = s_.substr(b, pos_ - b);
    return t;
  }

  Token Number() {
    size_t b = pos_;
    if (s_[pos_] == '-' || s_[pos_] == '+') ++pos_;
    bool is_float = false;
    while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
    if (pos_ < s_.size() && s_[pos_] == '.') {
      is_float = true;
      ++pos_;
      while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
    }
    if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
      size_t save = pos_;
      ++pos_;
      if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) ++pos_;
      if (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) {
        is_float = true;
        while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
      } else {
        pos_ = save;
      }
    }
    std::string text = s_.substr(b, pos_ - b);
    Token t;
    t.kind = Token::Number;
    t.text = text;
    if (!is_float) {
      errno = 0;
      char* endp = nullptr;
      long long v = std::strtoll(text.c_str(), &endp, 10);
      if (errno == ERANGE || endp != text.c_str() + text.size()) Fail("value_out_of_range");
      t.is_int = true;
      t.inum = v;
      t.num = static_cast<double>(v);
    } else {
      t.is_int = false;
      const char* next = nullptr;
      if (!FastParseDouble(text.c_str(), text.c_str() + text.size(), &t.num, &next)) {
        t.num = std::strtod(text.c_str(), nullptr);
      }
    }
    return t;
  }

  Token String(char quote) {
    ++pos_;  // opening quote
    // 快路径：整段没有转义、没有重复引号时，一次性拷贝。
    // 官方负载里一条批量 INSERT 只有一个约 249 KB 的字符串字面量，
    // 逐字符 push_back 会产生十几万次字符级操作与多次扩容拷贝
    // （火焰图实测：写入期 CPU 的 8% 花在 String 的 push_back 上）。
    {
      // 快路径：memchr 定位收尾引号 + memchr 检查区间有无反斜杠；
      // 连续两个引号（转义引号）也交给慢路径。
      const char* base = s_.data();
      const size_t total = s_.size();
      const char* scan = base + pos_;
      const void* hit = memchr(scan, quote, total - pos_);
      if (hit != nullptr) {
        const char* q = static_cast<const char*>(hit);
        const void* esc = memchr(scan, '\\', static_cast<size_t>(q - scan));
        const bool doubled = (q + 1 < base + total) && q[1] == quote;
        if (esc == nullptr && !doubled) {
          Token t;
          t.kind = Token::String;
          t.text.assign(scan, static_cast<size_t>(q - scan));
          pos_ = static_cast<size_t>(q - base) + 1;
          return t;
        }
      }
    }
    std::string out;
    while (pos_ < s_.size()) {
      char c = s_[pos_];
      if (c == quote) {
        if (pos_ + 1 < s_.size() && s_[pos_ + 1] == quote) {
          out.push_back(quote);
          pos_ += 2;
          continue;
        }
        ++pos_;
        Token t;
        t.kind = Token::String;
        t.text = std::move(out);
        return t;
      }
      if (c == '\\' && pos_ + 1 < s_.size()) {
        char e = s_[pos_ + 1];
        pos_ += 2;
        switch (e) {
          case 'n': out.push_back('\n'); break;
          case 't': out.push_back('\t'); break;
          case 'r': out.push_back('\r'); break;
          case '0': out.push_back('\0'); break;
          case 'b': out.push_back('\b'); break;
          default: out.push_back(e); break;
        }
        continue;
      }
      out.push_back(c);
      ++pos_;
    }
    Fail("syntax_error");
  }

  Token Punct() {
    Token t;
    t.kind = Token::Punct;
    if (pos_ + 1 < s_.size()) {
      std::string two = s_.substr(pos_, 2);
      if (two == "<=" || two == ">=" || two == "<>" || two == "!=" || two == "==") {
        t.text = two;
        pos_ += 2;
        return t;
      }
    }
    t.text = std::string(1, s_[pos_]);
    ++pos_;
    return t;
  }

  Token VectorOrPunct() {
    size_t save = pos_;
    ++pos_;  // '['
    Token t;
    t.kind = Token::Vec;
    std::vector<double> nums;
    SkipSpace();
    if (pos_ < s_.size() && s_[pos_] == ']') {
      ++pos_;
      t.nums = std::move(nums);
      return t;
    }
    while (true) {
      SkipSpace();
      if (pos_ >= s_.size()) break;
      char c = s_[pos_];
      if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '+' || c == '.')) break;
      size_t b = pos_;
      if (c == '-' || c == '+') ++pos_;
      bool any = false;
      while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) { ++pos_; any = true; }
      if (pos_ < s_.size() && s_[pos_] == '.') {
        ++pos_;
        while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) { ++pos_; any = true; }
      }
      if (!any) break;
      if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
        size_t save2 = pos_;
        ++pos_;
        if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) ++pos_;
        if (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) {
          while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
        } else {
          pos_ = save2;
        }
      }
      std::string text = s_.substr(b, pos_ - b);
      double v = 0;
      const char* next = nullptr;
      if (!FastParseDouble(text.c_str(), text.c_str() + text.size(), &v, &next)) {
        v = std::strtod(text.c_str(), nullptr);
      }
      nums.push_back(v);
      SkipSpace();
      if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
      break;
    }
    SkipSpace();
    if (pos_ < s_.size() && s_[pos_] == ']' && !nums.empty()) {
      ++pos_;
      t.nums = std::move(nums);
      return t;
    }
    pos_ = save;
    Token p;
    p.kind = Token::Punct;
    p.text = "[";
    ++pos_;
    return p;
  }

  const std::string& s_;
  size_t pos_ = 0;
};

class Parser {
 public:
  explicit Parser(std::vector<Token> toks) : t_(std::move(toks)) {}

  Statement ParseStatement() {
    Statement st;
    if (Peek().kind == Token::End) return st;
    if (Peek().kind != Token::Ident) Fail("syntax_error");
    std::string kw = Lower(Peek().text);
    if (kw == "create") return ParseCreate(st);
    if (kw == "insert") return ParseInsert(st);
    if (kw == "select") return ParseSelect(st);
    if (kw == "update") return ParseUpdate(st);
    if (kw == "delete") return ParseDelete(st);
    if (kw == "begin") { Next(); st.kind = StmtKind::Begin; return st; }
    if (kw == "commit") { Next(); st.kind = StmtKind::Commit; return st; }
    if (kw == "rollback") { Next(); st.kind = StmtKind::Rollback; return st; }
    if (kw == "start") {
      Next();
      ExpectKw("transaction");
      st.kind = StmtKind::Begin;
      return st;
    }
    Fail("unsupported_sql");
  }

 private:
  const Token& Peek(size_t off = 0) const { return t_[std::min(pos_ + off, t_.size() - 1)]; }
  const Token& Next() { return t_[std::min(pos_++, t_.size() - 1)]; }

  bool PeekKw(const char* kw) const {
    return Peek().kind == Token::Ident && Lower(Peek().text) == kw;
  }
  void ExpectKw(const char* kw) {
    if (!PeekKw(kw)) Fail("syntax_error");
    Next();
  }
  std::string ExpectIdent() {
    if (Peek().kind != Token::Ident) Fail("syntax_error");
    return Next().text;
  }
  void ExpectPunct(const char* p) {
    if (Peek().kind != Token::Punct || Peek().text != p) Fail("syntax_error");
    Next();
  }
  bool AcceptPunct(const char* p) {
    if (Peek().kind == Token::Punct && Peek().text == p) { Next(); return true; }
    return false;
  }
  int64_t ExpectInt() {
    if (Peek().kind != Token::Number || !Peek().is_int) Fail("syntax_error");
    return Next().inum;
  }
  void ExpectEnd() {
    // The JSONL protocol delivers the statement with its terminating ';'.
    if (Peek().kind == Token::Punct && Peek().text == ";") Next();
    if (Peek().kind != Token::End) Fail("syntax_error");
  }

  Literal ParseLiteral() {
    Literal lit;
    const Token& t = Peek();
    if (t.kind == Token::Number) {
      if (!t.is_int) Fail("value_out_of_range");
      lit.kind = LitKind::Int;
      lit.i = Next().inum;
      return lit;
    }
    if (t.kind == Token::String) {
      lit.kind = LitKind::Str;
      lit.s = Next().text;
      return lit;
    }
    if (t.kind == Token::Vec) {
      lit.kind = LitKind::Vec;
      lit.nums = Next().nums;
      return lit;
    }
    if (t.kind == Token::Ident) {
      std::string v = Lower(t.text);
      if (v == "null" || v == "true" || v == "false") Fail("unsupported_sql");
    }
    Fail("syntax_error");
  }

  Statement ParseCreate(Statement st) {
    ExpectKw("create");
    if (PeekKw("table")) {
      Next();
      return ParseCreateTable(st);
    }
    if (PeekKw("vector")) {
      Next();
      ExpectKw("index");
      st.kind = StmtKind::CreateVectorIndex;
      st.index_name = ExpectIdent();
      ExpectKw("on");
      st.table = ExpectIdent();
      ExpectPunct("(");
      st.index_col = ExpectIdent();
      ExpectPunct(")");
      ExpectEnd();
      return st;
    }
    Fail("unsupported_sql");
  }

  Statement ParseCreateTable(Statement st) {
    st.kind = StmtKind::CreateTable;
    if (PeekKw("if")) {
      Next();
      ExpectKw("not");
      ExpectKw("exists");
      st.if_not_exists = true;
    }
    st.table = ExpectIdent();
    ExpectPunct("(");
    bool have_pk = false;
    std::string table_pk;
    while (true) {
      if (PeekKw("primary")) {
        Next();
        ExpectKw("key");
        ExpectPunct("(");
        table_pk = ExpectIdent();
        ExpectPunct(")");
        if (AcceptPunct(",")) continue;
        break;
      }
      if (PeekKw("constraint")) {
        Next();
        if (Peek().kind == Token::Ident) Next();
        ExpectKw("primary");
        ExpectKw("key");
        ExpectPunct("(");
        table_pk = ExpectIdent();
        ExpectPunct(")");
        if (AcceptPunct(",")) continue;
        break;
      }
      ColumnDef def;
      def.name = ExpectIdent();
      std::string type = Lower(ExpectIdent());
      if (type == "bigint") {
        def.type.kind = ColumnType::BIGINT;
        // optional display width, e.g. BIGINT(20)
        if (AcceptPunct("(")) { ExpectInt(); ExpectPunct(")"); }
      } else if (type == "varchar") {
        def.type.kind = ColumnType::VARCHAR;
        ExpectPunct("(");
        int64_t n = ExpectInt();
        if (n <= 0 || n > 100000000) Fail("value_out_of_range");
        def.type.param = static_cast<uint32_t>(n);
        ExpectPunct(")");
      } else if (type == "vector") {
        def.type.kind = ColumnType::VECTOR;
        ExpectPunct("(");
        int64_t n = ExpectInt();
        if (n <= 0 || n > 65536) Fail("value_out_of_range");
        def.type.param = static_cast<uint32_t>(n);
        ExpectPunct(")");
      } else {
        Fail("unsupported_sql");
      }
      while (true) {
        if (PeekKw("primary")) {
          Next();
          ExpectKw("key");
          if (have_pk) Fail("syntax_error");
          def.primary = true;
          have_pk = true;
          continue;
        }
        if (PeekKw("not")) {
          Next();
          ExpectKw("null");
          continue;
        }
        if (PeekKw("null") || PeekKw("unique")) {
          Next();
          continue;
        }
        break;
      }
      st.columns.push_back(std::move(def));
      if (AcceptPunct(",")) continue;
      break;
    }
    ExpectPunct(")");
    if (!table_pk.empty()) {
      if (have_pk) Fail("syntax_error");
      std::string want;
      for (char c : table_pk) want.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
      bool found = false;
      for (auto& def : st.columns) {
        std::string have;
        for (char c : def.name) have.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        if (have == want) {
          def.primary = true;
          found = true;
        }
      }
      if (!found) Fail("unknown_column");
    }
    ExpectEnd();
    return st;
  }

  Statement ParseInsert(Statement st) {
    ExpectKw("insert");
    ExpectKw("into");
    st.kind = StmtKind::Insert;
    st.table = ExpectIdent();
    if (AcceptPunct("(")) {
      while (true) {
        st.insert_cols.push_back(ExpectIdent());
        if (AcceptPunct(",")) continue;
        break;
      }
      ExpectPunct(")");
    }
    ExpectKw("values");
    while (true) {
      ExpectPunct("(");
      std::vector<Literal> row;
      while (true) {
        row.push_back(ParseLiteral());
        if (AcceptPunct(",")) continue;
        break;
      }
      ExpectPunct(")");
      st.rows.push_back(std::move(row));
      if (AcceptPunct(",")) continue;
      break;
    }
    ExpectEnd();
    return st;
  }

  Statement ParseSelect(Statement st) {
    ExpectKw("select");
    st.kind = StmtKind::Select;
    while (true) {
      SelectItem item;
      if (AcceptPunct("*")) {
        item.star = true;
      } else {
        std::string name = ExpectIdent();
        if (AcceptPunct("(")) {
          if (Lower(name) != "l2_distance") Fail("unsupported_sql");
          item.is_distance = true;
          item.col = ExpectIdent();
          ExpectPunct(",");
          item.qvec = ParseLiteral();
          ExpectPunct(")");
        } else {
          item.col = name;
        }
        if (PeekKw("as")) {
          Next();
          item.alias = ExpectIdent();
        } else if (Peek().kind == Token::Ident) {
          std::string next = Lower(Peek().text);
          if (next != "from" && next != "where" && next != "order" && next != "limit" &&
              next != "and" && next != "asc" && next != "desc") {
            item.alias = Next().text;
          }
        }
      }
      st.select_items.push_back(std::move(item));
      if (AcceptPunct(",")) continue;
      break;
    }
    ExpectKw("from");
    st.table = ExpectIdent();
    if (PeekKw("where")) {
      Next();
      ParseWhere(st.where);
    }
    if (PeekKw("order")) {
      Next();
      ExpectKw("by");
      while (true) {
        OrderItem oi;
        std::string nm = ExpectIdent();
        if (AcceptPunct("(")) {
          if (Lower(nm) != "l2_distance") Fail("unsupported_sql");
          oi.is_distance = true;
          oi.col = ExpectIdent();
          ExpectPunct(",");
          oi.qvec = ParseLiteral();
          ExpectPunct(")");
        } else {
          oi.name = nm;
        }
        if (PeekKw("asc")) { Next(); }
        else if (PeekKw("desc")) { Next(); oi.desc = true; }
        st.order.push_back(std::move(oi));
        if (AcceptPunct(",")) continue;
        break;
      }
    }
    if (PeekKw("limit")) {
      Next();
      int64_t n = ExpectInt();
      if (n < 0) Fail("value_out_of_range");
      st.limit = n;
    }
    ExpectEnd();
    return st;
  }

  Statement ParseUpdate(Statement st) {
    ExpectKw("update");
    st.kind = StmtKind::Update;
    st.table = ExpectIdent();
    ExpectKw("set");
    while (true) {
      std::string col = ExpectIdent();
      ExpectPunct("=");
      Literal lit = ParseLiteral();
      st.sets.emplace_back(std::move(col), std::move(lit));
      if (AcceptPunct(",")) continue;
      break;
    }
    if (PeekKw("where")) {
      Next();
      ParseWhere(st.where);
    }
    ExpectEnd();
    return st;
  }

  Statement ParseDelete(Statement st) {
    ExpectKw("delete");
    ExpectKw("from");
    st.kind = StmtKind::Delete;
    st.table = ExpectIdent();
    if (PeekKw("where")) {
      Next();
      ParseWhere(st.where);
    }
    ExpectEnd();
    return st;
  }

  void ParseWhere(std::vector<Condition>& out) { ParseWhereGroup(out); }

  void ParseWhereGroup(std::vector<Condition>& out) {
    while (true) {
      if (AcceptPunct("(")) {
        ParseWhereGroup(out);
        ExpectPunct(")");
      } else {
        Condition c;
        c.col = ExpectIdent();
        const Token& t = Peek();
        if (t.kind != Token::Punct) Fail("syntax_error");
        if (t.text == "=") c.op = CmpOp::EQ;
        else if (t.text == "<") c.op = CmpOp::LT;
        else if (t.text == "<=") c.op = CmpOp::LE;
        else if (t.text == ">") c.op = CmpOp::GT;
        else if (t.text == ">=") c.op = CmpOp::GE;
        else Fail("syntax_error");
        Next();
        c.lit = ParseLiteral();
        out.push_back(std::move(c));
      }
      if (PeekKw("and")) { Next(); continue; }
      break;
    }
  }

  std::vector<Token> t_;
  size_t pos_ = 0;
};

}  // namespace

Statement Parse(const std::string& sql) {
  Statement st;
  try {
    Lexer lex(sql);
    Parser p(lex.Run());
    return p.ParseStatement();
  } catch (const ParseFail& f) {
    st.kind = StmtKind::Empty;
    st.error = f.name;
    return st;
  }
}

}  // namespace vdb
