// Classic recursive fib
function fib(n) {
  if (n < 2) return n;
  return fib(n - 1) + fib(n - 2);
}

var start = Date.now();
var n = 35;
var r = fib(n);
var elapsed = (Date.now() - start) / 1000;
console.log("fib(" + n + ") = " + r);
console.log("elapsed: " + elapsed + "s");
