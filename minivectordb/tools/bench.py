#!/usr/bin/env python3
"""MiniVectorDB performance benchmark on the official workload shape.

10,000 rows x 128-dim vectors, batches of 100 committed inserts, then 1,000
TopK queries (K=10), half of them filtered on an ordinary column equality.
Reports wall time, block I/O, peak RSS and sampled Recall@10, plus a restart
persistence check. Multiple rounds are summarised by median.
"""

import argparse
import json
import math
import os
import select
import statistics
import struct
import subprocess
import sys
import tempfile
import time


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


class Db:
    def __init__(self, binary, datadir):
        self.binary = binary
        self.datadir = datadir
        self.errpath = os.path.join(datadir, "bench-stderr.log")
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

    def proc_io(self):
        out = {}
        try:
            with open("/proc/%d/io" % self.p.pid) as f:
                for ln in f:
                    k, v = ln.split(":")
                    out[k] = int(v)
        except OSError:
            pass
        return out

    def proc_hwm(self):
        try:
            with open("/proc/%d/status" % self.p.pid) as f:
                for ln in f:
                    if ln.startswith("VmHWM"):
                        return int(ln.split()[1])
        except OSError:
            pass
        return 0

    def restart(self):
        self.send("--restart")
        self.p.wait(timeout=120)
        self.err.close()
        return Db(self.binary, self.datadir)

    def close(self):
        try:
            self.p.stdin.close()
        except OSError:
            pass
        self.p.wait(timeout=120)
        self.err.close()


def gen_data(rows, dim, seed):
    import random
    rng = random.Random(seed)
    vecs = []
    cats = []
    for i in range(rows):
        vecs.append([f32(rng.uniform(-1, 1)) for _ in range(dim)])
        cats.append("c%d" % (i % 100))
    return vecs, cats


def gen_queries(vecs, rows, dim, count, seed):
    import random
    rng = random.Random(seed + 1)
    out = []
    for i in range(count):
        q = [f32(rng.uniform(-1, 1)) for _ in range(dim)]
        filt = ("c%d" % (i % 100)) if i % 2 == 1 else None
        out.append((q, filt))
    return out


def vec_sql(v):
    return "[" + ",".join(repr(x) for x in v) + "]"


def brute_topk(vecs, cats, q, filt, k):
    cand = []
    for i, v in enumerate(vecs):
        if filt is not None and cats[i] != filt:
            continue
        d = math.sqrt(sum((a - b) ** 2 for a, b in zip(v, q)))
        cand.append((d, i))
    cand.sort()
    return [i for _, i in cand[:k]]


def run_round(binary, rows, dim, queries, k, seed, recall_sample):
    datadir = tempfile.mkdtemp(prefix="mvdb-bench-")
    db = Db(binary, datadir)
    vecs, cats = gen_data(rows, dim, seed)
    qs = gen_queries(vecs, rows, dim, queries, seed)

    db.q("CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), embedding VECTOR(%d))" % dim)
    io0 = db.proc_io()
    t0 = time.time()
    batch = 100
    for start in range(0, rows, batch):
        sql = "INSERT INTO docs VALUES " + ",".join(
            "(%d,'%s','%s')" % (i, cats[i], vec_sql(vecs[i])) for i in range(start, start + batch))
        r = db.q(sql)
        if r.get("affected_rows") != batch:
            raise RuntimeError("insert failed: %r" % (r,))
    t_write = time.time() - t0

    t0 = time.time()
    results = []
    for q, filt in qs:
        where = " WHERE category = '%s'" % filt if filt else ""
        sql = ("SELECT id, L2_DISTANCE(embedding, '%s') AS distance FROM docs%s "
               "ORDER BY distance LIMIT %d" % (vec_sql(q), where, k))
        r = db.q(sql)
        results.append(r)
    t_query = time.time() - t0

    # correctness: shape + sampled recall against brute force
    recalls = []
    ok = True
    for idx, r in enumerate(results):
        if "rows" not in r:
            ok = False
            break
        ids = [row[0] for row in r["rows"]]
        if len(ids) != min(k, rows) or len(set(ids)) != len(ids):
            ok = False
            break
        q, filt = qs[idx]
        if filt is not None and any(cats[i] != filt for i in ids):
            ok = False
            break
    step = max(1, queries // recall_sample)
    for idx in range(0, queries, step):
        q, filt = qs[idx]
        truth = brute_topk(vecs, cats, q, filt, k)
        got = [row[0] for row in results[idx]["rows"]]
        recalls.append(len(set(truth) & set(got)) / max(1, len(truth)))

    io1 = db.proc_io()
    mem_kb = db.proc_hwm()
    io_bytes = (io1.get("read_bytes", 0) - io0.get("read_bytes", 0) +
                io1.get("write_bytes", 0) - io0.get("write_bytes", 0))

    # persistence across a clean restart
    db = db.restart()
    persist = (db.q("SELECT id FROM docs WHERE id=0")["rows"] == [[0]] and
               db.q("SELECT id FROM docs WHERE id=%d" % (rows - 1))["rows"] == [[rows - 1]])
    q0, filt0 = qs[0]
    sql = "SELECT id FROM docs ORDER BY id LIMIT 1"
    persist = persist and db.q(sql)["rows"] == [[0]]
    db.close()

    return {
        "t_write_ms": round(t_write * 1000, 3),
        "t_query_ms": round(t_query * 1000, 3),
        "io_bytes": io_bytes,
        "mem_peak_kb": mem_kb,
        "correct": ok,
        "recall_min": round(min(recalls), 6) if recalls else None,
        "recall_mean": round(statistics.mean(recalls), 6) if recalls else None,
        "persistence": persist,
    }


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=os.path.join(here, "..", "build", "minivectordb"))
    ap.add_argument("--rows", type=int, default=10000)
    ap.add_argument("--dim", type=int, default=128)
    ap.add_argument("--queries", type=int, default=1000)
    ap.add_argument("--k", type=int, default=10)
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--recall-sample", type=int, default=10)
    ap.add_argument("--out", default="")
    args = ap.parse_args()

    rounds = []
    for i in range(args.rounds):
        r = run_round(args.binary, args.rows, args.dim, args.queries, args.k,
                      args.seed, args.recall_sample)
        rounds.append(r)
        print("round %d: %s" % (i + 1, json.dumps(r)), flush=True)
        if not r["correct"] or not r["persistence"]:
            print("CORRECTNESS FAILURE", file=sys.stderr)
            return 2

    summary = {
        "workload": {"rows": args.rows, "dim": args.dim, "queries": args.queries,
                     "k": args.k, "rounds": args.rounds, "seed": args.seed},
        "rounds": rounds,
        "median": {
            "t_write_ms": statistics.median(r["t_write_ms"] for r in rounds),
            "t_query_ms": statistics.median(r["t_query_ms"] for r in rounds),
            "io_bytes": statistics.median(r["io_bytes"] for r in rounds),
            "mem_peak_kb": statistics.median(r["mem_peak_kb"] for r in rounds),
        },
        "recall_min": min(r["recall_min"] for r in rounds),
        "correct": all(r["correct"] for r in rounds),
        "persistence": all(r["persistence"] for r in rounds),
    }
    print(json.dumps(summary, indent=2))
    if args.out:
        with open(args.out, "w") as f:
            json.dump(summary, f, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
