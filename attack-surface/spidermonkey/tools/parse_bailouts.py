#!/usr/bin/env python3

# Branch Target Reuse (BTR)
# April 30th 2026
# Yuhui Zhu

# Bailout analyzer for the BTR SpiderMonkey PoC. It answers one question the reuse
# attack cares about: does the JS driver keep bailing out of Ion DURING the
# measurement loop, or do all the bailouts fall in warmup/setup?
#
# It runs (or re-parses) the driver under the debug shell's bailout spew
#   IONFLAGS=bailouts,bl-bails <js> demo.js ARGS...
# and turns the raw frame-restore dump into a structured report:
#   * one row per bailout EVENT (anchored on "Took bailout!"),
#   * classified by kind (FirstExecution / TranspiledCacheIR / ...),
#   * attributed to a PHASE, and
#   * resolved to the enclosing function name in the source.
#
# Phase attribution. demo.js prints `rep=i:` at the END of iteration i's work
# (see go(): prep_btb + test_btb run, THEN print). So a bailout emitted during
# iteration i shows up BEFORE the `rep=i:` line. The correct label is therefore
# "number of rep= lines seen so far":
#   0 rep lines seen  -> "setup/iter0" (init + force_jit_utils warmup + iter 0)
#   k rep lines seen  -> iteration k is the one in flight
# A trailing bailout after the last rep print is teardown.
#
# Verdict / exit code. Warmup bailouts (FirstExecution while Ion tiers up the
# first few iterations) are benign. The tool flags as SUSPICIOUS, and exits 1:
#   * any bailout at an iteration beyond --warmup (steady-state churn), or
#   * any non-FirstExecution bailout during the measurement loop (iter >= 1).
# Otherwise it exits 0. That makes it drop-in for parameter sweeps / CI.
#
# Usage:
#   JS_CMD=/path/to/js tools/parse_bailouts.py -- \
#       --nr-trainer 256 --nr-bh 4096 --nr-bcond 256 --nr-bcond-taken 64 \
#       --repeats 50 --wa-mod wasm/leak_snippet_x86_64_v128.wasm
#   tools/parse_bailouts.py --log captured.txt          # re-parse a saved log
#   tools/parse_bailouts.py --json -- ...               # machine-readable

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from collections import Counter, defaultdict

# Raw spew line shapes we key on (channel prefix is ignored on purpose).
RE_TOOK = re.compile(r"Took bailout!\s*Snapshot offset:\s*(\d+)")
RE_BAILING = re.compile(r"Bailing to baseline\s+(\S+?):(\d+):(\d+)")
RE_KIND = re.compile(r"Bailout kind:\s*(\w+)")
RE_OUTER = re.compile(
    r"Restored outerScript=\((\S+?):(\d+):(\d+),\d+\).*?bailoutKind=(\d+)"
)
RE_REP = re.compile(r"^rep=(\d+):")

# Definition lines in the source, so a bailout location resolves to a name.
RE_FUNC = re.compile(r"^\s*(?:function\s+)?([A-Za-z_$][\w$]*)\s*\(")
RE_KEYWORDS = {
    "if", "for", "while", "switch", "catch", "return", "function",
    "else", "do", "with", "typeof", "new", "await", "yield",
}


class Event:
    __slots__ = ("phase", "iter", "file", "line", "col", "kind", "kind_num",
                 "snapshot", "func")

    def __init__(self, reps_done):
        self.iter = reps_done
        self.phase = "setup/iter0" if reps_done == 0 else f"iter{reps_done}"
        self.file = None
        self.line = None
        self.col = None
        self.kind = None
        self.kind_num = None
        self.snapshot = None
        self.func = None


def build_func_index(source_path):
    """Sorted [(line_no, name)] of function/method definitions in the source."""
    defs = []
    try:
        with open(source_path, encoding="utf-8", errors="replace") as fh:
            for i, text in enumerate(fh, start=1):
                m = RE_FUNC.match(text)
                if not m:
                    continue
                name = m.group(1)
                if name in RE_KEYWORDS:
                    continue
                defs.append((i, name))
    except OSError:
        return []
    return defs


def resolve_func(defs, line):
    """Enclosing definition name for a 1-based line (nearest preceding def)."""
    if not defs or line is None:
        return None
    best = None
    for ln, name in defs:
        if ln <= line:
            best = name
        else:
            break
    return best


def run_driver(js, script, js_args, save_path):
    """Run the shell under bailout spew; return merged stdout+stderr text."""
    cmd = [js, script, *js_args]
    # Line-buffer stdout so print()s interleave correctly with unbuffered
    # stderr spew in the merged pipe; stderr is already unbuffered.
    if shutil.which("stdbuf"):
        cmd = ["stdbuf", "-oL", "-eL", *cmd]
    env = dict(os.environ)
    env["IONFLAGS"] = "bailouts,bl-bails"
    proc = subprocess.run(
        cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True,
    )
    out = proc.stdout
    if save_path:
        with open(save_path, "w", encoding="utf-8") as fh:
            fh.write(out)
    return out


def parse(lines, defs):
    """Walk merged output; return (events, reps_seen)."""
    events = []
    reps_done = 0
    cur = None

    def finalize(ev):
        if ev is None:
            return
        ev.func = resolve_func(defs, ev.line)
        events.append(ev)

    for text in lines:
        rep = RE_REP.match(text)
        if rep:
            # A rep print closes the current event's window and advances phase.
            finalize(cur)
            cur = None
            reps_done = int(rep.group(1)) + 1
            continue

        m = RE_TOOK.search(text)
        if m:
            finalize(cur)
            cur = Event(reps_done)
            cur.snapshot = int(m.group(1))
            continue

        if cur is None:
            continue

        m = RE_BAILING.search(text)
        if m and cur.line is None:
            cur.file, cur.line, cur.col = m.group(1), int(m.group(2)), int(m.group(3))
            continue

        m = RE_KIND.search(text)
        if m and cur.kind is None:
            cur.kind = m.group(1)
            continue

        m = RE_OUTER.search(text)
        if m:
            # outerScript is the authoritative entry location + numeric kind;
            # prefer it over the first "Bailing to baseline" line.
            cur.file, cur.line, cur.col = m.group(1), int(m.group(2)), int(m.group(3))
            cur.kind_num = int(m.group(4))

    finalize(cur)
    return events, reps_done


def classify(events, warmup):
    """Return (suspicious_events, verdict_string)."""
    suspicious = []
    for ev in events:
        in_loop = ev.iter >= 1
        steady = ev.iter > warmup
        non_warmup_kind = ev.kind not in (None, "FirstExecution")
        if steady or (in_loop and non_warmup_kind):
            suspicious.append(ev)
    return suspicious


def report_text(events, reps_seen, warmup, suspicious):
    out = []
    total = len(events)
    out.append(f"total bailout events: {total}")
    out.append(f"measurement iterations observed (rep= prints): {reps_seen}")
    if total == 0:
        out.append("\nNo bailouts recorded.")
        return "\n".join(out)

    # By kind.
    by_kind = Counter((ev.kind or "?", ev.kind_num) for ev in events)
    out.append("\nby kind:")
    for (kind, num), n in by_kind.most_common():
        out.append(f"  {n:4d}  {kind} (bailoutKind={num})")

    # By phase: setup vs which iteration.
    setup = sum(1 for ev in events if ev.iter == 0)
    loop_iters = sorted({ev.iter for ev in events if ev.iter >= 1})
    last_iter = max((ev.iter for ev in events), default=0)
    out.append("\nby phase:")
    out.append(f"  {setup:4d}  setup/iter0 (init + warmup + iteration 0)")
    if loop_iters:
        rng = f"{loop_iters[0]}..{loop_iters[-1]}"
        out.append(f"  {len(events) - setup:4d}  measurement loop, iterations {rng}")
        out.append(f"        last iteration with a bailout: iter{last_iter}")
    else:
        out.append("     0  measurement loop (clean)")

    # By function / location.
    by_loc = defaultdict(lambda: {"n": 0, "kinds": set(), "iters": set(), "loc": ""})
    for ev in events:
        key = (ev.func or "?", ev.file, ev.line)
        rec = by_loc[key]
        rec["n"] += 1
        if ev.kind:
            rec["kinds"].add(ev.kind)
        rec["iters"].add(ev.iter)
        rec["loc"] = f"{ev.file}:{ev.line}"
    out.append("\nby function:")
    out.append(f"  {'count':>5}  {'function':<24} {'location':<16} {'kinds':<18} iters")
    for (func, _f, _l), rec in sorted(by_loc.items(), key=lambda kv: -kv[1]["n"]):
        iters = sorted(rec["iters"])
        iters_s = f"{iters[0]}..{iters[-1]}" if len(iters) > 1 else str(iters[0])
        kinds_s = ",".join(sorted(rec["kinds"])) or "?"
        out.append(f"  {rec['n']:5d}  {func:<24} {rec['loc']:<16} {kinds_s:<18} {iters_s}")

    # Verdict.
    out.append("")
    if suspicious:
        out.append(f"VERDICT: SUSPICIOUS — {len(suspicious)} bailout(s) past warmup "
                   f"(--warmup {warmup}) or non-FirstExecution inside the loop:")
        for ev in suspicious[:20]:
            out.append(f"  iter{ev.iter} {ev.func} {ev.file}:{ev.line} "
                       f"{ev.kind}(kind={ev.kind_num})")
    else:
        out.append(f"VERDICT: CLEAN — every bailout is warmup/setup "
                   f"(within --warmup {warmup}); the measurement loop is stable.")
    return "\n".join(out)


def report_json(events, reps_seen, warmup, suspicious):
    return json.dumps({
        "total": len(events),
        "iterations_observed": reps_seen,
        "warmup": warmup,
        "suspicious": len(suspicious),
        "verdict": "suspicious" if suspicious else "clean",
        "events": [
            {"iter": ev.iter, "phase": ev.phase, "func": ev.func,
             "file": ev.file, "line": ev.line, "kind": ev.kind,
             "kind_num": ev.kind_num, "snapshot": ev.snapshot}
            for ev in events
        ],
    }, indent=2)


def main():
    ap = argparse.ArgumentParser(
        description="Parse SpiderMonkey Ion bailout spew for the BTR PoC.")
    ap.add_argument("--log", help="parse this saved merged log instead of running")
    ap.add_argument("--js", default=os.environ.get("JS_CMD"),
                    help="js shell binary (default: $JS_CMD)")
    ap.add_argument("--script", default="demo.js",
                    help="JS driver, also the source for name resolution "
                         "(default: demo.js)")
    ap.add_argument("--warmup", type=int, default=10,
                    help="iterations treated as warmup (default: 10)")
    ap.add_argument("--save", help="when running, also write the raw log here")
    ap.add_argument("--json", action="store_true", help="emit JSON instead of text")
    ap.add_argument("js_args", nargs="*",
                    help="args passed to the driver (put after --)")
    args = ap.parse_args()

    defs = build_func_index(args.script)

    if args.log:
        with open(args.log, encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    else:
        if not args.js or not os.access(args.js, os.X_OK):
            ap.error(f"js shell not set/executable: {args.js!r} "
                     "(set $JS_CMD or pass --js)")
        text = run_driver(args.js, args.script, args.js_args, args.save)

    events, reps_seen = parse(text.splitlines(), defs)
    suspicious = classify(events, args.warmup)

    if args.json:
        print(report_json(events, reps_seen, args.warmup, suspicious))
    else:
        print(report_text(events, reps_seen, args.warmup, suspicious))

    sys.exit(1 if suspicious else 0)


if __name__ == "__main__":
    main()
