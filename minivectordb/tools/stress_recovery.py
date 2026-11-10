#!/usr/bin/env python3
"""Crash-window stress: kill during shutdown compaction and recovery.

These scenarios are hard to express as mysqltest cases:
- SIGKILL while the process is compacting on `--restart` (rename/truncate window)
- repeated SIGKILL while the next process is still recovering
- SIGKILL after every acknowledged commit
"""

import argparse
import json
import os
import random
import select
import shutil
import subprocess
import sys
import tempfile
import time


def vlit(dim, val="0.25"):
    return "[" + ",".join(val for _ in range(dim)) + "]"


class Db:
    def __init__(self, binary, datadir):
        self.binary = binary
        self.datadir = datadir
        self.errpath = os.path.join(datadir, "stress-stderr.log")
        self.err = open(self.errpath, "ab")
        self.p = subprocess.Popen([binary, "--data", datadir], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=self.err)
        self.buf = b""

    def send(self, sql):
        self.p.stdin.write(sql.encode("utf-8") + b"\n")
        self.p.stdin.flush()

    def read(self, timeout=120.0):
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
        self.p.wait(timeout=120)
        self.err.close()

    def kill(self):
        self.p.kill()
        self.p.wait()
        self.err.close()

    def close(self):
        try:
            self.p.stdin.close()
        except OSError:
            pass
        self.p.wait(timeout=120)
        self.err.close()


FAILS = []


def chk(label, got, want):
    if got != want:
        FAILS.append("%s got=%r want=%r" % (label, got, want))
        print("FAIL", label, got, "!=", want)
    else:
        print("ok  ", label)


def build_rows(db, rows, dim, grp_mod=10):
    v = vlit(dim)
    batch = 1000
    for start in range(0, rows, batch):
        sql = "INSERT INTO big VALUES " + ",".join(
            "(%d,%d,'%s')" % (i, i % grp_mod, v)
            for i in range(start, min(start + batch, rows)))
        r = db.q(sql)
        if r.get("affected_rows") != min(batch, rows - start):
            raise RuntimeError("insert failed: %r" % (r,))


def verify_rows(db, rows, dim):
    r = db.q("SELECT id FROM big ORDER BY id LIMIT %d" % (rows + 10))
    ids = [row[0] for row in r["rows"]]
    chk("row count", len(ids), rows)
    chk("first id", ids[0], 0)
    chk("last id", ids[-1], rows - 1)
    r = db.q("SELECT id FROM big ORDER BY L2_DISTANCE(v, '%s') LIMIT 10" % vlit(dim, "0"))
    chk("topk size", len(r["rows"]), 10)
    r = db.q("SELECT id FROM big WHERE grp = 7 ORDER BY id")
    chk("filtered count", len(r["rows"]), (rows + 9) // 10)


def compaction_window(binary, rows, dim, attempts, seed):
    print("== compaction window: %d rows x %d dim, %d attempts ==" % (rows, dim, attempts))
    rng = random.Random(seed)
    for attempt in range(attempts):
        datadir = tempfile.mkdtemp(prefix="mvdb-cw-")
        db = Db(binary, datadir)
        db.q("CREATE TABLE big (id BIGINT PRIMARY KEY, grp BIGINT, v VECTOR(%d))" % dim)
        build_rows(db, rows, dim)
        # ask for a clean shutdown (which compacts >16MiB of WAL), then kill at a
        # random point inside the compaction window
        db.send("--restart")
        delay = rng.uniform(0.0, 0.20)
        time.sleep(delay)
        if db.p.poll() is None:
            db.p.kill()
        db.p.wait()
        db.err.close()
        db = Db(binary, datadir)
        verify_rows(db, rows, dim)
        db.close()
        files = sorted(os.listdir(datadir))
        print("   attempt %d killed after %.0f ms, files=%s" % (attempt + 1, delay * 1000, files))
        shutil.rmtree(datadir, ignore_errors=True)


def recovery_kill_loop(binary, rows, dim, kills):
    print("== recovery kill loop: %d kills ==" % kills)
    datadir = tempfile.mkdtemp(prefix="mvdb-rk-")
    db = Db(binary, datadir)
    db.q("CREATE TABLE big (id BIGINT PRIMARY KEY, grp BIGINT, v VECTOR(%d))" % dim)
    build_rows(db, rows, dim)
    db.kill()
    for _ in range(kills):
        db = Db(binary, datadir)
        db.kill()
    db = Db(binary, datadir)
    verify_rows(db, rows, dim)
    db.close()
    shutil.rmtree(datadir, ignore_errors=True)


def commit_kill_loop(binary, iterations):
    print("== commit-then-kill loop: %d iterations ==" % iterations)
    datadir = tempfile.mkdtemp(prefix="mvdb-ck-")
    db = Db(binary, datadir)
    db.q("CREATE TABLE big (id BIGINT PRIMARY KEY, grp BIGINT, v VECTOR(2))")
    for i in range(iterations):
        if db.q("INSERT INTO big VALUES (%d,0,'[0,0]')" % i).get("affected_rows") != 1:
            FAILS.append("commit %d failed" % i)
        db.kill()
        db = Db(binary, datadir)
        r = db.q("SELECT id FROM big ORDER BY id LIMIT 1000")
        ids = set(row[0] for row in r["rows"])
        chk("committed after kill %d" % i, sorted(ids), list(range(i + 1)))
    db.close()
    shutil.rmtree(datadir, ignore_errors=True)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=os.path.join(here, "..", "build", "minivectordb"))
    ap.add_argument("--quick", action="store_true")
    args = ap.parse_args()
    rows = 20000 if args.quick else 40000
    dim = 64 if args.quick else 128
    attempts = 1 if args.quick else 3

    compaction_window(args.binary, rows, dim, attempts, 7)
    recovery_kill_loop(args.binary, 1000, 16, 3 if args.quick else 10)
    commit_kill_loop(args.binary, 5 if args.quick else 12)

    print("\n%d failures" % len(FAILS))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
