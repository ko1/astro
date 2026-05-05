# jstro 言語仕様

`jstro` は **JavaScript (ECMAScript 2023+)** のサブセットインタプリタ。
Web ブラウザや Node.js で普段書く JavaScript とほぼ同じものが動く。
完全な ECMAScript 仕様は [tc39.es/ecma262](https://tc39.es/ecma262/) を
参照。本書は jstro で動く範囲を端的に示す。

## 値の種類

JavaScript の標準どおり 7 種:

| 型 | 例 | 備考 |
|---|---|---|
| `undefined` | `undefined` | 値が無いことを示す既定値 |
| `null` | `null` | 意図的な「何もない」 |
| `boolean` | `true` `false` | |
| `number` | `42` `3.14` `NaN` `Infinity` | IEEE-754 double。jstro では SMI (small integer) と inline flonum で内部最適化 |
| `string` | `"hi"` `'world'` `` `tpl ${x}` `` | immutable |
| `symbol` | `Symbol("k")` | ユニーク識別子 |
| `object` | `{}` `[]` `function(){}` | 関数・配列もすべて object |

(`bigint` は構文 `123n` を受理するが Number として扱う — 独立した型としては未対応。)

**真偽判定**: `false` `0` `0n` `""` `null` `undefined` `NaN` が偽 (falsy)。
それ以外はすべて真 (truthy)。

## リテラル

```js
42  -3.14  0xff  1e10
"hello"  'world'
`template ${x + 1} string`           // テンプレート文字列
true  false  null  undefined
[1, 2, 3]                             // 配列
{name: "alice", age: 30}              // オブジェクト
{[expr]: val}                         // 計算キー
/regex/i                              // 正規表現
function () { ... }
(x) => x + 1                          // arrow function
```

## 変数宣言

```js
var x = 1;       // 関数スコープ、ホイスト可、再宣言可
let y = 2;       // ブロックスコープ、TDZ あり、再宣言不可
const z = 3;     // ブロックスコープ、再代入不可
```

`let` / `const` は **ブロックスコープ** + **TDZ (一時的死角)** を持つ。
宣言前のアクセスは ReferenceError:

```js
console.log(a);   // ReferenceError (TDZ)
let a = 1;
```

## 演算子

| カテゴリ | 演算子 |
|---|---|
| 算術 | `+ - * / % **` |
| 比較 | `< > <= >= == != === !==` (`===` は型を含めて厳密比較) |
| 論理 | `&& \|\| ! ?? ` (`??` は null/undefined のみ右辺を採る) |
| 代入 | `= += -= *= /= %= **= &&= \|\|= ??= &= \|= ^= <<= >>= >>>=` |
| ビット | `& \| ^ ~ << >> >>>` (>>> は符号なし右シフト) |
| その他 | `typeof instanceof in delete void` |
| インクリ | `++ --` (前置・後置) |
| 三項 | `? :` |
| 連鎖 | `?.` (optional chaining: 左辺が null/undefined なら undefined) |
| 文字列連結 | `+` |

## 制御構文

```js
if (cond) stmt
if (cond) stmt else stmt

while (cond) stmt
do stmt while (cond);

for (let i = 0; i < n; i++) stmt
for (const k in obj) stmt              // キーを反復
for (const v of iterable) stmt         // 値を反復

switch (e) {
  case 1: ... break;
  case 2: case 3: ...; break;
  default: ...;
}

try {
  ...
} catch (e) {
  ...
} finally {
  ...
}

throw new Error("oops");

label: for (...) {
  break label;       // ラベル付き break
  continue label;
}
```

## 関数

### 関数宣言・関数式

```js
function add(a, b) { return a + b; }       // 関数宣言: ホイスト
const sub = function(a, b) { return a - b; };  // 関数式: ホイストなし
const mul = (a, b) => a * b;                // arrow function
const sqr = x => x * x;                     // 引数 1 個は括弧省略可
const noop = () => {};
```

arrow function は **`this` を字句的に捕獲** する (関数宣言/式と異なる)。

### 引数の機能

```js
function f(a, b = 10, ...rest) { ... }     // デフォルト引数 + rest

f(1, 2, 3, 4, 5);                            // a=1, b=2, rest=[3,4,5]

const arr = [1, 2, 3];
f(...arr);                                    // spread 呼出
```

### 多値返却 (destructuring 利用)

```js
function pair() { return [1, 2]; }
const [a, b] = pair();      // 配列分割代入
const {x, y} = {x: 1, y: 2}; // オブジェクト分割代入
```

## クラス

```js
class Animal {
  constructor(name) { this.name = name; }
  greet() { return `hi ${this.name}`; }
  static create(name) { return new Animal(name); }
  get displayName() { return this.name.toUpperCase(); }
  set displayName(v) { this.name = v.toLowerCase(); }
}

class Dog extends Animal {
  constructor(name, breed) {
    super(name);                  // 親コンストラクタ
    this.breed = breed;
  }
  bark() { return `${super.greet()}, woof!`; }
}

const d = new Dog("rex", "lab");
d.bark();       // "hi rex, woof!"
```

サポート機能:

- 継承 (`extends`) と `super(...)` / `super.method()`
- `static` メソッド
- **static initialization block** (`static { ... }`)
- **private field** (`#foo`)
- getter / setter (`get name()` / `set name(v)`)

## オブジェクト

```js
const obj = {
  name: "alice",
  age: 30,
  greet() { return `hi ${this.name}`; },        // メソッド省略形
  ["computed_" + key]: 42,                       // 計算キー
};

obj.name;          // "alice"
obj["age"];        // 30
obj.unknown;       // undefined (例外ではない)

obj.email = "x@y";  // 動的に追加可
delete obj.age;     // 削除

const {name, age} = obj;       // 分割代入
const cloned = {...obj};        // spread コピー
const merged = {...a, ...b};    // マージ
```

### `for...in` / `for...of`

```js
for (const k in obj) { console.log(k); }       // 列挙可能なキー
for (const v of [1,2,3]) { console.log(v); }   // iterable の値
```

### Optional chaining / Nullish coalescing

```js
obj?.user?.name             // どこかで null/undefined なら undefined
obj?.method?.()              // 関数呼出も
arr?.[0]
const v = x ?? "default"     // x が null/undefined のときだけ default
```

## 配列

```js
const a = [1, 2, 3];
a.length;                    // 3
a[0];                        // 1
a.push(4);                   // a = [1, 2, 3, 4]
a.pop();                     // 4 を取り除いて返す

[1, 2, 3, 4, 5]
  .map(x => x * 2)           // [2, 4, 6, 8, 10]
  .filter(x => x > 4)        // [6, 8, 10]
  .reduce((s, x) => s + x);  // 24

a.find(x => x > 2);          // 3
a.includes(2);               // true
a.flat();                    // ネスト配列を 1 段平らに
a.at(-1);                    // 末尾要素
[...a, 99];                  // spread で連結
```

## 文字列

```js
"hello".length              // 5
"hello".toUpperCase()       // "HELLO"
"hello".slice(1, 3)         // "el"
"a,b,c".split(",")           // ["a", "b", "c"]
"abc".repeat(3)             // "abcabcabc"
"  x  ".trim()              // "x"
`${a} + ${b} = ${a+b}`      // テンプレート文字列
```

## Map / Set

```js
const m = new Map();
m.set("a", 1).set("b", 2);
m.get("a");                  // 1
m.has("a");                  // true
m.size;                       // 2
for (const [k, v] of m) ...

const s = new Set([1, 2, 2, 3]);
s.size;                       // 3
s.has(2);                     // true
s.add(4);
[...s];                       // [1, 2, 3, 4]
```

`WeakMap` / `WeakSet` も使えるが GC 連携は限定的。

## Promise (sync 実装)

```js
const p = new Promise((resolve, reject) => {
  resolve(42);
});

p.then(v => v * 2).then(console.log);    // 84

Promise.all([p1, p2]).then(([a, b]) => ...);
```

**注意**: jstro の Promise は **同期実行** で、`then` のコールバックは
microtask キューを介さず即座に呼ばれる。`async`/`await` も同様の同期セマンティクス。

## 例外

```js
try {
  throw new Error("bad");
} catch (e) {
  console.log(e.message);     // "bad"
} finally {
  cleanup();
}
```

組み込みエラー: `Error` `TypeError` `RangeError` `SyntaxError` `ReferenceError`。

## モジュール

```js
// CommonJS
const fs = require("./mymod");
module.exports = { foo: 1 };

// ES Modules
import { foo, bar } from "./mymod.js";
import defaultExport from "./mymod.js";
import * as ns from "./mymod.js";
export { x, y };
export default value;
```

両形式を受理。同じプログラム内で混在も可。

## 標準ライブラリ (主なもの)

`console` — `log` `error` `warn` `dir` `time/timeEnd`

`Math` — `Math.PI Math.E`、`abs floor ceil round trunc sign min max
sqrt pow log log2 log10 exp sin cos tan atan2 random hypot`

`Array.prototype` — `map filter reduce reduceRight find findIndex findLast
every some flat flatMap fill at slice splice concat join indexOf lastIndexOf
includes sort reverse forEach`

`String.prototype` — `length charAt charCodeAt codePointAt at slice substring
substr split replace replaceAll match matchAll search includes startsWith
endsWith padStart padEnd repeat trim trimStart trimEnd toLowerCase toUpperCase`

`Object` — `keys values entries fromEntries assign freeze isFrozen seal
defineProperty getOwnPropertyDescriptor getPrototypeOf setPrototypeOf is`

`Number.prototype` — `toFixed toString(radix) toPrecision toExponential`

`Function.prototype` — `call apply bind`

`JSON.parse` / `JSON.stringify`

`Symbol(description)` / `Symbol.iterator` / `Symbol.asyncIterator`

`Proxy(target, handler)` — `get` / `set` トラップ
`Reflect.get` / `Reflect.set` / `Reflect.has` / `Reflect.ownKeys`

`eval(str)` / `new Function(args, body)`

## 例

```js
// クラス + 継承
class Shape {
  constructor(name) { this.name = name; }
  describe() { return `a ${this.name}`; }
}

class Circle extends Shape {
  constructor(radius) {
    super("circle");
    this.radius = radius;
  }
  area() { return Math.PI * this.radius ** 2; }
  describe() { return `${super.describe()} of radius ${this.radius}`; }
}

const c = new Circle(5);
console.log(c.describe(), c.area());

// async-like (sync) Promise
async function fetchAll(urls) {
  const results = [];
  for (const u of urls) {
    results.push(await fetch(u));
  }
  return results;
}

// Map ベースのカウント
function countWords(text) {
  const m = new Map();
  for (const w of text.split(/\s+/)) {
    m.set(w, (m.get(w) ?? 0) + 1);
  }
  return m;
}
```

## 持たない / 制限

- 真の `function*` (yield 値は no-op、true suspend は未実装)
- microtask キュー (Promise は同期実行)
- `BigInt` (構文受理のみ)
- `await` の真の非同期セマンティクス (同期呼出として動く)
- DOM / Node.js のホスト API (基本のみ; `fs` / `http` 等は無し)
- `Intl` (国際化 API)
- ECMAScript の Realm 機能

詳細: [`done.md`](done.md) / [`todo.md`](todo.md)。
