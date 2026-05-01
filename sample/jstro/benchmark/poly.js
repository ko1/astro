// 6-shape polymorphic property access.  V8 has a 4-way IC + a
// megamorphic dictionary cache.  jstro has only a 1-way (monomorphic)
// IC so this site is fully megamorphic for us — every access calls
// js_shape_find_slot.  jstro AOT ~0.23 s vs node v18 ~0.06 s — 3.8×.
function S0(x) { this.a = x; }
function S1(x) { this.b = x; }
function S2(x) { this.c = x; }
function S3(x) { this.d = x; }
function S4(x) { this.e = x; }
function S5(x) { this.f = x; }
function get(obj) {
  if (obj.a !== undefined) return obj.a;
  if (obj.b !== undefined) return obj.b;
  if (obj.c !== undefined) return obj.c;
  if (obj.d !== undefined) return obj.d;
  if (obj.e !== undefined) return obj.e;
  return obj.f;
}
var arr = [];
for (var i = 0; i < 6000; i++) {
  if (i % 6 === 0) arr.push(new S0(i));
  else if (i % 6 === 1) arr.push(new S1(i));
  else if (i % 6 === 2) arr.push(new S2(i));
  else if (i % 6 === 3) arr.push(new S3(i));
  else if (i % 6 === 4) arr.push(new S4(i));
  else arr.push(new S5(i));
}
var start = Date.now();
var s = 0;
for (var k = 0; k < 200; k++) {
  for (var i = 0; i < arr.length; i++) s += get(arr[i]);
}
var elapsed = (Date.now() - start) / 1000;
console.log("s = " + s);
console.log("elapsed: " + elapsed + "s");
