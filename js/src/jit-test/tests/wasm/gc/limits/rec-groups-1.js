// |jit-test| include:wasm.js;

loadRelativeToScript("load-mod.js");

// Limit of 1 million recursion groups
wasmValidateBinary(loadMod("wasm-gc-limits-r1M-t1.wasm"));
