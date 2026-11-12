#!/usr/bin/env python3
"""拆分版基准：把 T_query 拆成"无过滤"和"有过滤"两条路径分别计时，
用于定位 1.7x 执行路径开销到底落在哪一段（同样走 JSONL 正式协议）。"""
import argparse, json, os, select, statistics, struct, subprocess, sys, tempfile, time

def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]

class Db:
    def __init__(self, binary, datadir):
        self.err = open(os.path.join(datadir, "stderr.log"), "ab")
        self.p = subprocess.Popen([binary, "--data", datadir], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=self.err)
        self.buf = b""
        self.n = 0
    def send(self, sql):
        req = {"id": self.n, "op": "execute", "sql": sql, "search_mode": "exact"}
        self.n += 1
        self.p.stdin.write(json.dumps(req).encode("utf-8") + b"\n")
        self.p.stdin.flush()
    def read(self, timeout=300.0):
        fd = self.p.stdout.fileno()
        deadline = time.time() + timeout
        while True:
            while b"\n" not in self.buf:
                remain = deadline - time.time()
                if remain <= 0: raise TimeoutError("no output")
                r, _, _ = select.select([fd], [], [], remain)
                if not r: raise TimeoutError("no output")
                chunk = os.read(fd, 1 << 16)
                if not chunk: raise EOFError("process exited")
                self.buf += chunk
            line, self.buf = self.buf.split(b"\n", 1)
            r = json.loads(line.decode("utf-8"))
            if isinstance(r, dict) and r.get("type") == "ready": continue
            return r
    def q(self, sql):
        self.send(sql); return self.read()
    def close(self):
        try: self.p.stdin.close()
        except OSError: pass
        self.p.wait(timeout=120); self.err.close()

def gen(rows, dim, seed):
    import random
    rng = random.Random(seed)
    vecs, cats = [], []
    for i in range(rows):
        vecs.append([f32(rng.uniform(-1, 1)) for _ in range(dim)])
        cats.append("c%d" % (i % 100))
    return vecs, cats

def vec_sql(v):
    return "[" + ",".join(repr(x) for x in v) + "]"

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--rows", type=int, default=10000)
    ap.add_argument("--dim", type=int, default=128)
    ap.add_argument("--queries", type=int, default=500)
    ap.add_argument("--k", type=int, default=10)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--rounds", type=int, default=3)
    args = ap.parse_args()

    import random
    vecs, cats = gen(args.rows, args.dim, args.seed)
    rng = random.Random(args.seed + 1)
    qs = [[f32(rng.uniform(-1, 1)) for _ in range(args.dim)] for _ in range(args.queries)]

    res = {"unfiltered": [], "filtered": [], "parse_only": []}
    for rnd in range(args.rounds):
        d = tempfile.mkdtemp(prefix="mvdb-split-")
        db = Db(args.binary, d)
        db.q("CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), embedding VECTOR(%d))" % args.dim)
        for start in range(0, args.rows, 100):
            sql = "INSERT INTO docs VALUES " + ",".join(
                "(%d,'%s','%s')" % (i, cats[i], vec_sql(vecs[i])) for i in range(start, start + 100))
            db.q(sql)
        # 预热一次过滤查询（触发 scan/过滤列缓存构建），避免把缓存构建算进计时
        db.q("SELECT id, L2_DISTANCE(embedding, '%s') AS distance FROM docs WHERE category = 'c1' "
             "ORDER BY distance LIMIT %d" % (vec_sql(qs[0]), args.k))

        t0 = time.time()
        for q in qs:
            db.q("SELECT id, L2_DISTANCE(embedding, '%s') AS distance FROM docs "
                 "ORDER BY distance LIMIT %d" % (vec_sql(q), args.k))
        res["unfiltered"].append((time.time() - t0) * 1000)

        t0 = time.time()
        for i, q in enumerate(qs):
            db.q("SELECT id, L2_DISTANCE(embedding, '%s') AS distance FROM docs WHERE category = 'c%d' "
                 "ORDER BY distance LIMIT %d" % (vec_sql(q), i % 100, args.k))
        res["filtered"].append((time.time() - t0) * 1000)

        # 纯解析/协议开销：SELECT 常量，不触碰向量列
        t0 = time.time()
        for i in range(args.queries):
            db.q("SELECT id FROM docs WHERE id = %d" % (i % args.rows))
        res["parse_only"].append((time.time() - t0) * 1000)
        db.close()
        print("round %d: unfiltered=%.1fms filtered=%.1fms parse_only=%.1fms" % (
            rnd + 1, res["unfiltered"][-1], res["filtered"][-1], res["parse_only"][-1]), flush=True)

    med = {k: round(statistics.median(v), 2) for k, v in res.items()}
    per_row_un = med["unfiltered"] * 1e-3 / (args.queries * args.rows) * 1e9
    per_row_fi = med["filtered"] * 1e-3 / (args.queries * args.rows) * 1e9
    print(json.dumps(med, indent=2))
    print("无过滤：%.1f ns/行    有过滤：%.1f ns/行（按全表行数摊薄）" % (per_row_un, per_row_fi))
    print("参照：微基准连续数组 AVX2 约 49 ns/行（0.614ms / 1000 查询 / 10000 行）")

if __name__ == "__main__":
    sys.exit(main())
