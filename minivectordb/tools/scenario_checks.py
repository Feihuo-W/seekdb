#!/usr/bin/env python3
"""Scenario checks for behaviours the public case pack does not cover.

Covers multi-table transactions, statement-failure atomicity, quote handling,
index/ann equivalence across restarts, crash rollback, and optionally a scale
run that triggers compaction.
"""

import argparse
import json
import os
import select
import shutil
import subprocess
import sys
import tempfile
import time


class Db:
    def __init__(self, binary, datadir):
        self.binary = binary
        self.datadir = datadir
        self.errpath = os.path.join(datadir, "scenario-stderr.log")
        self.err = open(self.errpath, "ab")
        self.p = subprocess.Popen([binary, "--data", datadir], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=self.err)
        self.buf = b""

    def send(self, sql):
        self.p.stdin.write(sql.encode("utf-8") + b"\n")
        self.p.stdin.flush()

    def read(self, timeout=30.0):
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

    def close_gracefully(self):
        self.send("--restart")
        self.p.wait(timeout=30)
        self.err.close()

    def kill(self):
        self.p.kill()
        self.p.wait()
        self.err.close()

    def reopen(self):
        return Db(self.binary, self.datadir)


FAILS = []


def chk(label, got, want):
    if got != want:
        FAILS.append("%s: got %r want %r" % (label, got, want))
        print("FAIL %s\n  got  %r\n  want %r" % (label, got, want))
    else:
        print("ok   %s" % label)


def multi_table(binary, datadir):
    db = Db(binary, datadir)
    chk("create a", db.q("CREATE TABLE a (id BIGINT PRIMARY KEY, s VARCHAR(8), v VECTOR(2))"), {})
    chk("create b", db.q("CREATE TABLE b (id BIGINT PRIMARY KEY, s VARCHAR(8))"), {})
    db.q("BEGIN")
    chk("txn insert a", db.q("INSERT INTO a VALUES (1,'x','[0,1]'),(2,'y','[1,1]')"),
        {"affected_rows": 2})
    chk("txn insert b", db.q("INSERT INTO b VALUES (1,'p')"), {"affected_rows": 1})
    chk("rollback", db.q("ROLLBACK"), {})
    chk("a empty", db.q("SELECT id FROM a"), {"columns": ["id"], "rows": []})
    chk("b empty", db.q("SELECT id FROM b"), {"columns": ["id"], "rows": []})
    db.q("BEGIN")
    db.q("INSERT INTO a VALUES (1,'x','[0,1]'),(2,'y','[1,1]')")
    db.q("INSERT INTO b VALUES (1,'p')")
    chk("commit", db.q("COMMIT"), {})
    db.close_gracefully()
    db = db.reopen()
    chk("a after restart", db.q("SELECT id,s FROM a ORDER BY id"),
        {"columns": ["id", "s"], "rows": [[1, "x"], [2, "y"]]})
    chk("b after restart", db.q("SELECT id,s FROM b ORDER BY id"),
        {"columns": ["id", "s"], "rows": [[1, "p"]]})
    db.close_gracefully()


def atomicity(binary, datadir):
    db = Db(binary, datadir)
    db.q("CREATE TABLE a (id BIGINT PRIMARY KEY, s VARCHAR(8), v VECTOR(2))")
    db.q("INSERT INTO a VALUES (1,'x','[0,1]'),(2,'y','[1,1]')")
    db.q("BEGIN")
    chk("txn update before failure", db.q("UPDATE a SET s='keep' WHERE id=1"),
        {"affected_rows": 1})
    chk("too long", db.q("INSERT INTO a VALUES (3,'%s','[0,0]')" % ("中" * 9)),
        {"error": "value_too_long"})
    chk("bad dim mid batch", db.q("INSERT INTO a VALUES (4,'q','[0,0]'),(5,'r','[0]')"),
        {"error": "dimension_mismatch"})
    chk("duplicate in txn", db.q("INSERT INTO a VALUES (2,'dup','[0,0]')"),
        {"error": "duplicate_key"})
    chk("earlier change kept", db.q("SELECT id,s FROM a ORDER BY id"),
        {"columns": ["id", "s"], "rows": [[1, "keep"], [2, "y"]]})
    chk("commit after failures", db.q("COMMIT"), {})
    chk("no partial rows", db.q("SELECT id FROM a ORDER BY id"),
        {"columns": ["id"], "rows": [[1], [2]]})
    chk("invalid update value", db.q("UPDATE a SET v='[1]' WHERE id=2"),
        {"error": "dimension_mismatch"})
    chk("vector unchanged", db.q("SELECT id, L2_DISTANCE(v,'[1,1]') AS d FROM a WHERE id=2"),
        {"columns": ["id", "d"], "rows": [[2, 0.0]]})
    db.close_gracefully()


def index_and_quotes(binary, datadir):
    db = Db(binary, datadir)
    chk("create", db.q('CREATE TABLE `q` (id BIGINT PRIMARY KEY, s VARCHAR(16), v VECTOR(2))'), {})
    chk("quoted strings", db.q('INSERT INTO q VALUES (1, "a;b(1)", "[3,4]")'),
        {"affected_rows": 1})
    chk("read back", db.q('SELECT id,s FROM q WHERE s = "a;b(1)"'),
        {"columns": ["id", "s"], "rows": [[1, "a;b(1)"]]})
    chk("reordered columns", db.q("INSERT INTO q (v, id, s) VALUES ('[0,0]', 2, 'x')"),
        {"affected_rows": 1})
    chk("create index", db.q("CREATE VECTOR INDEX q_vec ON q(v)"), {})
    exact = db.q("SELECT id, L2_DISTANCE(v,'[0,0]') AS distance FROM q ORDER BY distance LIMIT 5")
    chk("exact order", exact,
        {"columns": ["id", "distance"], "rows": [[2, 0.0], [1, 5.0]]})
    db.send("--mode ann")
    chk("ann equals exact", db.q(
        "SELECT id, L2_DISTANCE(v,'[0,0]') AS distance FROM q ORDER BY distance LIMIT 5"), exact)
    db.send("--mode=ann")
    chk("ann= syntax", db.q(
        "SELECT id, L2_DISTANCE(v,'[0,0]') AS distance FROM q ORDER BY distance LIMIT 5"), exact)
    db.close_gracefully()
    db = db.reopen()
    chk("ann after restart", db.q(
        "SELECT id, L2_DISTANCE(v,'[0,0]') AS distance FROM q ORDER BY distance LIMIT 5"), exact)
    db.close_gracefully()


def crash_txn(binary, datadir):
    db = Db(binary, datadir)
    db.q("CREATE TABLE c (id BIGINT PRIMARY KEY, s VARCHAR(8), v VECTOR(2))")
    db.q("INSERT INTO c VALUES (1,'base','[0,1]'),(2,'base','[1,1]')")
    db.q("BEGIN")
    db.q("UPDATE c SET s='txn' WHERE id=1")
    db.q("DELETE FROM c WHERE id=2")
    db.q("INSERT INTO c VALUES (3,'txn','[0,0]')")
    chk("read own writes", db.q("SELECT id,s FROM c ORDER BY id"),
        {"columns": ["id", "s"], "rows": [[1, "txn"], [3, "txn"]]})
    db.kill()
    db = db.reopen()
    chk("crash rolls back", db.q("SELECT id,s FROM c ORDER BY id"),
        {"columns": ["id", "s"], "rows": [[1, "base"], [2, "base"]]})
    db.close_gracefully()


def scale(binary, datadir, rows, dim):
    db = Db(binary, datadir)
    db.q("CREATE TABLE big (id BIGINT PRIMARY KEY, grp BIGINT, v VECTOR(%d))" % dim)
    vec = "[" + ",".join("0.5" for _ in range(dim)) + "]"
    batch = 1000
    t0 = time.time()
    for start in range(0, rows, batch):
        sql = "INSERT INTO big VALUES " + ",".join(
            "(%d,%d,'%s')" % (i, i % 10, vec) for i in range(start, min(start + batch, rows)))
        r = db.q(sql)
        chk("scale insert %d" % start, r, {"affected_rows": min(batch, rows - start)})
    print("scale insert %.1fs" % (time.time() - t0))
    db.close_gracefully()
    files = {f: os.path.getsize(os.path.join(datadir, f)) for f in os.listdir(datadir)}
    print("files", {f: s // (1 << 20) for f, s in files.items()})
    if rows * dim >= 200000:
        chk("snapshot created", "snapshot.dat" in files, True)
    db = db.reopen()
    chk("scale row present", db.q("SELECT id FROM big WHERE id=%d" % (rows - 1)),
        {"columns": ["id"], "rows": [[rows - 1]]})
    db.close_gracefully()


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=os.path.join(here, "..", "build", "minivectordb"))
    ap.add_argument("--scale-rows", type=int, default=0)
    ap.add_argument("--scale-dim", type=int, default=64)
    args = ap.parse_args()

    for fn in (multi_table, atomicity, index_and_quotes, crash_txn):
        datadir = tempfile.mkdtemp(prefix="mvdb-scn-")
        try:
            fn(args.binary, datadir)
        finally:
            shutil.rmtree(datadir, ignore_errors=True)
    if args.scale_rows > 0:
        datadir = tempfile.mkdtemp(prefix="mvdb-scale-")
        try:
            scale(args.binary, datadir, args.scale_rows, args.scale_dim)
        finally:
            shutil.rmtree(datadir, ignore_errors=True)

    print("\n%d failures" % len(FAILS))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
