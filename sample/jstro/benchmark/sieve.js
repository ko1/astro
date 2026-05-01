// Sieve of Eratosthenes — array-heavy benchmark.
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

var n = 1000000;
var start = Date.now();
var c = sieve(n);
var elapsed = (Date.now() - start) / 1000;
console.log("primes <= " + n + " = " + c);
console.log("elapsed: " + elapsed + "s");
