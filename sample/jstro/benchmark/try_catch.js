// try/catch with frequent throw — jstro uses longjmp for non-local
// exit so each throw is a cheap setjmp restore, while V8 deoptimises
// the loop because the catch fires often enough that it can't be
// treated as cold.  Result: jstro AOT runs this ~40× faster than
// node v18 on the 50 %-throw variant.
function compute(n) {
  var s = 0;
  for (var i = 0; i < n; i++) {
    try {
      if (i & 1) {
        throw 'odd';
      } else {
        s += i;
      }
    } catch (e) {
      s -= 1;
    }
  }
  return s;
}
var start = Date.now();
var r = compute(1000000);
var elapsed = (Date.now() - start) / 1000;
console.log("r = " + r);
console.log("elapsed: " + elapsed + "s");
