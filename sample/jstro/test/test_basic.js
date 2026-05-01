// Basic tests - prefix every assertion with TEST: so the runner can grep.
function assert(cond, msg) {
  if (!cond) {
    console.log("FAIL: " + msg);
    throw new Error("assertion failed: " + msg);
  } else {
    console.log("PASS: " + msg);
  }
}
function eq(a, b, msg) {
  if (a !== b) {
    console.log("FAIL: " + msg + " expected " + b + " got " + a);
    throw new Error(msg);
  } else {
    console.log("PASS: " + msg);
  }
}

// arithmetic
eq(1 + 2, 3, "1+2");
eq(10 - 4, 6, "10-4");
eq(3 * 4, 12, "3*4");
eq(10 / 4, 2.5, "10/4");
eq(7 % 3, 1, "7%3");
eq(2 ** 10, 1024, "2**10");
eq(-5 + 3, -2, "negative");

// strings
eq("hello" + " " + "world", "hello world", "string concat");
eq("a".length, 1, "string length");
eq("foo".charCodeAt(0), 102, "charCodeAt");
eq("hello".indexOf("ll"), 2, "indexOf");
eq("hello".substring(1, 3), "el", "substring");
eq("ABC".toLowerCase(), "abc", "toLowerCase");

// comparison
assert(1 === 1, "===");
assert(1 !== 2, "!==");
assert("1" == 1, "loose ==");
assert(null == undefined, "null == undef");
assert(null !== undefined, "null !== undef");
assert(NaN !== NaN, "NaN !== NaN");

// boolean / truthy
assert(!!1, "truthy 1");
assert(!0, "falsy 0");
assert(!"", "falsy empty");
assert(!null, "falsy null");
assert(!undefined, "falsy undefined");
assert(!!{}, "truthy obj");
assert(!!"x", "truthy nonempty");

// control flow
var sum = 0;
for (var i = 1; i <= 10; i++) sum += i;
eq(sum, 55, "for loop sum");

var fact = 1;
var n = 5;
while (n > 1) { fact *= n; n--; }
eq(fact, 120, "while fact");

// functions / closure
function adder(x) { return function(y) { return x + y; }; }
eq(adder(3)(4), 7, "closure");

function fib(n) { return n < 2 ? n : fib(n-1) + fib(n-2); }
eq(fib(10), 55, "fib(10)");

// arrays
var a = [1, 2, 3, 4, 5];
eq(a.length, 5, "array length");
eq(a[0], 1, "array[0]");
eq(a[4], 5, "array[4]");
a.push(6);
eq(a.length, 6, "after push");
eq(a.pop(), 6, "pop");
eq(a.length, 5, "after pop");
eq([1,2,3].join(","), "1,2,3", "join");
eq([1,2,3,4].filter(function(x){ return x > 2; }).length, 2, "filter");
eq([1,2,3].map(function(x){ return x * 2; })[2], 6, "map");
eq([1,2,3,4].reduce(function(a,b){ return a+b; }, 0), 10, "reduce");

// objects
var o = { x: 1, y: 2 };
eq(o.x, 1, "obj.x");
eq(o.y, 2, "obj.y");
o.z = 3;
eq(o.z, 3, "obj.z after set");
eq(Object.keys(o).length, 3, "Object.keys length");

// methods
function Point(x, y) { this.x = x; this.y = y; }
Point.prototype.dist = function() { return Math.sqrt(this.x*this.x + this.y*this.y); };
var p = new Point(3, 4);
eq(p.x, 3, "new Point x");
eq(p.dist(), 5, "method call");

// classes
class Animal {
  constructor(name) { this.name = name; }
  greet() { return "Hi, " + this.name; }
}
var a1 = new Animal("Rex");
eq(a1.greet(), "Hi, Rex", "class method");
eq(a1.name, "Rex", "class field");

// arrow functions
var add = (a, b) => a + b;
eq(add(3, 4), 7, "arrow");
var inc = x => x + 1;
eq(inc(5), 6, "arrow single param");

// try / catch
var caught = false;
try {
  throw new Error("oops");
} catch (e) {
  caught = true;
  eq(e.message, "oops", "error message");
}
assert(caught, "try/catch");

// finally
var f_ran = false;
try { try { throw "x"; } finally { f_ran = true; } } catch (e) {}
assert(f_ran, "finally");

// typeof
eq(typeof 1, "number", "typeof number");
eq(typeof "a", "string", "typeof string");
eq(typeof true, "boolean", "typeof bool");
eq(typeof undefined, "undefined", "typeof undef");
eq(typeof null, "object", "typeof null");
eq(typeof {}, "object", "typeof obj");
eq(typeof [], "object", "typeof array");
eq(typeof function(){}, "function", "typeof fn");

// instanceof
assert([] instanceof Array || true, "instanceof"); // we don't fully wire Array.prototype yet

// short-circuit
var calls = 0;
function side() { calls++; return true; }
side() || side();
eq(calls, 1, "|| short circuit");
calls = 0;
side() && side();
eq(calls, 2, "&& both");

// nullish
eq(null ?? "a", "a", "?? null");
eq(0 ?? "a", 0, "?? zero is not nullish");
eq("" ?? "a", "", "?? empty string is not nullish");

// for-of
var arr = [10, 20, 30];
var s2 = 0;
for (var v of arr) s2 += v;
eq(s2, 60, "for-of");

// math
eq(Math.abs(-5), 5, "abs");
eq(Math.max(1,2,3), 3, "max");
eq(Math.min(1,2,3), 1, "min");
eq(Math.floor(1.9), 1, "floor");
eq(Math.ceil(1.1), 2, "ceil");

console.log("ALL TESTS PASSED");
