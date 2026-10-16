const { runInThisContext } = require("node:vm");
const each = runInThisContext("(cb, n) => { let s = 0; for (let i = 0; i < n; i++) s += cb(i); return s }");
const t0 = performance.now();
const s = each((i) => i, 1000000);
console.log(s, Math.round(performance.now() - t0));
