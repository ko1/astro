# done.md — 実装済み機能リスト

ECMAScript 仕様への対応状況と、性能向上のために行った変更を記録する。

## 言語機能 (ES2023+ サブセット)

### §6 Types
- [x] Undefined / Null / Boolean / String / Number / Object / Symbol
- [x] IEEE 754 double のフルレンジ (NaN/±0/±Inf 含む)
- [x] SMI (符号付き 63 bit 整数) と inline flonum のタグ表現 ※ Number 型の内部最適化
- [x] Symbol — `Symbol(desc)` がユニークな heap 値を返し、`typeof === "symbol"`、
      Symbol.iterator / .asyncIterator / .toPrimitive を well-known string key として公開
- [ ] **BigInt は未実装**。`123n` リテラル構文は lexer が `n` を consume するが、値は通常の
      Number として扱う。`typeof 123n` は `"number"` を返す (spec は `"bigint"`)。
      混合算術 (`1 + 1n`) は TypeError にならず Number として動く

### §7 Abstract operations
- [x] ToBoolean / ToNumber / ToString / ToPrimitive / ToObject
- [x] ToInt32 / ToUint32 (ビット演算用)
- [x] SameValueNonNumber, IsLooselyEqual, IsLessThan (tri-state で NaN/undefined を区別)
- [x] strict equality on NaN (NaN !== NaN)
- [x] SameValue (Object.is)
- [x] Loose equality with Symbol/BigInt (pointer comparison)

### §10–§13 Statements / Expressions
- [x] `var` (関数スコープ・関数先頭にホイスト)
- [x] `let` / `const` (ブロックスコープ + TDZ; ブロック先頭ホイスト)
- [x] block `{ ... }`
- [x] `if` / `if-else`
- [x] `while` / `do-while`
- [x] C-style `for (init; cond; step)`
- [x] `for-of` (Array / String / Symbol.iterator プロトコル / `next()` ダックタイピング)
- [x] `for-in` (own keys + プロトタイプチェイン辿り、重複除去)
- [x] **for-let の per-iteration binding** (各反復で新しい box を割り当て)
- [x] `break` / `continue` / `return`
- [x] **Labeled statement** + `break <label>` (named loop break)
- [x] `throw` / `try-catch-finally` (longjmp 経由)
- [x] `switch (e) { case ...: ...; default: ... }`
- [x] 算術: `+ - * / % **` (整数フラット, double フラット, 混在の各 fast path)
- [x] 比較: `< <= > >= == != === !==`
- [x] 論理: `&& || !`
- [x] Nullish: `??` / `?.` / `?.[]` / `?.()`
- [x] Bitwise: `& | ^ ~ << >> >>>`
- [x] Assignment compound: `+= -= *= /= %= &= |= ^= <<= >>= >>>= **=`
- [x] Logical compound: `&&= ||= ??=`
- [x] Pre/post `++` / `--` (ローカル / boxed / upval / member / 添字 / global)
- [x] `typeof` (識別子未宣言でも throw しない特例含む)
- [x] `void`, `delete obj.name` (実際にスロットを HOLE 化、enumerationからも除外)
- [x] `instanceof`, `in`
- [x] template literal、タグ付きテンプレート (`tag\`...${x}...\``)

### §14 Functions
- [x] 関数宣言 (`function name() {}`) — テキスト pre-scan によるホイスト
- [x] 関数式 / アロー関数 (語彙的 `this`)
- [x] **デフォルト引数** (`function f(x = 10, y = x*2) {}`)
- [x] **rest パラメータ** (`function f(a, ...rest) {}`)
- [x] **spread 呼び出し** (`f(...arr)`, `new C(...args)`, `obj.m(...args)`)
- [x] **配列スプレッド** (`[...a, ...b]`)
- [x] **オブジェクトスプレッド** (`{...o, x: 1}`)
- [x] **計算プロパティキー** (`{[k]: v}`)
- [x] **ショートハンド** (`{x}` ≡ `{x: x}`、メソッド `{ foo() {} }`)
- [x] **`arguments` オブジェクト** (関数内の暗黙変数)
- [x] **`new.target`** (コンストラクタ起動時の `new` 参照)
- [x] **destructuring** — 配列/オブジェクトパターン、デフォルト値、rest、ネスト、関数引数
- [x] `new` (constructor 起動 + プロトタイプチェイン接続)
- [x] `Function.prototype.call` / `.apply` / `.bind`

### §15 Class
- [x] `class Name { constructor() {} method() {} static foo() {} }`
- [x] static メソッド + **static フィールド初期化ブロック** (`static { ... }`)
- [x] プロトタイプメソッド
- [x] **`class Sub extends Base { ... }`** — プロトタイプチェイン接続
- [x] **`super(...)`** (Sub の constructor から Base を呼ぶ)
- [x] **`super.method(...)`** (Base のメソッドを this 付きで呼ぶ)
- [x] **getter / setter** (`get x() {}` / `set x(v) {}`) — class & object literal 双方
- [x] **private フィールド** `#foo` (lexer サポート; 真のプライバシは `#` プリフィックスの規約)

### §22 / §24 stdlib
- [x] `Array.prototype` の `push, pop, shift, unshift, slice, concat, join,
      indexOf, lastIndexOf, forEach, map, filter, reduce, sort (merge),
      reverse, includes, find, findIndex, findLast, findLastIndex, every,
      some, flat, flatMap, fill, at, copyWithin`
- [x] `Array.isArray` 相当 (グローバル `_Array_isArray`)
- [x] `String.prototype.{charAt, charCodeAt, indexOf, substring, slice,
      split, toUpperCase, toLowerCase, replace, replaceAll, includes,
      startsWith, endsWith, trim, trimStart, trimEnd, repeat, padStart,
      padEnd, at, codePointAt, normalize, concat}`
- [x] `Object.{keys, values, assign, create, freeze, isFrozen, seal,
      isSealed, preventExtensions, getPrototypeOf, setPrototypeOf, entries,
      fromEntries, is, getOwnPropertyNames, defineProperty, defineProperties}`
- [x] `Object.prototype.hasOwnProperty`
- [x] `Number.{isFinite, isNaN, isInteger}` + 定数
- [x] **Number.prototype.{toFixed, toString(radix), toPrecision, valueOf}**
- [x] `parseInt` / `parseFloat` / `isNaN` / `isFinite`
- [x] `Function.prototype.{call, apply, bind}`
- [x] `Math.{abs, sqrt, floor, ceil, round, trunc, sin, cos, tan, atan,
      atan2, log, exp, pow, min, max, random, cbrt, hypot, sign, log2,
      log10, log1p, expm1, fround, clz32, imul, asin, acos, sinh, cosh, tanh}`
- [x] `Date.now`, `performance.now`
- [x] `console.{log, error, warn, info}`
- [x] `Error` / `TypeError` / `RangeError` / `ReferenceError` / `SyntaxError`
- [x] **`Map`** (SameValueZero、`size`、CRUD、`forEach`、`keys/values/entries`、for-of)
- [x] **`Set`** (同上、`add` 含む)
- [x] **`WeakMap` / `WeakSet`** (Map/Set へのエイリアス; 真の弱参照は GC 未実装ゆえ)
- [x] **`TypedArray` ファミリ** (Uint8Array etc.) — Array にエイリアス
- [x] **`ArrayBuffer`** (同上)
- [x] **`Symbol`** (`Symbol(desc)` ユニーク値、well-known)

### §22 Regex
- [x] **regex リテラル** `/pattern/flags` — `/` の文脈分離 (除算 vs regex)
- [x] **NFA バックトラッキング・マッチャ** — リテラル文字、`. * + ? {m,n}`、
      `^ $`、`[abc]` `[^abc]` `[a-z]`、`\d\D\s\S\w\W \b\B`、`(group)` `(?:...)`、
      `|` 交替、フラグ `i g m s` (u/y/d は受理だが無視)
- [x] **`RegExp(pattern, flags)`** コンストラクタ
- [x] **`re.test(s)`** / **`re.exec(s)`**

### §27 Promise / async / Generator
- [x] **`async function`** / **`await expr`** — 同期実行 (Promise を `__value`
      slot 経由で取り出し)。マイクロタスクスケジューリングは未対応
- [x] **`Promise`** コンストラクタ、`Promise.resolve` / `Promise.reject` / `Promise.all`、`.then(cb)`
- [x] `function*` / `yield` 構文 — 受理だが yield は no-op (suspend 未実装)

### §16 Modules
- [x] **CommonJS `require()`** — パス解決、モジュールキャッシュ、`module.exports`
- [x] **ES `import`** (default / named / namespace / 副作用のみ)
- [x] **ES `export`** (`export default`, `export const`, `export function/class`,
      `export { name as alias }`)

### §28 Reflection / Proxy
- [x] **`Proxy(target, handler)`** — `get` / `set` トラップ
- [x] **`Reflect.{get, set, has, deleteProperty, ownKeys, getPrototypeOf,
      setPrototypeOf, apply, construct}`**
- [x] **`eval(str)`** — 同期コンパイル+実行 (caller スコープ非対応; 新規 top-level)
- [x] **`Function(args, body)`** コンストラクタ — 同様

### §24 JSON
- [x] **`JSON.parse`** — フル仕様 (escape含む)
- [x] **`JSON.stringify`** — 主要型 + `toJSON` メソッド呼び出し

### コアランタイム
- [x] V8 風 hidden class (JsShape) と shape transition のキャッシュ
- [x] property access の inline cache (shape + slot)
- [x] property write の transition IC (constructor pattern 高速化)
- [x] global access の inline cache
- [x] call site の monomorphic IC (cached_fn / cached_body / cached_nlocals)
- [x] longjmp ベースの例外伝播 (try-frame setjmp)
- [x] 文字列インターン (オープンアドレッシングハッシュ)
- [x] アロー関数の語彙的 `this` (生成時にキャプチャ)
- [x] 関数の upvalue ボックス化 (`JsBox`) によるクロージャ実装
- [x] **オブジェクトの `own_props`** — JsFunction / JsCFunction に静的プロパティ用
      補助オブジェクトを追加
- [x] **アクセサ (`get/set`)** — `JS_TACCESSOR` 型のスロット値で実装、IC fast path
      は accessor を検出して slow path にフォールバック
- [x] **prototype chain の `js_object_get_with_receiver`** — receiver を保持して
      proto chain を辿る (getter の `this` がインスタンスを指す)
- [x] **freeze / seal / preventExtensions の実効化** — JsObject.gc.flags に bit
- [x] **delete** — スロットを HOLE 化、Object.keys / for-in でフィルタ
- [x] **`#priv` トークン** — `#` を識別子の一部として字句解析
- [x] `typeof` で symbol / bigint を返す
- [x] **`__chainProto__` / `__defAccessor__` / `__makeRegex__` / `__awaitSync__`** ランタイム補助関数
- [x] **AOT 特化 (SD bake) モード** — `astro_code_store` 連携。`-c` で
      `code_store/c/SD_<hash>.c` を吐いて `make` で `all.so` にビルド、
      その場で `dlopen` + `dlsym` して各ノードの dispatcher を SD に
      差し替える。`--aot-compile` は bake のみ、`--no-compile` は
      code store を完全に無視する純インタプリタ。`-p` (PG) は run-
      then-bake — jstro は kind-swap を実装していないので現状 AOT と
      等価だが、将来の profile-driven specialization 用に経路を確保。
      関数本体は `code_repo_add` で個別に登録し、parser side-array
      ノードも個別に bake することで dlsym カバレッジを最大化
      (luastro と同様)。inner SD は `static inline` だが
      `jstro_export_sd_wrappers` が `SD_<h>_INL` にリネームして
      `__attribute__((weak)) RESULT SD_<h>(...) { return SD_<h>_INL(...); }`
      の薄い extern wrapper を append し、dlsym が全ノードを解決
      できるようにしている。深い AOT-inline 鎖が 8 MB スタックを
      食い潰すので、本体は `pthread_create` で 4 GiB virtual stack の
      worker thread に切り出して実行 (luastro と同じ手法)。
      ベンチ結果 (geo-mean 2.0×): fib 2.4× / fact 3.1× / sieve 2.9× /
      mandelbrot 1.9× / nbody 1.6× / binary_trees 1.2×。
- [x] **自動 GC (mark-sweep)** — `c->all_objects` リンクトリストを基盤にしたマーク&スイープ。
      ルート: globals / protos / this / cur_args / last_thrown / break-val / intern_buckets / modules /
      `frame_stack` (alloca フレーム連鎖)。トリガはセーフポイント方式 (node_seq / node_for /
      node_while などの文境界で `jstro_gc_safepoint`)。閾値は alive オブジェクト数の 2× で
      動的調整 (floor 4096 / ceiling 1M)。color polarity flip でスイープせずマーク反転だけで再利用。
      引数評価中の use-after-free 対策として、各 call dispatcher と array/object literal が
      `js_frame_link` を介して半構築 argv / 受け手を pin する。

### 開発体験
- [x] `--dump-ic` オプション — `js_shape_find_slot` 呼び出し回数を表示
- [x] **diff fuzzer** (`test/fuzz.rb`) — ランダム JS 式で node と差分検出

## 未実装 (詳細は [`todo.md`](./todo.md))

- 多形性 IC (4-way) — call IC は monomorphic、shape IC も 1 段
- 真のジェネレータ (suspendable frame; ucontext 等)
- async/await の microtask スケジューリング
- 多形性 IC (4-way)
- Source map / Error.stack の行番号
