#!/usr/bin/env python3
"""Hostile-input driver check: every input line must yield exactly one JSON
response (or none, for comment/directive lines) without hanging.

This mirrors the platform failure mode "JSON response timeout: no JSON response
received in time (check newlines, flush and process blocking)".
"""

import json
import os
import select
import subprocess
import sys
import tempfile
import time


class Db:
    def __init__(self, binary, datadir):
        self.p = subprocess.Popen([binary, "--data", datadir], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.buf = b""

    def send(self, line):
        self.p.stdin.write(line.encode("utf-8") + b"\n")
        self.p.stdin.flush()

    def read(self, timeout):
        fd = self.p.stdout.fileno()
        deadline = time.time() + timeout
        while True:
            while b"\n" not in self.buf:
                remain = deadline - time.time()
                if remain <= 0:
                    return None
                r, _, _ = select.select([fd], [], [], remain)
                if not r:
                    return None
                chunk = os.read(fd, 1 << 16)
                if not chunk:
                    return None
                self.buf += chunk
            line, self.buf = self.buf.split(b"\n", 1)
            r = json.loads(line.decode("utf-8"))
            if isinstance(r, dict) and r.get("type") == "ready":
                continue
            if isinstance(r, dict) and "ok" in r:
                if r["ok"] is False:
                    return {"error": (r.get("error") or {}).get("code", "unknown")}
                return {k: v for k, v in r.items() if k not in ("ok", "id")}
            return r


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    binary = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "build", "minivectordb")
    datadir = tempfile.mkdtemp(prefix="mvdb-hostile-")
    db = Db(binary, datadir)
    fails = []

    def expect_response(label, line, timeout=2.0):
        db.send(line)
        r = db.read(timeout)
        if r is None:
            fails.append("%s: no JSON response within %.1fs for %r" % (label, timeout, line[:80]))
            print("FAIL", label, "-> no response")
        else:
            print("ok  ", label, "->", json.dumps(r)[:90])

    def expect_no_response(label, line, timeout=0.6):
        db.send(line)
        r = db.read(timeout)
        if r is not None:
            fails.append("%s: unexpected response %r for %r" % (label, r, line))
            print("FAIL", label, "-> unexpected response")

    db.send("CREATE TABLE t (id BIGINT PRIMARY KEY, grp BIGINT, s VARCHAR(16), v VECTOR(2))")
    db.read(5)
    db.send("INSERT INTO t VALUES (1,1,'a','[0,0]')")
    db.read(5)

    expect_response("unterminated quote", "SELECT id FROM t WHERE s = 'unterminated")
    expect_response("unterminated paren", "SELECT (id FROM t")
    expect_response("extra close paren", "SELECT id FROM t WHERE id = 1)")
    expect_response("line comment with apostrophe", "SELECT id FROM t WHERE id = 1 -- don't")
    expect_response("block comment with apostrophe", "SELECT id FROM t /* don't */ WHERE id = 1")
    expect_response("trailing AND", "SELECT id FROM t WHERE id = 1 AND")
    expect_response("incomplete ORDER BY", "SELECT id FROM t ORDER BY")
    expect_response("syntax error", "SELEC id FROM t")
    expect_response("unsupported group by", "SELECT id FROM t GROUP BY id")
    expect_response("semicolon inside string", "SELECT id FROM t WHERE s = 'a;b'")
    expect_response("semicolon terminated", "SELECT id FROM t;")
    expect_response("duplicate key", "INSERT INTO t VALUES (1,1,'a','[0,0]')")
    expect_response("huge statement", "INSERT INTO t VALUES (999,9,'" +
                    ",".join(["0.5"] * 400) + "')")
    expect_response("still alive", "SELECT id FROM t WHERE id = 1")
    expect_no_response("hash comment", "# just a comment")
    expect_no_response("dash comment", "-- just a comment")
    expect_response("alive after comments", "SELECT id FROM t WHERE id = 1")

    # Directives occupy no assertion slot: the driver reads no answer for them,
    # and a stray answer would desynchronize every following statement.
    expect_no_response("directive silence", "--mode ann", timeout=1.5)

    # A line that carries no statement must still be answered, not swallowed.
    expect_response("bare semicolon", ";")
    expect_response("comment only", "/* nothing */")

    # Steady pipelining of lines that look unterminated: each line is complete
    # per the protocol and must be answered immediately, in order.
    db.send("SELECT id FROM t WHERE s = 'unterminated")
    db.send("SELECT id FROM t WHERE id = 1")
    db.send("SELECT (id FROM t")
    db.send("SELECT id FROM t WHERE id = 1")
    r1 = db.read(2.0)
    r2 = db.read(2.0)
    r3 = db.read(2.0)
    r4 = db.read(2.0)
    if r1 is None or r2 is None or r3 is None or r4 is None:
        fails.append("pipeline: missing answer (%r %r %r %r)" % (r1, r2, r3, r4))
        print("FAIL pipeline -> missing answer")
    elif r2.get("rows") != [[1]] or r4.get("rows") != [[1]]:
        fails.append("pipeline: answers out of order (%r %r)" % (r2, r4))
        print("FAIL pipeline -> out of order")
    else:
        print("ok   pipeline -> four ordered answers")

    # A driver that pipelines a directive must not receive a stray answer.
    db.send("--mode ann")
    db.send("SELECT id FROM t WHERE id = 1")
    r = db.read(2.0)
    if r is None or r.get("rows") != [[1]]:
        fails.append("directive pipeline: expected the query answer, got %r" % (r,))
        print("FAIL directive pipeline ->", r)
    else:
        print("ok   directive pipeline ->", json.dumps(r)[:60])
    stray = db.read(0.6)
    if stray is not None:
        fails.append("directive pipeline: stray extra answer %r" % (stray,))
        print("FAIL directive pipeline -> stray", stray)
    else:
        print("ok   directive pipeline -> no stray answer")

    print("\n%d failures" % len(fails))
    for f in fails:
        print("FAIL " + f)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
