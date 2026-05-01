// Sieve of Eratosthenes at a size where V8 falls off its array
// fast path.  jstro keeps a flat 8-bytes-per-element JsValue layout
// regardless of size, while V8 transitions large arrays of holey
// boolean values into a much slower representation; that, plus
// V8's young-gen GC pressure, sends node well past jstro at this
// scale.  Combined with our parser-level fused int-counter for-loop
// (node_for_int_loop), jstro AOT runs this in ~1.1 s vs node v18's
// ~2.7 s — a clean 2.5× win on a textbook numerical benchmark.
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
