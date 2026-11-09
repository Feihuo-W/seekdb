#!/usr/bin/env python3
"""Differential fuzz test: random SQL against an independent Python model.

The model tracks the same subset the engine implements (BIGINT/VARCHAR/VECTOR,
primary key, explicit single transaction, filtered TopK). It is intentionally a
separate implementation so that disagreements expose real semantic bugs.
"""

import argparse
import json
import math
import os
import random
import select
import struct
import subprocess
import sys
import tempfile
import time

TOL = 1e-5


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def fmt_vec(vec):
    return "[" + ",".join(repr(v) for v in vec) + "]"


def sql_str(s):
    return "'" + s.replace("'", "''") + "'"


class Model:
    def __init__(self):
        self.tables = {}
        self.active = False
        self.overlay = {}  # (table, pk) -> row tuple or None

    def visible(self, tname):
        t = self.tables[tname]
        out = {}
        for pk, row in t["rows"].items():
            if (tname, pk) in self.overlay:
                continue
            out[pk] = row
        for (tn, pk), row in self.overlay.items():
            if tn == tname and row is not None:
                out[pk] = row
        return out

    def commit(self):
        for (tn, pk), row in self.overlay.items():
            if row is None:
                self.tables[tn]["rows"].pop(pk, None)
            else:
                self.tables[tn]["rows"][pk] = row
        self.overlay.clear()
        self.active = False

    def rollback(self):
        self.overlay.clear()
        self.active = False

    def put(self, tname, pk, row):
        self.overlay[(tname, pk)] = row


def num_add(a, b):
    return abs(a - b) <= TOL or abs(a - b) <= TOL * max(abs(a), abs(b))


def vec_eq(a, b):
    return len(a) == len(b) and all(num_add(x, y) for x, y in zip(a, b))


def cell_eq(a, b):
    if isinstance(a, list) and isinstance(b, list):
        return vec_eq(a, b)
    if isinstance(a, str) or isinstance(b, str):
        return a == b
    return a == b or num_add(a, b)


def row_eq(a, b):
    return len(a) == len(b) and all(cell_eq(x, y) for x, y in zip(a, b))


class Conn:
    def __init__(self, binary, datadir):
        self.binary = binary
        self.datadir = datadir
        self.errpath = os.path.join(datadir, "fuzz-stderr.log")
        self.err = open(self.errpath, "ab")
        self.p = subprocess.Popen([binary, "--data", datadir],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=self.err)
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

    def restart(self):
        self.send("--restart")
        self.p.wait(timeout=30)
        self.err.close()

    def close(self):
        try:
            self.p.stdin.close()
        except OSError:
            pass
        self.p.wait(timeout=30)
        self.err.close()


class Fuzzer:
    def __init__(self, conn, model, rng):
        self.c = conn
        self.m = model
        self.rng = rng
        self.errors = []

    def expect(self, sql, expected, note=""):
        self.c.send(sql)
        actual = self.c.read()
        ok = True
        if "error" in expected:
            if actual.get("error") != expected["error"]:
                ok = False
        else:
            if "error" in actual:
                ok = False
            elif "affected_rows" in expected:
                ok = actual.get("affected_rows") == expected["affected_rows"]
            elif "columns" in expected:
                ok = (actual.get("columns") == expected["columns"] and
                      len(actual.get("rows", [])) == len(expected["rows"]) and
                      all(row_eq(a, b)
                          for a, b in zip(actual.get("rows", []), expected["rows"])))
        if not ok:
            self.errors.append("%s\n  sql: %s\n  expected: %r\n  actual:   %r"
                               % (note, sql, expected, actual))
        return ok

    def bind(self, col, val):
        """Returns SQL literal for a python value of the column type."""
        if col[1] == "BIGINT":
            return str(val)
        if col[1] == "VARCHAR":
            return sql_str(val)
        return sql_str(fmt_vec(val))

    def rand_value(self, col):
        rng = self.rng
        if col[1] == "BIGINT":
            return rng.choice([rng.randint(-1000, 1000), rng.randint(-(2**63), 2**63 - 1)])
        if col[1] == "VARCHAR":
            return "".join(rng.choice(["a", "b", "c", "中", "文"]) for _ in range(rng.randint(0, 3)))
        dim = col[2]
        return tuple(f32(rng.uniform(-2, 2)) for _ in range(dim))

    def where_clause(self, tname):
        t = self.m.tables[tname]
        scalar = [c for c in t["cols"] if c[1] in ("BIGINT", "VARCHAR") and c[0] != "id"]
        if not scalar or self.rng.random() < 0.35:
            return "", lambda row: True
        conds = []
        checks = []
        for _ in range(self.rng.randint(1, 2)):
            col = self.rng.choice(scalar)
            idx = t["cols"].index(col)
            if col[1] == "BIGINT":
                val = self.rng.choice([self.rng.randint(0, 100), self.rng.randint(0, 5)])
                op = self.rng.choice(["=", "<", "<=", ">", ">="])
            else:
                val = self.rng.choice(["a", "b", "c", "中"])
                op = self.rng.choice(["=", "<", "<=", ">", ">="])
            conds.append("%s %s %s" % (col[0], op, self.bind(col, val)))
            checks.append((idx, op, val))
        def pred(row):
            for idx, op, val in checks:
                rv = row[idx]
                if rv is None:
                    return False
                if op == "=" and not (rv == val):
                    return False
                if op == "<" and not (rv < val):
                    return False
                if op == "<=" and not (rv <= val):
                    return False
                if op == ">" and not (rv > val):
                    return False
                if op == ">=" and not (rv >= val):
                    return False
            return True
        return " WHERE " + " AND ".join(conds), pred

    def run(self, steps):
        rng = self.rng
        schema = [("id", "BIGINT", 0),
                  ("grp", "BIGINT", 0),
                  ("name", "VARCHAR", 8),
                  ("emb", "VECTOR", rng.choice([2, 3, 4]))]
        self.m.tables["t"] = {"cols": schema, "rows": {}}
        create = ("CREATE TABLE t (id BIGINT PRIMARY KEY, grp BIGINT, "
                  "name VARCHAR(8), emb VECTOR(%d))" % schema[3][2])
        self.expect(create, {}, "create")

        next_pk = [0]
        for step in range(steps):
            op = rng.choices(
                ["insert", "select", "update", "delete", "txn", "restart"],
                weights=[26, 34, 10, 8, 16, 6])[0]
            if op == "insert":
                n = rng.randint(1, 5)
                rows = []
                cols = [schema[0], schema[1], schema[2], schema[3]]
                new_pk = None
                for _ in range(n):
                    pk = next_pk[0]
                    next_pk[0] += 1
                    new_pk = pk
                    vals = [pk, rng.randint(0, 5),
                            "".join(rng.choice(["a", "b", "c", "中"]) for _ in range(rng.randint(0, 3))),
                            tuple(f32(rng.uniform(-2, 2)) for _ in range(schema[3][2]))]
                    rows.append(vals)
                sql = "INSERT INTO t VALUES " + ",".join(
                    "(" + ",".join(self.bind(cols[i], v[i]) for i in range(4)) + ")"
                    for v in rows)
                visible = self.m.visible("t")
                dup = any(v[0] in visible for v in rows) or len(set(v[0] for v in rows)) != len(rows)
                if dup:
                    self.expect(sql, {"error": "duplicate_key"}, "insert dup")
                else:
                    self.expect(sql, {"affected_rows": len(rows)}, "insert")
                    for v in rows:
                        self.m.put("t", v[0], tuple(v))
                    if not self.m.active:
                        self.m.commit()

            elif op == "select":
                q, pred = self.where_clause("t")
                visible = self.m.visible("t")
                matched = [r for _, r in sorted(visible.items()) if pred(r)]
                want_dist = rng.random() < 0.7
                limit = rng.choice([None, 0, 1, 3, 10])
                if want_dist:
                    dim = schema[3][2]
                    qv = tuple(f32(rng.uniform(-2, 2)) for _ in range(dim))
                    expr = "L2_DISTANCE(emb, %s) AS distance" % sql_str(fmt_vec(qv))
                    cols = ["id", "distance"]
                    rows = []
                    for r in matched:
                        d = math.sqrt(sum((a - b) ** 2 for a, b in zip(r[3], qv)))
                        rows.append([r[0], d])
                    rows.sort(key=lambda x: (x[1], x[0]))
                    sql = "SELECT id, %s FROM t%s ORDER BY distance" % (expr, q)
                    if limit is not None:
                        rows = rows[:limit]
                        sql += " LIMIT %d" % limit
                else:
                    cols = ["id", "grp", "name"]
                    key = rng.choice(["id", "grp", "name"])
                    idx = cols.index(key)
                    desc = rng.random() < 0.25
                    rows = [[r[0], r[1], r[2]] for r in matched]
                    rows.sort(key=lambda x: x[0])
                    rows.sort(key=lambda x: x[idx], reverse=desc)
                    sql = "SELECT id, grp, name FROM t%s ORDER BY %s%s" % (
                        q, key, " DESC" if desc else "")
                    if limit is not None:
                        rows = rows[:limit]
                        sql += " LIMIT %d" % limit
                self.expect(sql, {"columns": cols, "rows": rows}, "select")

            elif op == "update":
                t = self.m.tables["t"]
                q, pred = self.where_clause("t")
                visible = self.m.visible("t")
                target = rng.choice([schema[1], schema[2], schema[3]])
                val = self.rand_value(target)
                matched = [(pk, r) for pk, r in sorted(visible.items()) if pred(r)]
                sql = "UPDATE t SET %s = %s%s" % (target[0], self.bind(target, val), q)
                self.expect(sql, {"affected_rows": len(matched)}, "update")
                idx = schema.index(target)
                for pk, r in matched:
                    nr = list(r)
                    nr[idx] = val
                    self.m.put("t", pk, tuple(nr))
                if not self.m.active:
                    self.m.commit()

            elif op == "delete":
                q, pred = self.where_clause("t")
                visible = self.m.visible("t")
                matched = [pk for pk, r in sorted(visible.items()) if pred(r)]
                self.expect("DELETE FROM t" + q, {"affected_rows": len(matched)}, "delete")
                for pk in matched:
                    self.m.put("t", pk, None)
                if not self.m.active:
                    self.m.commit()

            elif op == "txn":
                if not self.m.active:
                    self.expect("BEGIN", {}, "begin")
                    self.m.active = True
                else:
                    if rng.random() < 0.5:
                        self.expect("COMMIT", {}, "commit")
                        self.m.commit()
                    else:
                        self.expect("ROLLBACK", {}, "rollback")
                        self.m.rollback()

            else:  # restart
                binary, datadir = self.c.binary, self.c.datadir
                self.c.restart()
                self.c = Conn(binary, datadir)
                if self.m.active:
                    # normal shutdown rolls back the open transaction
                    self.m.rollback()
            self.after_step(step, op)

    def after_step(self, step, op):
        pass


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=os.path.join(here, "..", "build", "minivectordb"))
    ap.add_argument("--seeds", type=int, default=5)
    ap.add_argument("--steps", type=int, default=120)
    args = ap.parse_args()

    failures = 0
    for seed in range(args.seeds):
        rng = random.Random(seed)
        datadir = tempfile.mkdtemp(prefix="mvdb-fuzz-")
        conn = Conn(args.binary, datadir)
        model = Model()
        fz = Fuzzer(conn, model, rng)
        try:
            fz.run(args.steps)
        except (TimeoutError, EOFError) as exc:
            try:
                with open(fz.c.errpath, "rb") as f:
                    diag = f.read().decode("utf-8", "replace")[-800:]
            except OSError:
                diag = ""
            fz.errors.append("driver error: %s\n  engine alive=%r stderr=%r"
                             % (exc, fz.c.p.poll() is None, diag))
        try:
            conn.close()
        except (OSError, subprocess.TimeoutExpired):
            pass
        if fz.errors:
            failures += 1
            print("FAIL seed=%d (%d mismatches)" % (seed, len(fz.errors)))
            for e in fz.errors[:4]:
                print("  " + e.replace("\n", "\n  "))
        else:
            print("ok   seed=%d" % seed)
    print("\n%d/%d seeds passed" % (args.seeds - failures, args.seeds))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
