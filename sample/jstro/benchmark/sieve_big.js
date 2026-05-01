// Sieve of Eratosthenes at a size where V8 falls off its array
// element-kind fast path.  jstro keeps a flat 8-bytes-per-element
// JsValue layout regardless of size, so the array-write inner loop
// is constant-cost; V8 transitions large arrays of holey boolean
// values into a much slower representation and pays heavy young-gen
// GC at this scale.  Combined with our other optimisations (inline
// 4-slot JsObject, inlined safepoint, encoded-form SMI compares,
// AOT specialisation, fused int-counter for-loop, swap_dispatcher
// kind specialisation), jstro AOT comfortably beats node v18 here.
function sieve(n) {
  var a = new Array(n + 1);
  for (var i = 0; i <= n; i++) a[i] = true;
  a[0] = false; a[1] = false;
  var count = 0;
  for (var i = 2; i <= n; i++) {
    if (a[i]) {
      count++;
      for (var j = i * 2; j <= n; j += i) a[j] = false;
    }
  }
  return count;
}

var n = 40000000;
var start = Date.now();
var c = sieve(n);
var elapsed = (Date.now() - start) / 1000;
console.log("primes <= " + n + " = " + c);
console.log("elapsed: " + elapsed + "s");
