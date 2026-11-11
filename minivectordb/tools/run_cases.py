#!/usr/bin/env python3
"""Local runner for the public MiniVectorDB mysqltest-style case pack.

Mimics the organiser driver: one SQL statement per line over stdin, one JSON
result per statement on stdout, --restart / --mode directives handled by the
driver.
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

TOL = 1e-5


class Proc:
    def __init__(self, binary, datadir):
        self.err_path = os.path.join(datadir, "stderr.log")
        self.err = open(self.err_path, "ab")
        self.p = subprocess.Popen(
            [binary, "--data", datadir],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self.err,
        )
        self.buf = b""

    def send(self, line):
        self.p.stdin.write(line.encode("utf-8") + b"\n")
        self.p.stdin.flush()

    def read_line(self, timeout=30.0):
        fd = self.p.stdout.fileno()
        deadline = time.time() + timeout
        while True:
            while b"\n" not in self.buf:
                remain = deadline - time.time()
                if remain <= 0:
                    raise TimeoutError("timed out waiting for output")
                r, _, _ = select.select([fd], [], [], remain)
                if not r:
                    raise TimeoutError("timed out waiting for output")
                chunk = os.read(fd, 65536)
                if not chunk:
                    raise EOFError("process exited; stderr:\n" + self.stderr_text())
                self.buf += chunk
            line, self.buf = self.buf.split(b"\n", 1)
            text = line.decode("utf-8", "replace")
            if text.strip() == '{"type":"ready"}':
                continue
            return text

    def stderr_text(self):
        try:
            self.err.flush()
            with open(self.err_path, "rb") as f:
                return f.read().decode("utf-8", "replace")[-4000:]
        except OSError:
            return ""

    def shutdown(self):
        try:
            self.p.stdin.close()
        except OSError:
            pass
        try:
            self.p.wait(timeout=30)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()
        self.err.close()

    def restart(self):
        self.send("--restart")
        try:
            self.p.wait(timeout=30)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()
        self.err.close()


def num_eq(a, b):
    if isinstance(a, bool) or isinstance(b, bool):
        return a == b
    if isinstance(a, int) and isinstance(b, int):
        return a == b
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        diff = abs(a - b)
        return diff <= TOL or diff <= TOL * max(abs(a), abs(b))
    return False


def val_eq(a, b):
    if isinstance(a, str) or isinstance(b, str):
        return a == b
    if isinstance(a, list) or isinstance(b, list):
        if not (isinstance(a, list) and isinstance(b, list)):
            return False
        return len(a) == len(b) and all(val_eq(x, y) for x, y in zip(a, b))
    if a is None or b is None:
        return a is b
    return num_eq(a, b)


def row_eq(r1, r2):
    return len(r1) == len(r2) and all(val_eq(x, y) for x, y in zip(r1, r2))


def compare(expected, actual):
    errs = []
    if "error" in expected:
        if "error" not in actual:
            errs.append("expected error %r, got %r" % (expected["error"], actual))
        elif expected["error"] != actual["error"]:
            errs.append("error mismatch: expected %r, got %r"
                        % (expected["error"], actual["error"]))
        return errs

    if "error" in actual:
        return ["unexpected error %r (expected %r)" % (actual["error"], expected)]

    if "affected_rows" in expected:
        got = actual.get("affected_rows")
        if got != expected["affected_rows"]:
            errs.append("affected_rows: expected %r, got %r"
                        % (expected["affected_rows"], got))

    if "columns" in expected:
        got = actual.get("columns")
        if got != expected["columns"]:
            errs.append("columns: expected %r, got %r"
                        % (expected["columns"], got))

    if "rows" in expected:
        exp_rows = expected["rows"]
        act_rows = actual.get("rows")
        if act_rows is None:
            errs.append("missing rows in result %r" % (actual,))
            return errs
        if expected.get("unordered"):
            remaining = list(act_rows)
            for er in exp_rows:
                for i, ar in enumerate(remaining):
                    if row_eq(er, ar):
                        del remaining[i]
                        break
                else:
                    errs.append("unordered rows: %r not found in %r" % (er, act_rows))
                    break
            if remaining:
                errs.append("unordered rows: extra rows %r" % (remaining,))
        else:
            if len(exp_rows) != len(act_rows):
                errs.append("row count: expected %d, got %d (%r)"
                            % (len(exp_rows), len(act_rows), act_rows))
            for i, (er, ar) in enumerate(zip(exp_rows, act_rows)):
                if not row_eq(er, ar):
                    errs.append("row %d: expected %r, got %r" % (i, er, ar))
                    break
    return errs


def load_cases(cases_dir):
    manifest = os.path.join(cases_dir, "manifest.json")
    if os.path.exists(manifest):
        with open(manifest, encoding="utf-8") as f:
            m = json.load(f)
        return [(c["id"], os.path.join(cases_dir, c["test"]),
                 os.path.join(cases_dir, c["result"])) for c in m["cases"]]
    out = []
    for name in sorted(os.listdir(os.path.join(cases_dir, "cases"))):
        if name.endswith(".test"):
            base = os.path.join(cases_dir, "cases", name[:-5])
            out.append((name[:-5], base + ".test", base + ".result"))
    return out


def run_case(binary, case_id, test_path, result_path, verbose=False):
    with open(test_path, encoding="utf-8") as f:
        lines = [ln.rstrip("\n") for ln in f]
    with open(result_path, encoding="utf-8") as f:
        expected = json.load(f)

    datadir = tempfile.mkdtemp(prefix="mvdb-%s-" % case_id)
    proc = None
    failures = []
    ei = 0
    step = 0
    try:
        proc = Proc(binary, datadir)
        for line in lines:
            stmt = line.strip()
            if not stmt:
                continue
            step += 1
            if stmt.startswith("--restart"):
                proc.restart()
                proc = Proc(binary, datadir)
                continue
            if stmt.startswith("--mode"):
                proc.send(stmt)
                continue
            if ei >= len(expected):
                failures.append("unexpected SQL at step %d: %r" % (step, stmt))
                break
            proc.send(stmt)
            raw = proc.read_line()
            try:
                actual = json.loads(raw)
            except json.JSONDecodeError:
                failures.append("step %d: invalid JSON %r for %r" % (step, raw, stmt))
                break
            if isinstance(actual, dict) and "ok" in actual:
                if actual["ok"] is False:
                    actual = {"error": (actual.get("error") or {}).get("code", "unknown")}
                else:
                    actual = {k: v for k, v in actual.items() if k not in ("ok", "id")}
            errs = compare(expected[ei], actual)
            if errs:
                failures.append("step %d %r: %s" % (step, stmt, "; ".join(errs)))
            ei += 1
        if ei < len(expected):
            failures.append("only %d of %d assertions produced results" % (ei, len(expected)))
        proc.shutdown()
        proc = None
    except (TimeoutError, EOFError, BrokenPipeError, OSError) as exc:
        failures.append("driver error: %s" % (exc,))
        if proc is not None:
            try:
                proc.p.kill()
            except OSError:
                pass
            proc.err.close()
        proc = None
    finally:
        if proc is not None:
            try:
                proc.p.kill()
                proc.err.close()
            except OSError:
                pass
        if failures and verbose:
            print("  data dir kept: %s" % datadir)
        else:
            shutil.rmtree(datadir, ignore_errors=True)

    return failures


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    default_binary = os.path.join(here, "..", "build", "minivectordb")
    default_cases = os.path.join(here, "..", "..", "mysqltest")
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=default_binary)
    ap.add_argument("--cases", default=default_cases)
    ap.add_argument("--filter", default="")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    cases = [c for c in load_cases(args.cases) if args.filter in c[0]]
    failed = 0
    for case_id, test_path, result_path in cases:
        failures = run_case(args.binary, case_id, test_path, result_path, args.verbose)
        if failures:
            failed += 1
            print("FAIL %s" % case_id)
            for f in failures[:6]:
                print("  - %s" % f)
        else:
            print("ok   %s" % case_id)
    print("\n%d/%d cases passed" % (len(cases) - failed, len(cases)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
