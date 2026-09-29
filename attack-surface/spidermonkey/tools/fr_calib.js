// Branch Target Reuse (BTR)
// April 30th 2026
// Yuhui Zhu

// FLUSH+RELOAD timing calibration: measure what a cache hit and a cache miss
// actually cost on THIS host, so the hit/miss decision threshold used by the BTB
// stage is a measured number instead of a compiled-in constant.

// demo.js decides "the probe line was brought in" with `res[j] < FR_THRESHOLD`
// (a per-slot reload time in nanoseconds, produced by the reloadbufoffset
// builtin, which times a single load with clock_gettime(CLOCK_REALTIME)). That
// comparison is only meaningful while FR_THRESHOLD sits in the gap between the
// two distributions, and the gap moves with the host, the clock source, the
// core frequency and the build type. This tool measures both distributions and
// reports the gap.

// It reads the REAL demo.js -- NOT a copy -- the same way tools/abi_check.js
// does: read the source, strip its top-level attack entry (everything from
// `let { cfg, ctx } = init();` on), and evaluate() the remaining definitions.
// FR_CACHE_LINE, CACHE_LINE_SZ, PAGE_SZ, FR_THRESHOLD and the Context that owns
// the F+R buffer are therefore the ones the attack itself uses, and the probe
// offset is built with go()'s own expression.

// Two sample sets, collected round-robin (one hit then one miss per rep) so
// frequency drift and thermal noise land on both sets equally:
//   hit  : touch the probe line, then time a reload of it.
//   miss : flushbufoffset() the line, then time a reload of it.

// Output: per-set n / min / max / mean / variance / stddev / MAD / percentiles
// and a value histogram, then the derived separation and the recommended
// threshold. The last line is `FR_CALIB_JSON={...}` for fr_calib.sh to capture.

// Usage:  js tools/fr_calib.js [path/to/demo.js] [--samples N] [--warmup N]

var demoPath = "demo.js";
var SAMPLES = 20000;
var WARMUP = 1000;

for (var i = 0; i < scriptArgs.length; i++) {
  var a = scriptArgs[i];
  if (a === "--samples" && i + 1 < scriptArgs.length) SAMPLES = parseInt(scriptArgs[++i]);
  else if (a === "--warmup" && i + 1 < scriptArgs.length) WARMUP = parseInt(scriptArgs[++i]);
  else demoPath = a;
}

// --- statistics -------------------------------------------------------------

// Nearest-rank percentile over an ascending-sorted array.
function pct(sorted, p) {
  let k = Math.ceil(p * sorted.length) - 1;
  return sorted[Math.min(sorted.length - 1, Math.max(0, k))];
}

// Everything the report needs about one sample set, computed once.
function stats(samples) {
  let a = samples.slice().sort((x, y) => x - y);
  let n = a.length;
  let sum = 0;
  for (let i = 0; i < n; i++) sum += a[i];
  let mean = sum / n;
  let sq = 0;
  for (let i = 0; i < n; i++) sq += (a[i] - mean) * (a[i] - mean);
  let variance = sq / n;
  let median = pct(a, 0.5);
  let dev = a.map(v => Math.abs(v - median)).sort((x, y) => x - y);
  let counts = new Map();
  for (let i = 0; i < n; i++) counts.set(a[i], (counts.get(a[i]) || 0) + 1);
  return {
    sorted: a, counts: counts, n: n,
    min: a[0], max: a[n - 1],
    mean: mean, variance: variance, stddev: Math.sqrt(variance),
    mad: pct(dev, 0.5),
    p001: pct(a, 0.001), p01: pct(a, 0.01), p05: pct(a, 0.05),
    p25: pct(a, 0.25), p50: median, p75: pct(a, 0.75),
    p95: pct(a, 0.95), p99: pct(a, 0.99), p999: pct(a, 0.999),
  };
}

// Samples strictly below t (the "looks like a hit" side of the decision).
function countBelow(sorted, t) {
  let lo = 0, hi = sorted.length;
  while (lo < hi) {
    let mid = (lo + hi) >> 1;
    if (sorted[mid] < t) lo = mid + 1; else hi = mid;
  }
  return lo;
}

// --- reporting --------------------------------------------------------------

function f2(x) { return x.toFixed(2); }

function print_stats(label, s) {
  print(`[frcalib] ${label}: n=${s.n} min=${s.min} max=${s.max} mean=${f2(s.mean)} ` +
        `var=${f2(s.variance)} sd=${f2(s.stddev)} mad=${s.mad}`);
  print(`[frcalib] ${label}: p0.1=${s.p001} p1=${s.p01} p5=${s.p05} p25=${s.p25} ` +
        `p50=${s.p50} p75=${s.p75} p95=${s.p95} p99=${s.p99} p99.9=${s.p999}`);
}

// The clock quantizes reload times into coarse steps, so the value counts
// describe the distribution's shape better than fine percentiles do. The rows
// span [min, p99] at a bucket width picked off a 1/2/5/10/... ladder so the
// body fits in ~MAX_ROWS lines; the slow 1% is folded into a single tail row,
// keeping one outlier from stretching the whole axis.
const MAX_ROWS = 24;

function bucket_width(span) {
  const LADDER = [1, 2, 5, 10, 20, 25, 50, 100, 200, 500, 1000];
  for (let bw of LADDER) if (span / bw <= MAX_ROWS) return bw;
  return Math.ceil(span / MAX_ROWS);
}

function print_hist(label, s) {
  let hi = s.p99;
  let bw = bucket_width(hi - s.min);
  let lo = Math.floor(s.min / bw) * bw;
  let buckets = new Map();
  let tail = 0;
  for (let [v, c] of s.counts) {
    if (v > hi) { tail += c; continue; }
    let b = Math.floor(v / bw) * bw;
    buckets.set(b, (buckets.get(b) || 0) + c);
  }
  let peak = Math.max(...buckets.values());
  print(`[frcalib] ${label} histogram (bucket=${bw}ns, ${s.counts.size} distinct values):`);
  for (let b = lo; b <= hi; b += bw) {
    let c = buckets.get(b) || 0;
    if (c === 0) continue;
    let bar = "#".repeat(Math.max(1, Math.round(40 * c / peak)));
    print(`[frcalib]   ${String(b).padStart(6)} ${String(c).padStart(7)} ` +
          `${(100 * c / s.n).toFixed(2).padStart(6)}%  ${bar}`);
  }
  if (tail > 0) {
    print(`[frcalib]   ${(">" + hi).padStart(6)} ${String(tail).padStart(7)} ` +
          `${(100 * tail / s.n).toFixed(2).padStart(6)}%  (tail up to ${s.max}ns)`);
  }
}

// Misclassification of one threshold: miss samples that read as hits, and hit
// samples that read as misses.
function errors_at(hit, miss, t) {
  let false_hit = countBelow(miss.sorted, t);          // miss sample read as a hit
  let false_miss = hit.n - countBelow(hit.sorted, t);  // hit sample read as a miss
  return {
    threshold: t,
    false_hit: false_hit, false_hit_pct: 100 * false_hit / miss.n,
    false_miss: false_miss, false_miss_pct: 100 * false_miss / hit.n,
    err_pct: 100 * (false_hit + false_miss) / (miss.n + hit.n),
  };
}

function print_errors(label, e) {
  print(`[frcalib] ${label} t=${e.threshold}: miss-read-as-hit=${e.false_hit} ` +
        `(${f2(e.false_hit_pct)}%) hit-read-as-miss=${e.false_miss} ` +
        `(${f2(e.false_miss_pct)}%) total-err=${f2(e.err_pct)}%`);
}

// --- demo.js definitions, then the measurement ------------------------------

var src = os.file.readFile(demoPath);
var MARKER = "let { cfg, ctx } = init();";
var idx = src.indexOf(MARKER);
if (idx < 0)
  throw new Error("fr_calib: entry marker not found in " + demoPath +
                  " ('" + MARKER + "') -- demo.js layout changed");

var harness = src.slice(0, idx) + `
;(function fr_calib() {
  const SAMPLES = ${SAMPLES}, WARMUP = ${WARMUP};

  // Config.minimal() keeps setup cheap: the calibration times the F+R buffer
  // alone, so it leaves dispatcher_warmup off and touches neither the
  // dispatcher nor the dummy callees. Context still owns the real frbuf.
  let cfg = Config.minimal();
  let ctx = new Context(cfg);
  let frbuf = ctx.frbuf, u8 = ctx.fru8array;

  // go()'s own probe offset, and go()'s residency pre-touch (see init()).
  let off = (FR_CACHE_LINE * CACHE_LINE_SZ) + 0x3025;
  for (let i = 0; i < u8.length; i += PAGE_SZ) u8[i] = 0xff;

  print("[frcalib] demo.js=${demoPath} FR_CACHE_LINE=" + FR_CACHE_LINE +
        " probe_offset=" + off + " (0x" + off.toString(16) + ")" +
        " buffer=0x" + u8.length.toString(16) + "B");
  print("[frcalib] samples=" + SAMPLES + " warmup=" + WARMUP +
        " demo.js FR_THRESHOLD=" + FR_THRESHOLD + "ns");

  let hit = [], miss = [];
  for (let i = 0; i < WARMUP + SAMPLES; i++) {
    let keep = (i >= WARMUP);
    // hit: the line is resident (the previous reload wrote it back), and the
    // touch below keeps that true on the very first rep too.
    u8[off] |= 1;
    let t_hit = reloadbufoffset(frbuf, off);
    // miss: evict the line, then pay for bringing it back.
    flushbufoffset(frbuf, off);
    let t_miss = reloadbufoffset(frbuf, off);
    if (keep) { hit.push(t_hit); miss.push(t_miss); }
  }

  report(hit, miss, FR_THRESHOLD, off, SAMPLES, WARMUP);
})();
`;

// Turn the two sample sets into the human report plus the JSON line. Defined
// out here (not in the harness) so it is plain tooling code, not demo.js scope.
function report(hitSamples, missSamples, demoThreshold, probeOffset, samples, warmup) {
  let hit = stats(hitSamples), miss = stats(missSamples);

  print_stats("hit ", hit);
  print_hist("hit ", hit);
  print_stats("miss", miss);
  print_hist("miss", miss);

  // Separation: the band between the slowest hits and the fastest misses. The
  // miss side uses p0.1 rather than min because the miss tail's fast end is as
  // noisy as its slow end.
  let gap_lo = hit.p999, gap_hi = miss.p001, gap = gap_hi - gap_lo;
  let recommended, rule;
  if (gap > 0) {
    recommended = Math.round((gap_lo + gap_hi) / 2);
    rule = "midpoint of [hit p99.9, miss p0.1]";
  } else {
    // Overlapping distributions: fall back to the value with the fewest
    // misclassified samples.
    let best = null;
    for (let t = hit.min; t <= miss.max; t++) {
      let e = errors_at(hit, miss, t);
      if (best === null || e.err_pct < best.err_pct) best = e;
    }
    recommended = best.threshold;
    rule = "min-error scan (distributions overlap)";
  }

  let dprime = (miss.mean - hit.mean) /
               Math.sqrt((hit.variance + miss.variance) / 2);
  let e_rec = errors_at(hit, miss, recommended);
  let e_demo = errors_at(hit, miss, demoThreshold);

  print(`[frcalib] separation: hit p99.9=${gap_lo}ns .. miss p0.1=${gap_hi}ns ` +
        `gap=${gap}ns  d'=${f2(dprime)}`);
  print(`[frcalib] recommended threshold=${recommended}ns (${rule})`);
  print_errors("recommended", e_rec);
  print_errors("demo.js    ", e_demo);
  print(`[frcalib] demo.js FR_THRESHOLD=${demoThreshold}ns is ` +
        (demoThreshold > gap_lo && demoThreshold < gap_hi
          ? "INSIDE the separation band" : "OUTSIDE the separation band"));

  print("FR_CALIB_JSON=" + JSON.stringify({
    samples: samples, warmup: warmup, probe_offset: probeOffset,
    demo_threshold: demoThreshold, recommended: recommended, rule: rule,
    gap_lo: gap_lo, gap_hi: gap_hi, gap: gap, dprime: dprime,
    err_pct_recommended: e_rec.err_pct, err_pct_demo: e_demo.err_pct,
    false_hit_pct: e_rec.false_hit_pct, false_miss_pct: e_rec.false_miss_pct,
    hit: summary(hit), miss: summary(miss),
  }));
}

// The stats worth carrying in the JSON (drops the raw samples and histogram).
function summary(s) {
  return {
    n: s.n, min: s.min, max: s.max, mean: s.mean, variance: s.variance,
    stddev: s.stddev, mad: s.mad,
    p001: s.p001, p01: s.p01, p05: s.p05, p25: s.p25, p50: s.p50,
    p75: s.p75, p95: s.p95, p99: s.p99, p999: s.p999,
  };
}

evaluate(harness);
