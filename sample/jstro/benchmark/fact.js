function fact(n) {
  var r = 1;
  for (var i = 2; i <= n; i++) r *= i;
  return r;
}

// run many times — fact is small, so we need a hot loop to reach 1s.
var start = Date.now();
var sum = 0;
var iters = 5000000;
for (var k = 0; k < iters; k++) sum += fact(20);
var elapsed = (Date.now() - start) / 1000;
console.log("sum (last) = " + sum);
console.log("iters = " + iters + ", elapsed = " + elapsed + "s");
