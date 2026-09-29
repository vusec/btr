// Branch Target Reuse (BTR)
// April 30th 2026
// Yuhui Zhu

// Stage 1 of the PoC flow: show how a trainer/callee reads its arguments off the
// caller-configured stack, so the reused wasm gadget can be built to read the
// SAME slots. This dumps the REAL demo.js F+R callees (dummy_callee_reload1/2) --
// NOT a copy -- without modifying or running demo.js:

//   1. read demo.js source,
//   2. strip its top-level attack entry (everything from `let { cfg, ctx } =
//      init();` on), leaving only the class/function DEFINITIONS,
//   3. evaluate() that + a small harness that builds a Context (which warms
//      dummy_callee_reload1/2 to Ion via warm_dummy_callees, exactly as the
//      attack does) and disnative()s the two callees.

// In the Ion frame the args land at (post-prologue) rbp+0x28 (arg0), rbp+0x30
// (arg1), rbp+0x38 (arg2) -- i.e. rsp+0x20/0x28/0x30 at call-stub entry, which
// is exactly what build_gadget() in make_target_wasm_x86_64.py reads. abi_check.sh
// asserts 0x28(%rbp) and 0x38(%rbp) appear for both callees.

// Usage:  js tools/abi_check.js [path/to/demo.js]

var demoPath = (typeof scriptArgs !== "undefined" && scriptArgs.length > 0)
    ? scriptArgs[0] : "demo.js";

var src = os.file.readFile(demoPath);
var MARKER = "let { cfg, ctx } = init();";
var idx = src.indexOf(MARKER);
if (idx < 0)
  throw new Error("abi_check: entry marker not found in " + demoPath +
                  " ('" + MARKER + "') -- demo.js layout changed");

// Definitions only, then build the runtime the same way the attack does and dump
// the real callees. Config.minimal() with dispatcher_warmup=true keeps setup
// cheap while still running warm_dummy_callees() so the callees reach Ion.
var harness = src.slice(0, idx) + `
;(function abi_dump() {
  let cfg = Config.minimal();
  cfg.dispatcher_warmup = true;
  let ctx = new Context(cfg);
  print("; abi-dump: dummy_callee_reload1 (real demo.js F+R callee)");
  print(disnative(ctx.dummy_callee_reload1));
  print("; abi-dump: dummy_callee_reload2 (real demo.js F+R callee)");
  print(disnative(ctx.dummy_callee_reload2));
})();
`;

evaluate(harness);
