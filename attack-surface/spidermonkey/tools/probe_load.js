// Branch Target Reuse (BTR)
// April 30th 2026
// Yuhui Zhu

// Shared driver for wasm_layout.gdb: compiles+instantiates a 7-function
// leak/target module so a gdb script breaking at WasmJS.cpp:1655 can inspect its
// Tier-1 code layout. The .wasm path is taken from the first script argument
// (default: leak_snippet_x86_64.wasm), so the same driver serves both the
// reference module and any generated target. Run from the tools/ dir:
//   gdb -q -nx -batch -x wasm_layout.gdb  --args "$JS_CMD" probe_load.js leak_snippet_x86_64.wasm
//   gdb -q -nx -batch -x wasm_layout.gdb  --args "$JS_CMD" probe_load.js ../wasm/leak_snippet_x86_64.wasm
var path = (typeof scriptArgs !== "undefined" && scriptArgs.length > 0)
    ? scriptArgs[0] : "leak_snippet_x86_64.wasm";
var wa_bin = os.file.readFile(path, "binary");
var mod = new WebAssembly.Module(wa_bin.buffer);
var instance = new WebAssembly.Instance(mod);
// print(instance.exports.func7(0, 0, 0, 0, 0, 0));
