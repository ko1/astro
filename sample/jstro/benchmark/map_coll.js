// jstro loses badly here — V8's Map is a hash table tuned for
// string keys; ours is a linear scan.  Kept honest: jstro AOT ~24 s
// vs node v18 ~0.06 s — 400× behind.  Fixing this would mean writing
// a real hash-bucket Map in js_stdlib.c.
var m = new Map();
var start = Date.now();
for (var i = 0; i < 100000; i++) {
  m.set("key" + i, i);
}
var sum = 0;
for (var i = 0; i < 100000; i++) {
  sum += m.get("key" + i) || 0;
}
var elapsed = (Date.now() - start) / 1000;
console.log("size = " + m.size + " sum = " + sum);
console.log("elapsed: " + elapsed + "s");
