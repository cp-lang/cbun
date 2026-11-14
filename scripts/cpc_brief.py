#!/usr/bin/env python3
"""
Token-saving verifier: compile + ast-check + static gates, compact output.
Usage: python3 scripts/cpc_brief.py [-n 8] <stem.cp> [more.cp ...]
Output per file: PASS/FAIL, error counts grouped by code, first N distinct errors
(code, line, 1-line message, offending source line), then the 4-gate summary.
Never prints the raw cpc output. Writes <stem>.zig (removes *.zig.map).
"""
import argparse
import os
import re
import subprocess
from collections import Counter, OrderedDict

ERR_RE = re.compile(r"^error\[(E\d+)\]: (.*)$")
LOC_RE = re.compile(r"^\s*--> (.+?):(\d+):(\d+)")


def run(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True)


def gates(cp, bak):
    txt = open(cp, encoding="utf-8").read()
    res = OrderedDict()
    res["backup"] = os.path.exists(bak)
    res["zig{}"] = len(re.findall(r"\bzig\s*\{", txt))
    res["var"] = len(re.findall(r"^\s*(?:pub\s+)?var\s", txt, re.M))
    res["tuple.@N"] = len(re.findall(r'\.@"\d+"', txt))
    res["anyopaque"] = len(re.findall(r"\*anyopaque|\*any!", txt))
    if res["backup"]:
        a, b = sum(1 for _ in open(cp, encoding="utf-8")), sum(1 for _ in open(bak, encoding="utf-8"))
        res["ratio"] = round(a / b, 3)
        res["ratio_ok"] = a / b <= 1.2
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-n", type=int, default=8)
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()
    for cp in a.files:
        stem = cp[:-3] if cp.endswith(".cp") else cp
        cp, zig, bak = stem + ".cp", stem + ".zig", stem + "_.zig"
        r = run("cpc --lib %s -o %s" % (cp, zig))
        out = r.stdout + r.stderr
        errs, cur = [], None
        lines = out.splitlines()
        src = open(cp, encoding="utf-8").read().splitlines()
        for ln in lines:
            m = ERR_RE.match(ln)
            if m:
                cur = {"code": m.group(1), "msg": m.group(2), "line": 0}
                errs.append(cur)
                continue
            m = LOC_RE.match(ln)
            if m and cur and not cur["line"]:
                cur["line"] = int(m.group(2))
        status = "PASS"
        if r.returncode != 0 or errs:
            status = "FAIL(cpc)"
        else:
            z = run("vendor/zig/zig ast-check %s" % zig)
            if z.returncode != 0:
                status = "FAIL(ast-check)"
                zl = [l for l in (z.stderr + z.stdout).splitlines() if re.match(r".*\.zig:\d+:\d+: error", l)]
                for l in zl[: a.n]:
                    print("   ", l[:200])
                print("    ast-check errors:", len(zl))
        if os.path.exists(zig + ".map"):
            os.remove(zig + ".map")
        print("== %s: %s" % (cp, status))
        if errs:
            cnt = Counter(e["code"] for e in errs)
            print("   total=%d by code: %s" % (len(errs), ", ".join("%s x%d" % kv for kv in cnt.most_common())))
            seen = set()
            shown = 0
            for e in errs:
                key = (e["code"], e["msg"][:60])
                if key in seen:
                    continue
                seen.add(key)
                s = src[e["line"] - 1].strip()[:110] if 0 < e["line"] <= len(src) else ""
                print("   %s L%d: %s\n        | %s" % (e["code"], e["line"], e["msg"][:110], s))
                shown += 1
                if shown >= a.n:
                    break
        g = gates(cp, bak)
        bad = [k for k, v in g.items() if (k in ("zig{}", "var", "tuple.@N", "anyopaque") and v) or (k == "backup" and not v) or (k == "ratio_ok" and not v)]
        print("   gates: %s%s" % (dict(g), "  VIOLATIONS=%s" % bad if bad else "  OK"))


if __name__ == "__main__":
    main()
