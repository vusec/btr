# Attribute each mid-loop JIT compilation of a demo.js util function to the
# go() rep in which it happened, so you can see WHERE warmup is still leaking
# compilations into the measurement loop.
#
# Usage (from attack-surface/spidermonkey/):
#   <run jit_ev.gdb, capture output to jit.log>
#   awk -f tools/jit_churn.awk demo.js jit.log
#
# Reads demo.js FIRST to map each function's start line -> name (so it survives
# demo.js edits), then scans the gdb log. Events are attributed to a rep because
# go() prints "rep=i:" AFTER doing all of rep i's work, so every compile between
# "rep=(i-1):" and "rep=i:" belongs to rep i. Dynamically spawned trainers
# (".. > Function") are the intended per-cycle allocations and are skipped.

# First file: demo.js -> record "function NAME(" start lines.
FNR == NR {
  if ($0 ~ /^function [A-Za-z_]/) {
    name = $2; sub(/\(.*/, "", name); fname[FNR] = name;
  }
  next
}

# Second file: the gdb log.
/rep=0:/ { seen = 1 }

seen && (/\[BASELINE\]/ || /\[TIER-UP\]/) && $0 !~ /> Function/ {
  tag = ($0 ~ /TIER-UP/) ? "TIER-UP" : "BASELINE";
  if (match($0, /demo\.js:[0-9]+/)) {
    ln = substr($0, RSTART + 8, RLENGTH - 8) + 0;
    pend[++np] = sprintf("%-8s demo.js:%d (%s)", tag, ln,
                         (ln in fname) ? fname[ln] : "?");
  }
}

match($0, /rep=[0-9]+:/) {
  r = substr($0, RSTART + 4); sub(/:.*/, "", r);
  for (i = 1; i <= np; i++) printf "rep %-4s  %s\n", r, pend[i];
  np = 0;
}
