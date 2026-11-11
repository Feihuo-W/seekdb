#!/usr/bin/env python3
"""Out-of-core check: load a dataset several times larger than the process's
resident memory, then run full-table scans, ordering, bulk mutations, restart
and recovery while watching peak RSS (VmHWM). Row bodies must stay in the
file-backed page cache, not in anonymous memory.
"""

import argparse
import json
import os
import random
import select
import subprocess
import sys
import tempfile
import time


class Db:
    def __init__(self, binary, datadir):
        self.p = subprocess.Popen([binary, "--data", datadir],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL)
        self.buf = b""

    def send(self, line):
        self.p.stdin.write(line.encode("utf-8") + b"\n")
        self.p.stdin.flush()

    def read(self, timeout=600):
        fd = self.p.stdout.fileno()
        deadline = time.time() + timeout
        while b"\n" not in self.buf:
            remain = deadline - time.time()
            if remain <= 0:
                raise TimeoutError("no response within %ss" % timeout)
            r, _, _ = select.select([fd], [], [], remain)
            if not r:
                raise TimeoutError("no response within %ss" % timeout)
            chunk = os.read(fd, 1 << 20)
            if not chunk:
                raise EOFError("process exited")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line.decode("utf-8"))

    def q(self, sql, timeout=600):
        self.send(sql)
        return self.read(timeout)

    def hwm_mb(self):
        try:
            with open("/proc/%d/status" % self.p.pid) as f:
                for ln in f:
                    if ln.startswith("VmHWM:"):
                        return int(ln.split()[1]) // 1024
        except OSError:
            pass
        return -1

    def mem_mb(self):
        out = {}
        try:
            with open("/proc/%d/status" % self.p.pid) as f:
                for ln in f:
                    for k in ("VmHWM:", "RssAnon:", "RssFile:"):
                        if ln.startswith(k):
                            out[k[:-1]] = int(ln.split()[1]) // 1024
        except OSError:
            pass
        return out

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(timeout=120)
        except Exception:
            self.p.kill()


def main():
    ap = argparse.ArgumentParser()
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument("--binary", default=os.path.join(here, "..", "build", "minivectordb"))
    ap.add_argument("--rows", type=int, default=3000000)
    ap.add_argument("--dim", type=int, default=128)
    ap.add_argument("--batch", type=int, default=500)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    n, dim = args.rows, args.dim
    datadir = tempfile.mkdtemp(prefix="mvdb-big-")
    rnd = random.Random(42)
    pats = []
    for _ in range(64):
        pats.append("[" + ",".join("%.3f" % rnd.uniform(-1, 1) for _ in range(dim)) + "]")
    qvecs = [pats[i] for i in (3, 17, 40)]

    fails = []

    def check(label, cond, extra=""):
        if cond:
            print("ok   %s %s" % (label, extra), flush=True)
        else:
            fails.append(label)
            print("FAIL %s %s" % (label, extra), flush=True)

    db = Db(args.binary, datadir)
    t0 = time.time()
    r = db.q("CREATE TABLE big (id BIGINT PRIMARY KEY, grp BIGINT, tag VARCHAR(16), v VECTOR(%d))" % dim)
    check("create", r == {}, r)

    ins = 0
    t0 = time.time()
    for base in range(0, n, args.batch):
        cnt = min(args.batch, n - base)
        vals = ",".join("(%d,%d,'t%d','%s')" % (base + i, (base + i) % 1000,
                                               (base + i) % 97, pats[(base + i) % 64])
                        for i in range(cnt))
        r = db.q("INSERT INTO big VALUES " + vals)
        if r != {"affected_rows": cnt}:
            check("insert batch %d" % base, False, r)
            break
        ins += cnt
    dt = time.time() - t0
    check("load %d rows" % ins, ins == n, "%.1fs, %.0f rows/s, hwm=%dMB" % (dt, ins / dt, db.hwm_mb()))

    data_gb = ins * (dim * 4 + 40) / 1e9
    print("     approx row data: %.2f GiB; peak RSS so far: %d MiB" % (data_gb, db.hwm_mb()), flush=True)

    # Full-table unordered scan: streamed, must not materialize in the server.
    t0 = time.time()
    r = db.q("SELECT id FROM big")
    dt = time.time() - t0
    rows = r.get("rows", [])
    ok = len(rows) == n and rows[0] == [0] and rows[-1] == [n - 1]
    check("full scan", ok, "%d rows, %.1fs, hwm=%dMB" % (len(rows), dt, db.hwm_mb()))
    del rows, r

    # ORDER BY with LIMIT: bounded heap.
    t0 = time.time()
    r = db.q("SELECT id, grp FROM big ORDER BY grp DESC LIMIT 100")
    dt = time.time() - t0
    rows = r.get("rows", [])
    ok = len(rows) == 100 and all(rows[i][1] >= rows[i + 1][1] for i in range(99))
    check("order+limit", ok, "%.1fs, hwm=%dMB" % (dt, db.hwm_mb()))

    # ORDER BY without LIMIT: full sort of lightweight refs, streamed output.
    t0 = time.time()
    r = db.q("SELECT id, grp FROM big ORDER BY grp, id")
    dt = time.time() - t0
    rows = r.get("rows", [])
    ok = len(rows) == n
    if ok:
        for i in range(1, n):
            a, b = rows[i - 1], rows[i]
            if (a[1], a[0]) > (b[1], b[0]):
                ok = False
                break
    check("full order", ok, "%d rows, %.1fs, hwm=%dMB" % (len(rows), dt, db.hwm_mb()))
    del rows, r

    # Filtered exact TopK.
    t0 = time.time()
    r = db.q("SELECT id, L2_DISTANCE(v, '%s') AS d FROM big WHERE grp = 7 ORDER BY d LIMIT 10" % qvecs[0])
    dt = time.time() - t0
    rows = r.get("rows", [])
    ok = len(rows) == 10 and all(rows[i][1] <= rows[i + 1][1] + 1e-9 for i in range(9))
    ok = ok and len(set(x[0] for x in rows)) == 10
    check("filtered topk", ok, "%.1fs, hwm=%dMB" % (dt, db.hwm_mb()))

    # Bulk autocommit UPDATE (streams through the WAL).
    upd_n = n // 3
    t0 = time.time()
    r = db.q("UPDATE big SET grp = 1007 WHERE id < %d" % upd_n)
    dt = time.time() - t0
    check("bulk update", r == {"affected_rows": upd_n}, "%.1fs, hwm=%dMB" % (dt, db.hwm_mb()))
    r = db.q("SELECT id, grp FROM big WHERE id = 5")
    check("update visible", r.get("rows") == [[5, 1007]], r)

    # Bulk autocommit DELETE.
    del_n = n // 10
    t0 = time.time()
    r = db.q("DELETE FROM big WHERE id >= %d" % (n - del_n))
    dt = time.time() - t0
    check("bulk delete", r == {"affected_rows": del_n}, "%.1fs, hwm=%dMB" % (dt, db.hwm_mb()))
    alive = n - del_n

    # Restart: shutdown + recovery must reuse /data and stay bounded.
    t0 = time.time()
    db.send("--restart")
    db.p.wait(timeout=300)
    db = Db(args.binary, datadir)
    r = db.q("SELECT id FROM big WHERE id = 0")
    dt = time.time() - t0
    check("restart+recover", r.get("rows") == [[0]], "%.1fs, hwm=%dMB" % (dt, db.hwm_mb()))
    r = db.q("SELECT id FROM big ORDER BY id DESC LIMIT 1")
    check("post-restart max id", r.get("rows") == [[alive - 1]], r)
    r = db.q("SELECT id, grp FROM big WHERE id = 5")
    check("post-restart update", r.get("rows") == [[5, 1007]], r)

    # TopK still exact after mutations and recovery.
    r = db.q("SELECT id, L2_DISTANCE(v, '%s') AS d FROM big WHERE grp = 1007 ORDER BY d LIMIT 5" % qvecs[1])
    rows = r.get("rows", [])
    ok = len(rows) == 5 and all(rows[i][1] <= rows[i + 1][1] + 1e-9 for i in range(len(rows) - 1))
    check("post-restart topk", ok, r if not ok else "")

    hwm = db.hwm_mb()
    mem = db.mem_mb()
    print("     final memory: %s (MiB); data ~%.2f GiB" % (mem, data_gb), flush=True)
    anon = mem.get("RssAnon", hwm)
    check("anonymous RSS bounded vs data", anon < max(600, data_gb * 1024 * 0.5),
          "anon=%dMiB vs data=%.2fGiB" % (anon, data_gb))
    db.close()
    if not args.keep:
        import shutil
        shutil.rmtree(datadir, ignore_errors=True)
    else:
        print("datadir kept:", datadir)
    print("\n%d failures" % len(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())