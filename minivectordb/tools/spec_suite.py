#!/usr/bin/env python3
"""Spec-driven self-test suite for MiniVectorDB (2026-competition-round2).

Covers every requirement stated in the problem statement beyond the 20 public
samples: schema/type boundaries, SQL semantics, vector retrieval, index/ann,
transactions, crash recovery, unsupported statements, and protocol variants.
"""

import argparse
import json
import math
import os
import select
import shutil
import struct
import subprocess
import sys
import tempfile
import time

TOL = 1e-5
FAILS = []
COUNT = [0]


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def vlit(v):
    return "[" + ",".join(repr(x) for x in v) + "]"


class Db:
    def __init__(self, binary, datadir):
        self.binary = binary
        self.datadir = datadir
        self.errpath = os.path.join(datadir, "spec-stderr.log")
        self.err = open(self.errpath, "ab")
        self.p = subprocess.Popen([binary, "--data", datadir], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=self.err)
        self.buf = b""

    def send(self, sql):
        self.p.stdin.write(sql.encode("utf-8") + b"\n")
        self.p.stdin.flush()

    def read(self, timeout=60.0):
        fd = self.p.stdout.fileno()
        deadline = time.time() + timeout
        while b"\n" not in self.buf:
            remain = deadline - time.time()
            if remain <= 0:
                raise TimeoutError("no output")
            r, _, _ = select.select([fd], [], [], remain)
            if not r:
                raise TimeoutError("no output")
            chunk = os.read(fd, 1 << 16)
            if not chunk:
                raise EOFError("process exited")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line.decode("utf-8"))

    def q(self, sql):
        self.send(sql)
        return self.read()

    def restart(self):
        self.send("--restart")
        self.p.wait(timeout=60)
        self.err.close()
        return Db(self.binary, self.datadir)

    def kill(self):
        self.p.kill()
        self.p.wait()
        self.err.close()
        return Db(self.binary, self.datadir)

    def close(self):
        try:
            self.p.stdin.close()
        except OSError:
            pass
        self.p.wait(timeout=60)
        self.err.close()


def num_eq(a, b):
    if isinstance(a, bool) or isinstance(b, bool):
        return a == b
    if isinstance(a, int) and isinstance(b, int):
        return a == b
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        if isinstance(a, int) and isinstance(b, float) and b == int(b):
            return a == b
        if isinstance(b, int) and isinstance(a, float) and a == int(a):
            return a == b
        diff = abs(a - b)
        return diff <= TOL or diff <= TOL * max(abs(a), abs(b))
    return a == b


def cell_eq(a, b):
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(num_eq(x, y) for x, y in zip(a, b))
    if isinstance(a, str) or isinstance(b, str):
        return a == b
    return num_eq(a, b)


def result_eq(got, want):
    if isinstance(want, dict) and "error" in want:
        return isinstance(got, dict) and got.get("error") == want["error"]
    if not isinstance(got, dict) or "error" in got:
        return False
    for k in ("affected_rows", "columns"):
        if k in want and got.get(k) != want[k]:
            return False
    if "rows" in want:
        rows = got.get("rows")
        if rows is None or len(rows) != len(want["rows"]):
            return False
        for a, b in zip(rows, want["rows"]):
            if len(a) != len(b) or not all(cell_eq(x, y) for x, y in zip(a, b)):
                return False
    return True


def chk(label, got, want):
    COUNT[0] += 1
    if result_eq(got, want):
        return
    FAILS.append("%s\n   got=%r\n  want=%r" % (label, got, want))


def err(label, got):
    """Any non-empty error code is acceptable for unsupported constructs."""
    COUNT[0] += 1
    if not (isinstance(got, dict) and isinstance(got.get("error"), str) and got["error"]):
        FAILS.append("%s\n   got=%r (expected non-empty error)" % (label, got))


def ok_empty(label, got):
    chk(label, got, {})


def chk_raw(label, got, want):
    COUNT[0] += 1
    if got != want:
        FAILS.append("%s\n   got=%r\n  want=%r" % (label, got, want))


# ---------------------------------------------------------------- schema / ddl
def schema_suite(db):
    ok_empty("create basic", db.q(
        "CREATE TABLE docs (id BIGINT PRIMARY KEY, s VARCHAR(16), v VECTOR(3))"))
    ok_empty("create inline+constraints", db.q(
        "CREATE TABLE t2 (id BIGINT NOT NULL, s VARCHAR(4) NULL, v VECTOR(2), "
        "PRIMARY KEY (id))"))
    ok_empty("create backtick", db.q(
        "CREATE TABLE `t3` (`id` BIGINT, `v` VECTOR(2), PRIMARY KEY (`id`))"))
    ok_empty("create if not exists", db.q(
        "CREATE TABLE IF NOT EXISTS t3 (id BIGINT PRIMARY KEY)"))
    chk("duplicate table", db.q("CREATE TABLE docs (id BIGINT PRIMARY KEY)"),
        {"error": "table_exists"})
    err("no primary key", db.q("CREATE TABLE np (id BIGINT, v VECTOR(2))"))
    err("two vector columns", db.q(
        "CREATE TABLE two (id BIGINT PRIMARY KEY, a VECTOR(2), b VECTOR(2))"))
    err("non-bigint pk", db.q("CREATE TABLE badpk (id VARCHAR(4) PRIMARY KEY)"))
    err("duplicate column", db.q(
        "CREATE TABLE dupc (id BIGINT PRIMARY KEY, id BIGINT)"))
    err("alter table", db.q("ALTER TABLE docs ADD extra BIGINT"))
    err("drop table", db.q("DROP TABLE docs"))
    err("create foreign key", db.q(
        "CREATE TABLE fk (id BIGINT PRIMARY KEY, r BIGINT, FOREIGN KEY (r) REFERENCES docs(id))"))
    err("varchar zero", db.q("CREATE TABLE v0 (id BIGINT PRIMARY KEY, s VARCHAR(0))"))
    ok_empty("create vector index", db.q("CREATE VECTOR INDEX docs_vec ON docs(v)"))
    ok_empty("create index idempotent", db.q("CREATE VECTOR INDEX docs_vec ON docs(v)"))
    err("index on scalar", db.q("CREATE VECTOR INDEX bad ON docs(s)"))
    err("index unknown column", db.q("CREATE VECTOR INDEX bad2 ON docs(nope)"))


# --------------------------------------------------------------- types / rows
def types_suite(db):
    ok_empty("create t", db.q("CREATE TABLE t (id BIGINT PRIMARY KEY, a BIGINT, s VARCHAR(16), v VECTOR(2))"))
    imin = -9223372036854775808
    imax = 9223372036854775807
    chk("insert bigint bounds", db.q(
        "INSERT INTO t VALUES (%d, %d, 'x', '[0,0]'), (%d, %d, 'y', '[0,0]')"
        % (imin, imin, imax, imax)), {"affected_rows": 2})
    chk("read bigint bounds", db.q(
        "SELECT id, a FROM t ORDER BY id"),
        {"columns": ["id", "a"], "rows": [[imin, imin], [imax, imax]]})
    err("bigint literal overflow", db.q("INSERT INTO t VALUES (1, 9223372036854775808, 'x', '[0,0]')"))
    err("bigint from string", db.q("INSERT INTO t VALUES (1, '123', 'x', '[0,0]')"))

    ok_empty("create u", db.q("CREATE TABLE u (id BIGINT PRIMARY KEY, s VARCHAR(2))"))
    chk("varchar 2 cjk", db.q("INSERT INTO u VALUES (1, '中文')"), {"affected_rows": 1})
    err("varchar 3 cjk", db.q("INSERT INTO u VALUES (2, '中文字')"))
    chk("varchar emoji counts one", db.q("INSERT INTO u VALUES (3, '🙂')"), {"affected_rows": 1})
    chk("string code-point order", db.q(
        "SELECT id FROM u WHERE s > 'z' ORDER BY id"),
        {"columns": ["id"], "rows": [[1], [3]]})
    chk("string compare exact", db.q("SELECT id FROM u WHERE s = '中文'"),
        {"columns": ["id"], "rows": [[1]]})
    err("varchar too long update", db.q("UPDATE u SET s = '中文x' WHERE id = 1"))
    chk("too-long update unchanged", db.q("SELECT s FROM u WHERE id = 1"),
        {"columns": ["s"], "rows": [["中文"]]})

    ok_empty("create q", db.q("CREATE TABLE q (id BIGINT PRIMARY KEY, s VARCHAR(16))"))
    chk("double quoted string", db.q('INSERT INTO q VALUES (1, "a;b(1)")'), {"affected_rows": 1})
    chk("escaped quote", db.q("INSERT INTO q VALUES (2, 'O''Brien')"), {"affected_rows": 1})
    chk("read quoted", db.q("SELECT id, s FROM q ORDER BY id"),
        {"columns": ["id", "s"], "rows": [[1, "a;b(1)"], [2, "O'Brien"]]})

    ok_empty("create f", db.q("CREATE TABLE f (id BIGINT PRIMARY KEY, v VECTOR(2))"))
    chk("float32 rounding", db.q(
        "INSERT INTO f VALUES (1, '[16777217,0]'), (2, '[16777216,0]')"),
        {"affected_rows": 2})
    chk("float32 distance", db.q(
        "SELECT id, L2_DISTANCE(v, '[16777216,0]') AS d FROM f ORDER BY id"),
        {"columns": ["id", "d"], "rows": [[1, 0.0], [2, 0.0]]})
    chk("3-4-5 distance", db.q(
        "INSERT INTO f VALUES (3, '[3,4]')"), {"affected_rows": 1})
    chk("euclidean not squared", db.q(
        "SELECT id, L2_DISTANCE(v, '[0,0]') AS d FROM f WHERE id = 3"),
        {"columns": ["id", "d"], "rows": [[3, 5.0]]})
    err("vector dim mismatch", db.q("INSERT INTO f VALUES (4, '[1,2,3]')"))
    err("vector NaN", db.q("INSERT INTO f VALUES (4, '[NaN,0]')"))
    err("vector Infinity", db.q("INSERT INTO f VALUES (4, '[Infinity,0]')"))
    err("vector overflow", db.q("INSERT INTO f VALUES (4, '[1e40,0]')"))
    err("vector empty", db.q("INSERT INTO f VALUES (4, '[]')"))
    err("vector malformed", db.q("INSERT INTO f VALUES (4, 'not-a-vector')"))
    chk("multi-row insert atomic", db.q(
        "INSERT INTO f VALUES (5,'[0,0]'), (6,'[0]')"),
        {"error": "dimension_mismatch"})
    chk("no partial rows", db.q("SELECT id FROM f ORDER BY id"),
        {"columns": ["id"], "rows": [[1], [2], [3]]})
    err("duplicate pk in statement", db.q(
        "INSERT INTO f VALUES (7,'[0,0]'), (7,'[0,0]')"))
    chk("insert then duplicate", db.q("INSERT INTO f VALUES (7,'[0,0]')"), {"affected_rows": 1})
    chk("duplicate existing pk", db.q("INSERT INTO f VALUES (7,'[1,1]')"),
        {"error": "duplicate_key"})
    ok_empty("create ic", db.q("CREATE TABLE ic (id BIGINT PRIMARY KEY, s VARCHAR(8), v VECTOR(2))"))
    chk("column list reorder", db.q("INSERT INTO ic (v, id, s) VALUES ('[1,2]', 1, 'x')"),
        {"affected_rows": 1})
    chk("column list read", db.q("SELECT id, s, v FROM ic"),
        {"columns": ["id", "s", "v"], "rows": [[1, "x", [1, 2]]]})
    err("duplicate column list", db.q("INSERT INTO ic (id, id) VALUES (2, 2)"))
    err("column count mismatch", db.q("INSERT INTO ic (id, s) VALUES (2)"))


# ------------------------------------------------------------------- dml/query
def query_suite(db):
    ok_empty("create t", db.q(
        "CREATE TABLE t (id BIGINT PRIMARY KEY, grp BIGINT, s VARCHAR(8), v VECTOR(2))"))
    chk("multi insert", db.q(
        "INSERT INTO t VALUES (5,2,'e','[1,0]'), (1,1,'a','[0,1]'), (3,2,'c','[2,0]'), "
        "(2,1,'b','[0,2]'), (4,1,'d','[1,1]')"), {"affected_rows": 5})
    chk("select star", db.q("SELECT * FROM t ORDER BY id"), {
        "columns": ["id", "grp", "s", "v"],
        "rows": [[1, 1, "a", [0, 1]], [2, 1, "b", [0, 2]], [3, 2, "c", [2, 0]],
                 [4, 1, "d", [1, 1]], [5, 2, "e", [1, 0]]]})
    chk("cmp AND", db.q("SELECT id FROM t WHERE grp = 1 AND id >= 2 ORDER BY id"),
        {"columns": ["id"], "rows": [[2], [4]]})
    chk("cmp lt", db.q("SELECT id FROM t WHERE grp < 2 ORDER BY id"),
        {"columns": ["id"], "rows": [[1], [2], [4]]})
    chk("string cmp", db.q("SELECT id FROM t WHERE s <= 'b' ORDER BY id"),
        {"columns": ["id"], "rows": [[1], [2]]})
    chk("projection alias", db.q("SELECT id AS k, grp AS g FROM t WHERE id = 1"),
        {"columns": ["k", "g"], "rows": [[1, 1]]})
    chk("order by unselected column", db.q("SELECT id FROM t ORDER BY grp DESC, id"),
        {"columns": ["id"], "rows": [[3], [5], [1], [2], [4]]})
    chk("order ties pk asc", db.q("SELECT id FROM t ORDER BY grp, id DESC"),
        {"columns": ["id"], "rows": [[4], [2], [1], [5], [3]]})
    chk("limit zero", db.q("SELECT id FROM t ORDER BY id LIMIT 0"),
        {"columns": ["id"], "rows": []})
    chk("limit exceeds rows", db.q("SELECT id FROM t ORDER BY id LIMIT 99"),
        {"columns": ["id"], "rows": [[1], [2], [3], [4], [5]]})
    chk("empty table", db.q("SELECT id FROM t WHERE id > 100 ORDER BY id"),
        {"columns": ["id"], "rows": []})
    chk("delete none", db.q("DELETE FROM t WHERE id > 100"), {"affected_rows": 0})
    chk("update no where", db.q("UPDATE t SET grp = 7"), {"affected_rows": 5})
    chk("update all read", db.q("SELECT id, grp FROM t ORDER BY id"),
        {"columns": ["id", "grp"], "rows": [[1, 7], [2, 7], [3, 7], [4, 7], [5, 7]]})
    chk("update multiple cols", db.q(
        "UPDATE t SET grp = 1, s = 'z', v = '[9,9]' WHERE id = 3"), {"affected_rows": 1})
    chk("update vector applied", db.q(
        "SELECT id, s, v FROM t WHERE id = 3"),
        {"columns": ["id", "s", "v"], "rows": [[3, "z", [9, 9]]]})
    err("update pk", db.q("UPDATE t SET id = 9 WHERE id = 1"))
    ok_empty("delete no where", db.q("DELETE FROM t"))
    chk("delete all", db.q("SELECT id FROM t"), {"columns": ["id"], "rows": []})
    chk("reinsert after delete", db.q("INSERT INTO t VALUES (1,0,'a','[0,0]')"),
        {"affected_rows": 1})


# --------------------------------------------------------------- vector topk
def vector_suite(db):
    ok_empty("create t", db.q(
        "CREATE TABLE t (id BIGINT PRIMARY KEY, cat VARCHAR(8), v VECTOR(2))"))
    db.q("INSERT INTO t VALUES (8,'keep','[1,0]'), (3,'keep','[-1,0]'), "
         "(1,'skip','[0,0]'), (2,'skip','[0,0]')")
    chk("topk filter before limit", db.q(
        "SELECT id, L2_DISTANCE(v, '[0,0]') AS d FROM t WHERE cat = 'keep' ORDER BY d LIMIT 1"),
        {"columns": ["id", "d"], "rows": [[3, 1.0]]})
    chk("topk ties pk asc", db.q(
        "SELECT id, L2_DISTANCE(v, '[0,0]') AS d FROM t WHERE cat = 'keep' ORDER BY d LIMIT 9"),
        {"columns": ["id", "d"], "rows": [[3, 1.0], [8, 1.0]]})
    chk("topk empty filter", db.q(
        "SELECT id FROM t WHERE cat = 'absent' ORDER BY id LIMIT 9"),
        {"columns": ["id"], "rows": []})
    chk("order by distance expression", db.q(
        "SELECT id FROM t ORDER BY L2_DISTANCE(v, '[0,0]') LIMIT 2"),
        {"columns": ["id"], "rows": [[1], [2]]})
    chk("distance expr with desc", db.q(
        "SELECT id FROM t ORDER BY L2_DISTANCE(v, '[0,0]') DESC LIMIT 2"),
        {"columns": ["id"], "rows": [[3], [8]]})
    chk("distance expr + filter", db.q(
        "SELECT id FROM t WHERE cat = 'skip' ORDER BY L2_DISTANCE(v, '[0,0]') LIMIT 5"),
        {"columns": ["id"], "rows": [[1], [2]]})
    ok_empty("create index", db.q("CREATE VECTOR INDEX t_vec ON t(v)"))
    exact = db.q("SELECT id, L2_DISTANCE(v, '[0,0]') AS d FROM t ORDER BY d LIMIT 3")
    chk("exact with index", exact,
        {"columns": ["id", "d"], "rows": [[1, 0.0], [2, 0.0], [3, 1.0]]})
    db.send("--mode ann")
    chk("ann equals exact", db.q(
        "SELECT id, L2_DISTANCE(v, '[0,0]') AS d FROM t ORDER BY d LIMIT 3"), exact)
    db.send("--mode=ann")
    chk("ann= syntax equals exact", db.q(
        "SELECT id, L2_DISTANCE(v, '[0,0]') AS d FROM t ORDER BY d LIMIT 3"), exact)
    db.send("--mode exact")
    chk("switch back to exact", db.q(
        "SELECT id, L2_DISTANCE(v, '[0,0]') AS d FROM t ORDER BY d LIMIT 3"), exact)
    chk("index maintenance update", db.q("UPDATE t SET v = '[0,0]' WHERE id = 8"),
        {"affected_rows": 1})
    chk("index maintenance delete", db.q("DELETE FROM t WHERE id = 3"), {"affected_rows": 1})
    chk("index maintenance insert", db.q("INSERT INTO t VALUES (4,'keep','[0,1]')"),
        {"affected_rows": 1})
    after = db.q("SELECT id, L2_DISTANCE(v, '[0,0]') AS d FROM t ORDER BY d LIMIT 5")
    chk("maintenance result", after,
        {"columns": ["id", "d"], "rows": [[1, 0.0], [2, 0.0], [8, 0.0], [4, 1.0]]})
    db = db.restart()
    chk("ann after restart", db.q(
        "SELECT id, L2_DISTANCE(v, '[0,0]') AS d FROM t ORDER BY d LIMIT 5"), after)
    chk("filtered topk after restart", db.q(
        "SELECT id FROM t WHERE cat = 'skip' ORDER BY id"),
        {"columns": ["id"], "rows": [[1], [2]]})
    return db


# ---------------------------------------------------------------- transactions
def txn_suite(db):
    ok_empty("create a", db.q("CREATE TABLE a (id BIGINT PRIMARY KEY, s VARCHAR(8), v VECTOR(2))"))
    ok_empty("create b", db.q("CREATE TABLE b (id BIGINT PRIMARY KEY, s VARCHAR(8))"))
    chk("seed", db.q("INSERT INTO a VALUES (1,'base','[0,1]'), (2,'base','[1,1]')"),
        {"affected_rows": 2})
    ok_empty("begin", db.q("BEGIN"))
    err("nested begin", db.q("BEGIN"))
    chk("read own insert", db.q("INSERT INTO a VALUES (3,'txn','[0,0]')"), {"affected_rows": 1})
    chk("read own update", db.q("UPDATE a SET s = 'txn' WHERE id = 1"), {"affected_rows": 1})
    chk("read own vector update", db.q("UPDATE a SET v = '[5,5]' WHERE id = 1"), {"affected_rows": 1})
    chk("txn sees new vector", db.q(
        "SELECT id, L2_DISTANCE(v, '[0,0]') AS d FROM a WHERE id = 1"),
        {"columns": ["id", "d"], "rows": [[1, math.sqrt(50.0)]]})
    chk("read own delete", db.q("DELETE FROM a WHERE id = 2"), {"affected_rows": 1})
    chk("visible in txn", db.q("SELECT id, s FROM a ORDER BY id"),
        {"columns": ["id", "s"], "rows": [[1, "txn"], [3, "txn"]]})
    chk("txn topk sees writes", db.q(
        "SELECT id FROM a WHERE s = 'txn' ORDER BY L2_DISTANCE(v, '[0,0]') LIMIT 5"),
        {"columns": ["id"], "rows": [[3], [1]]})
    chk("txn multi-table", db.q("INSERT INTO b VALUES (1,'p')"), {"affected_rows": 1})
    ok_empty("rollback", db.q("ROLLBACK"))
    chk("rollback content", db.q("SELECT id, s FROM a ORDER BY id"),
        {"columns": ["id", "s"], "rows": [[1, "base"], [2, "base"]]})
    chk("rollback other table", db.q("SELECT id FROM b"), {"columns": ["id"], "rows": []})
    chk("rollback vector search", db.q(
        "SELECT id FROM a ORDER BY L2_DISTANCE(v, '[0,0]') LIMIT 5"),
        {"columns": ["id"], "rows": [[1], [2]]})
    chk("rollback restores vector", db.q(
        "SELECT L2_DISTANCE(v, '[0,0]') AS d FROM a WHERE id = 1"),
        {"columns": ["d"], "rows": [[1.0]]})

    ok_empty("begin2", db.q("BEGIN"))
    db.q("UPDATE a SET s = 'keep' WHERE id = 1")
    chk("failed insert in txn", db.q("INSERT INTO a VALUES (4,'x','[0]')"),
        {"error": "dimension_mismatch"})
    chk("failed duplicate in txn", db.q("INSERT INTO a VALUES (1,'dup','[0,0]')"),
        {"error": "duplicate_key"})
    chk("prior work kept", db.q("SELECT id, s FROM a ORDER BY id"),
        {"columns": ["id", "s"], "rows": [[1, "keep"], [2, "base"]]})
    ok_empty("commit2", db.q("COMMIT"))
    chk("committed", db.q("SELECT s FROM a WHERE id = 1"),
        {"columns": ["s"], "rows": [["keep"]]})
    chk("failed rows absent", db.q("SELECT id FROM a ORDER BY id"),
        {"columns": ["id"], "rows": [[1], [2]]})
    chk("autocommit fail atomic", db.q("INSERT INTO a VALUES (5,'y','[0,0]'), (6,'z','[0]')"),
        {"error": "dimension_mismatch"})
    chk("autocommit no partial", db.q("SELECT id FROM a ORDER BY id"),
        {"columns": ["id"], "rows": [[1], [2]]})
    ok_empty("commit without begin", db.q("COMMIT"))
    ok_empty("rollback without begin", db.q("ROLLBACK"))
    return db


# ------------------------------------------------------------------- recovery
def recovery_suite(binary, datadir):
    db = Db(binary, datadir)
    db.q("CREATE TABLE r (id BIGINT PRIMARY KEY, s VARCHAR(8), v VECTOR(2))")
    db.q("INSERT INTO r VALUES (1,'a','[0,0]'), (2,'b','[1,1]')")
    db = db.restart()
    chk("restart keeps rows", db.q("SELECT id, s FROM r ORDER BY id"),
        {"columns": ["id", "s"], "rows": [[1, "a"], [2, "b"]]})
    db.q("BEGIN")
    db.q("UPDATE r SET s = 'lost' WHERE id = 1")
    db.q("DELETE FROM r WHERE id = 2")
    db.q("INSERT INTO r VALUES (3,'lost','[2,2]')")
    db = db.restart()
    chk("normal shutdown rollback", db.q("SELECT id, s FROM r ORDER BY id"),
        {"columns": ["id", "s"], "rows": [[1, "a"], [2, "b"]]})
    db.q("UPDATE r SET s = 'durable' WHERE id = 1")
    db = db.kill()
    chk("kill keeps committed", db.q("SELECT id, s FROM r ORDER BY id"),
        {"columns": ["id", "s"], "rows": [[1, "durable"], [2, "b"]]})
    db.q("BEGIN")
    db.q("UPDATE r SET v = '[9,9]' WHERE id = 1")
    db.q("DELETE FROM r")
    db = db.kill()
    chk("kill drops uncommitted", db.q("SELECT id FROM r ORDER BY id"),
        {"columns": ["id"], "rows": [[1], [2]]})
    chk("kill drops uncommitted vector", db.q(
        "SELECT L2_DISTANCE(v, '[0,0]') AS d FROM r WHERE id = 1"),
        {"columns": ["d"], "rows": [[0.0]]})
    # kill repeatedly while the next process is still recovering
    for _ in range(3):
        db.kill()
    db = Db(binary, datadir)
    chk("recovery repeatable", db.q("SELECT id, s FROM r ORDER BY id"),
        {"columns": ["id", "s"], "rows": [[1, "durable"], [2, "b"]]})
    db.close()


# ---------------------------------------------------------------- unsupported
def unsupported_suite(db):
    db.q("CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(8), v VECTOR(2))")
    db.q("INSERT INTO t VALUES (1,'a','[0,0]')")
    err("insert NULL", db.q("INSERT INTO t VALUES (2, NULL, '[0,0]')"))
    err("join", db.q("SELECT t.id FROM t JOIN t x ON t.id = x.id"))
    err("aggregate", db.q("SELECT COUNT(*) FROM t"))
    err("subquery", db.q("SELECT id FROM t WHERE id IN (SELECT id FROM t)"))
    err("group by", db.q("SELECT id FROM t GROUP BY id"))
    err("or condition", db.q("SELECT id FROM t WHERE id = 1 OR id = 2"))
    err("not equal", db.q("SELECT id FROM t WHERE id != 1"))
    err("insert select", db.q("INSERT INTO t SELECT * FROM t"))
    err("unknown table", db.q("SELECT id FROM nope"))
    err("unknown column", db.q("SELECT nope FROM t"))
    err("syntax error", db.q("SELEC id FROM t"))
    db.close()


# --------------------------------------------------------------- protocol
def protocol_suite(binary, datadir):
    db = Db(binary, datadir)
    # UTF-8 BOM on the very first line, CRLF endings, trailing semicolons
    db.p.stdin.write(b"\xef\xbb\xbfCREATE TABLE p (id BIGINT PRIMARY KEY, v VECTOR(2));\r\n")
    db.p.stdin.flush()
    chk("bom+crlf create", db.read(), {})
    # two statements on one line produce two responses in order
    db.p.stdin.write(b"INSERT INTO p VALUES (1,'[0,0]'); INSERT INTO p VALUES (2,'[1,0]');\r\n")
    db.p.stdin.flush()
    chk("two statements line 1", db.read(), {"affected_rows": 1})
    chk("two statements line 2", db.read(), {"affected_rows": 1})
    db.p.stdin.write(b"\n\n")
    db.p.stdin.flush()
    # blank lines produce no response; a following query must still line up
    db.send("SELECT id FROM p ORDER BY id;")
    chk("after blank lines", db.read(), {"columns": ["id"], "rows": [[1], [2]]})
    db.close()


# ---------------------------------------------------------------- adversarial
def adversarial_suite(db):
    ok_empty("create t", db.q(
        "CREATE TABLE t (id BIGINT PRIMARY KEY, grp BIGINT, s VARCHAR(8), v VECTOR(2))"))
    db.q("INSERT INTO t VALUES (1,1,'aa','[0,0]'), (2,2,'bb','[1,0]'), (3,1,'cc','[2,0]')")
    chk("nested parens", db.q(
        "SELECT id FROM t WHERE ((grp = 1) AND (id >= 1)) ORDER BY id"),
        {"columns": ["id"], "rows": [[1], [3]]})
    chk("parens mixed", db.q(
        "SELECT id FROM t WHERE (grp = 1 AND s >= 'aa') AND id < 3 ORDER BY id"),
        {"columns": ["id"], "rows": [[1]]})
    chk("bare alias", db.q("SELECT id x FROM t WHERE id = 2 ORDER BY x"),
        {"columns": ["x"], "rows": [[2]]})
    ok_empty("create short", db.q("CREATE TABLE short (id BIGINT PRIMARY KEY, s VARCHAR(4))"))
    db.q("INSERT INTO short VALUES (1,'abcd')")
    chk("overlong literal compare", db.q("SELECT id FROM short WHERE s = 'abcdef'"),
        {"columns": ["id"], "rows": []})
    chk("overlong literal order", db.q("SELECT id FROM short WHERE s > 'abcde'"),
        {"columns": ["id"], "rows": []})
    err("overlong storage insert", db.q("INSERT INTO short VALUES (2,'abcde')"))

    # 1024-dim vectors and exact ties
    ok_empty("create v1024", db.q("CREATE TABLE v1024 (id BIGINT PRIMARY KEY, v VECTOR(1024))"))
    vec = "[" + ",".join("0.25" for _ in range(1024)) + "]"
    rows = ",".join("(%d,'%s')" % (i, vec) for i in range(1, 11))
    chk("insert 1024-dim", db.q("INSERT INTO v1024 VALUES " + rows), {"affected_rows": 10})
    chk("1024-dim topk ties", db.q(
        "SELECT id FROM v1024 ORDER BY L2_DISTANCE(v, '%s') LIMIT 3" %
        ("[" + ",".join("0.25" for _ in range(1024)) + "]")),
        {"columns": ["id"], "rows": [[1], [2], [3]]})

    # float32 boundary values
    ok_empty("create fb", db.q("CREATE TABLE fb (id BIGINT PRIMARY KEY, v VECTOR(2))"))
    chk("float32 large finite", db.q("INSERT INTO fb VALUES (1,'[3.4028234e38,0]')"),
        {"affected_rows": 1})
    err("float32 overflow", db.q("INSERT INTO fb VALUES (2,'[3.5e38,0]')"))
    chk("negative zero and subnormal", db.q(
        "INSERT INTO fb VALUES (3,'[-0.0,1e-45]')"), {"affected_rows": 1})

    # long VARCHAR boundary and unicode
    ok_empty("create lv", db.q("CREATE TABLE lv (id BIGINT PRIMARY KEY, s VARCHAR(512))"))
    s512 = "a" * 512
    chk("varchar 512 ascii", db.q("INSERT INTO lv VALUES (1,'%s')" % s512), {"affected_rows": 1})
    err("varchar 513 ascii", db.q("INSERT INTO lv VALUES (2,'%s')" % ("a" * 513)))
    chk("varchar 512 cjk", db.q("INSERT INTO lv VALUES (3,'%s')" % ("中" * 512)),
        {"affected_rows": 1})
    err("varchar 513 cjk", db.q("INSERT INTO lv VALUES (4,'%s')" % ("中" * 513)))

    # unicode code-point ordering matches Python
    ok_empty("create uni", db.q("CREATE TABLE uni (id BIGINT PRIMARY KEY, s VARCHAR(8))"))
    words = ["a", "Z", "中", "文", "🙂", "é", "中a", "ｚ"]
    vals = ",".join("(%d,'%s')" % (i + 1, w) for i, w in enumerate(words))
    db.q("INSERT INTO uni VALUES " + vals)
    for pivot in ["Z", "中", "🙂", "é"]:
        want = [[i + 1] for i, w in enumerate(words) if w > pivot]
        chk("unicode order vs %s" % pivot, db.q(
            "SELECT id FROM uni WHERE s > '%s' ORDER BY id" % pivot),
            {"columns": ["id"], "rows": want})

    # order by multiple keys, mixed direction, ties by pk
    chk("multi-key mixed", db.q(
        "SELECT id FROM t ORDER BY grp DESC, s ASC, id"),
        {"columns": ["id"], "rows": [[2], [1], [3]]})
    chk("order alias shadows nothing", db.q(
        "SELECT grp AS k FROM t ORDER BY k DESC LIMIT 1"),
        {"columns": ["k"], "rows": [[2]]})

    # same pk re-inserted after delete in one transaction
    db.q("BEGIN")
    db.q("INSERT INTO t VALUES (10,5,'x','[9,9]')")
    db.q("UPDATE t SET s = 'y' WHERE id = 10")
    db.q("DELETE FROM t WHERE id = 10")
    chk("reinsert in txn", db.q("INSERT INTO t VALUES (10,6,'z','[8,8]')"), {"affected_rows": 1})
    ok_empty("commit reinsert", db.q("COMMIT"))
    chk("final reinsert state", db.q("SELECT grp, s FROM t WHERE id = 10"),
        {"columns": ["grp", "s"], "rows": [[6, "z"]]})

    # bulk transaction rollback (500 rows)
    ok_empty("create bulk", db.q("CREATE TABLE bulk (id BIGINT PRIMARY KEY, v VECTOR(2))"))
    db.q("BEGIN")
    for start in range(0, 500, 100):
        rows = ",".join("(%d,'[0,0]')" % i for i in range(start, start + 100))
        db.q("INSERT INTO bulk VALUES " + rows)
    ok_empty("bulk rollback", db.q("ROLLBACK"))
    chk("bulk rolled back", db.q("SELECT id FROM bulk LIMIT 5"),
        {"columns": ["id"], "rows": []})
    for start in range(0, 500, 100):
        rows = ",".join("(%d,'[0,0]')" % i for i in range(start, start + 100))
        db.q("INSERT INTO bulk VALUES " + rows)
    got = db.q("SELECT id FROM bulk ORDER BY id LIMIT 1000")
    chk_raw("bulk committed count", len(got["rows"]), 500)
    chk_raw("bulk committed first-last", [got["rows"][0], got["rows"][-1]], [[0], [499]])

    # large single statement (300 rows x 128 dims)
    ok_empty("create wide", db.q(
        "CREATE TABLE wide (id BIGINT PRIMARY KEY, v VECTOR(128))"))
    v = "[" + ",".join("0.5" for _ in range(128)) + "]"
    big = ",".join("(%d,'%s')" % (i, v) for i in range(300))
    chk("large single insert", db.q("INSERT INTO wide VALUES " + big), {"affected_rows": 300})
    chk_raw("large statement topk", len(db.q(
        "SELECT id FROM wide ORDER BY L2_DISTANCE(v, '%s') LIMIT 5" % v)["rows"]), 5)

    # rapid statement stream
    t0 = time.time()
    for i in range(2000):
        db.q("SELECT id FROM t WHERE id = 1")
    elapsed = time.time() - t0
    chk_raw("rapid stream ok", elapsed < 60.0, True)

    # filtered TopK restricted by primary key range
    chk("pk-range topk", db.q(
        "SELECT id FROM t WHERE id >= 2 AND id <= 3 ORDER BY L2_DISTANCE(v, '[0,0]') LIMIT 2"),
        {"columns": ["id"], "rows": [[2], [3]]})
    # alias shadows an existing column name (alias wins in ORDER BY)
    chk("alias shadowing", db.q("SELECT grp AS s FROM t ORDER BY s"),
        {"columns": ["s"], "rows": [[1], [1], [2], [6]]})
    # distance alias with filter and DESC
    chk("alias distance desc filtered", db.q(
        "SELECT id, L2_DISTANCE(v, '[0,0]') AS d FROM t WHERE grp = 1 ORDER BY d DESC LIMIT 2"),
        {"columns": ["id", "d"], "rows": [[3, 2.0], [1, 0.0]]})
    # equal distances at the LIMIT boundary keep primary-key order
    ok_empty("create ties", db.q("CREATE TABLE ties (id BIGINT PRIMARY KEY, v VECTOR(2))"))
    db.q("INSERT INTO ties VALUES " + ",".join("(%d,'[1,1]')" % i for i in range(5, 0, -1)))
    chk("boundary ties", db.q(
        "SELECT id FROM ties ORDER BY L2_DISTANCE(v, '[1,1]') LIMIT 3"),
        {"columns": ["id"], "rows": [[1], [2], [3]]})
    # empty string ordering
    ok_empty("create es", db.q("CREATE TABLE es (id BIGINT PRIMARY KEY, s VARCHAR(4))"))
    db.q("INSERT INTO es VALUES (1,''), (2,'a')")
    chk("empty string compare", db.q("SELECT id FROM es WHERE s < 'a'"),
        {"columns": ["id"], "rows": [[1]]})
    # string condition update by code-point order
    ok_empty("create uc", db.q("CREATE TABLE uc (id BIGINT PRIMARY KEY, s VARCHAR(8))"))
    db.q("INSERT INTO uc VALUES (1,'a'), (2,'中'), (3,'文'), (4,'🙂')")
    chk("update by codepoint order", db.q("UPDATE uc SET s = 'z' WHERE s > '中'"),
        {"affected_rows": 2})
    chk("codepoint update result", db.q("SELECT id, s FROM uc ORDER BY id"),
        {"columns": ["id", "s"], "rows": [[1, "a"], [2, "中"], [3, "z"], [4, "z"]]})
    # ann mode without any index still answers exactly
    ok_empty("create noi", db.q("CREATE TABLE noi (id BIGINT PRIMARY KEY, v VECTOR(2))"))
    db.q("INSERT INTO noi VALUES (1,'[0,0]'), (2,'[1,0]'), (3,'[2,0]')")
    db.send("--mode ann")
    chk("ann without index", db.q(
        "SELECT id FROM noi ORDER BY L2_DISTANCE(v, '[0,0]') LIMIT 2"),
        {"columns": ["id"], "rows": [[1], [2]]})
    db.send("--mode exact")
    # index on a table without a vector column
    ok_empty("create nv", db.q("CREATE TABLE nv (id BIGINT PRIMARY KEY)"))
    err("index on non-vector table", db.q("CREATE VECTOR INDEX nv_i ON nv(id)"))
    # multiple tables with indexes survive a restart together
    ok_empty("create ma", db.q("CREATE TABLE ma (id BIGINT PRIMARY KEY, v VECTOR(2))"))
    ok_empty("create mb", db.q("CREATE TABLE mb (id BIGINT PRIMARY KEY, v VECTOR(2))"))
    db.q("CREATE VECTOR INDEX ma_i ON ma(v)")
    db.q("CREATE VECTOR INDEX mb_i ON mb(v)")
    db.q("INSERT INTO ma VALUES (1,'[0,0]'), (2,'[1,1]')")
    db.q("INSERT INTO mb VALUES (1,'[2,2]'), (2,'[0,0]')")
    db = db.restart()
    chk("multi index restart a", db.q(
        "SELECT id FROM ma ORDER BY L2_DISTANCE(v, '[0,0]') LIMIT 1"),
        {"columns": ["id"], "rows": [[1]]})
    chk("multi index restart b", db.q(
        "SELECT id FROM mb ORDER BY L2_DISTANCE(v, '[0,0]') LIMIT 1"),
        {"columns": ["id"], "rows": [[2]]})
    # failed update inside a transaction keeps the transaction usable
    db.q("BEGIN")
    chk("failed update in txn", db.q("UPDATE ma SET v = '[1]' WHERE id = 1"),
        {"error": "dimension_mismatch"})
    chk("update after failure", db.q("UPDATE ma SET v = '[9,9]' WHERE id = 1"),
        {"affected_rows": 1})
    ok_empty("commit after failure", db.q("COMMIT"))
    chk("vector unchanged by failed update", db.q(
        "SELECT L2_DISTANCE(v, '[0,0]') AS d FROM ma WHERE id = 1"),
        {"columns": ["d"], "rows": [[math.sqrt(162.0)]]})
    # table-level primary key placed before the columns
    ok_empty("pk constraint first", db.q(
        "CREATE TABLE pkf (PRIMARY KEY (id), id BIGINT, v VECTOR (2), s VARCHAR (4))"))
    db.q("INSERT INTO pkf VALUES (1,'[0,0]','a')")
    chk("pk constraint first read", db.q("SELECT id, s FROM pkf"),
        {"columns": ["id", "s"], "rows": [[1, "a"]]})
    # uppercase keywords / identifiers and backticked names with spaces
    ok_empty("uppercase ddl", db.q(
        "CREATE TABLE UP (ID BIGINT PRIMARY KEY, S VARCHAR(8), V VECTOR(2))"))
    db.q("INSERT INTO UP VALUES (1,'x','[0,0]')")
    chk("uppercase select", db.q("SELECT ID, S FROM UP WHERE ID = 1"),
        {"columns": ["ID", "S"], "rows": [[1, "x"]]})
    ok_empty("backtick spaced", db.q(
        "CREATE TABLE `sp` (`my id` BIGINT PRIMARY KEY, v VECTOR(2))"))
    db.q("INSERT INTO sp (`my id`, v) VALUES (1, '[0,0]')")
    chk("backtick spaced order", db.q("SELECT `my id` FROM sp ORDER BY `my id`"),
        {"columns": ["my id"], "rows": [[1]]})
    # ORDER BY primary key DESC with LIMIT
    chk("order pk desc limit", db.q("SELECT id FROM t ORDER BY id DESC LIMIT 2"),
        {"columns": ["id"], "rows": [[10], [3]]})
    # comment-only lines are ignored
    db.p.stdin.write(b"# a comment line\n")
    db.p.stdin.flush()
    db.send("SELECT id FROM t WHERE id = 1")
    chk("after comment line", db.read(), {"columns": ["id"], "rows": [[1]]})
    return db


# ------------------------------------------------------------- signals/kills
def signal_suite(binary, datadir):
    db = Db(binary, datadir)
    db.q("CREATE TABLE s (id BIGINT PRIMARY KEY, v VECTOR(2))")
    db.q("INSERT INTO s VALUES (1,'[0,0]')")
    db.q("BEGIN")
    db.q("INSERT INTO s VALUES (2,'[1,1]')")
    db.p.terminate()
    db.p.wait(timeout=30)
    db.err.close()
    db = Db(binary, datadir)
    chk("sigterm rolls back txn", db.q("SELECT id FROM s ORDER BY id"),
        {"columns": ["id"], "rows": [[1]]})
    db.close()

    # commit-then-kill repeatedly: every acknowledged commit must survive
    db = Db(binary, datadir)
    for i in range(2, 7):
        chk("commit before kill %d" % i, db.q(
            "INSERT INTO s VALUES (%d,'[0,0]')" % i), {"affected_rows": 1})
        db = db.kill()
        got = db.q("SELECT id FROM s ORDER BY id")
        chk_raw("committed survive kill %d" % i, len(got["rows"]), i)
        chk_raw("latest committed present %d" % i, got["rows"][-1], [i])
    db.close()


# -------------------------------------------------------------------- main
def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=os.path.join(here, "..", "build", "minivectordb"))
    args = ap.parse_args()

    def run(name, fn, fresh):
        datadir = tempfile.mkdtemp(prefix="mvdb-spec-")
        try:
            db = Db(args.binary, datadir)
            out = fn(db)
            if out is not None:
                db = out
            db.close()
        except (TimeoutError, EOFError) as exc:
            FAILS.append("%s: driver error: %s" % (name, exc))
        finally:
            shutil.rmtree(datadir, ignore_errors=True)
        print("section %-14s checks=%d failures=%d" % (name, COUNT[0], len(FAILS)), flush=True)

    run("schema", schema_suite, True)
    run("types", types_suite, True)
    run("query", query_suite, True)
    run("vector", vector_suite, True)
    run("txn", txn_suite, True)
    recovery_suite(args.binary, tempfile.mkdtemp(prefix="mvdb-spec-recovery-"))
    print("section %-14s checks=%d failures=%d" % ("recovery", COUNT[0], len(FAILS)), flush=True)
    run("adversarial", adversarial_suite, True)
    signal_suite(args.binary, tempfile.mkdtemp(prefix="mvdb-spec-signal-"))
    print("section %-14s checks=%d failures=%d" % ("signal", COUNT[0], len(FAILS)), flush=True)
    run("unsupported", unsupported_suite, True)
    protocol_suite(args.binary, tempfile.mkdtemp(prefix="mvdb-spec-proto-"))
    print("section %-14s checks=%d failures=%d" % ("protocol", COUNT[0], len(FAILS)), flush=True)

    print("\n%d checks, %d failures" % (COUNT[0], len(FAILS)))
    for f in FAILS[:25]:
        print("FAIL " + f)
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
