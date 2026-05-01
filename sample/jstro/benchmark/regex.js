// jstro loses — V8 has Irregexp, a regex JIT.  We have a small
// NFA backtracker.  jstro AOT ~0.011 s vs node v18 ~0.004 s — 2.7×.
var pattern = /a(b+)c/;
var str = "xxabbbbbbbbbbbcyyyabcabbbbbbbbbbbbbcz";
var start = Date.now();
var n = 0;
for (var i = 0; i < 100000; i++) {
  if (pattern.test(str)) n++;
}
var elapsed = (Date.now() - start) / 1000;
console.log("n = " + n);
console.log("elapsed: " + elapsed + "s");
