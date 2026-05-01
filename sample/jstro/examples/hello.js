console.log("hello, jstro!");
console.log(1 + 2 * 3);
console.log("typeof 42:", typeof 42);

function fib(n) {
  if (n < 2) return n;
  return fib(n - 1) + fib(n - 2);
}
console.log("fib(10) =", fib(10));
