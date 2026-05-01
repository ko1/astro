# jstro — JavaScript on ASTro

ASTro 上に構築した JavaScript (ECMAScript) インタプリタ。
ツリー・ウォーキング型ながら、V8 風の hidden class + inline cache、CRuby 風の SMI/inline-flonum 値表現、`longjmp` ベースの例外伝播、単形性の関数呼び出し IC、shape 遷移 IC、safepoint 駆動の mark-sweep GC など動的言語の典型的な高速化を多数取り込んでいる。

ES2023+ をかなりカバーする。クラス継承+`super`、destructuring、spread、optional chaining、Map/Set、Symbol、regex、Promise (sync)、CommonJS+ES モジュール、Proxy/Reflect、`eval`、JSON、tagged templates、private fields、static blocks、テキスト pre-scan による関数/let/const ホイスティング、for-let の per-iteration binding まで動く。

## ビルド & 実行

```sh
make             # ./jstro
./jstro examples/hello.js
make test        # test/*.js を全件回す
make bench       # benchmark/*.js を回す
ruby test/fuzz.rb # 簡易 differential fuzzer (vs node)
```

CLI:

```
jstro [options] file.js

  -q, --quiet              suppress non-essential output
  -v                       verbose (cs hit/miss stats etc.)
  --show-result            print the value of the last top-level expr
  --dump                   dump the parsed AST
  --dump-ic                print IC and GC counters at exit
  -c, --aot-compile-first  AOT-bake SDs into code_store/, then run with them active
  --aot-compile            AOT-bake SDs into code_store/ and exit (no run)
  -p, --pg-compile         run first (profile), then bake SDs (currently == -c)
  --no-compile             don't consult / write code store (pure interpreter)
```

実行モード:

- **plain** (`./jstro file.js`): 既存の `code_store/all.so` があれば
  起動時に `dlopen` し、parse 中に各ノードの dispatcher を SD に
  差し替える。なければ純インタプリタとして動く。
- **AOT bake-then-run** (`-c`): 上記の bake を 1 プロセスで実施し、
  続けて実行。`code_store/c/SD_<hash>.c` → `gcc -O3 -fPIC` →
  `dlopen` の流れ。
- **AOT bake only** (`--aot-compile`): bake のみ、即終了。次回以降
  `./jstro file.js` がキャッシュを利用する。
- **plain (no compile)** (`--no-compile`): code store を完全に無視。
- **PG** (`-p`): 実行 → bake。jstro はまだ kind-swap を実装して
  いないので bake 内容は AOT と等価。将来の profile-driven
  specialization 用に経路だけ確保。

## サポートしている JavaScript 機能

詳細は [`docs/done.md`](./docs/done.md)。要約:

- **値型** — Undefined / Null / Boolean / String / Number / Object / Symbol
  (BigInt は構文 `123n` を受理するだけで値は Number、独立した型としては未実装)
- **コア構文** — `var/let/const` (TDZ + ホイスト), function 宣言/式/arrow/async/`function*`,
  control flow (`if/while/do/for/for-of/for-in/switch/try-catch-finally/throw`),
  ラベル付き break, あらゆる演算子 (`?? ?. ??= ||= &&= ** ` を含む)
- **ES2015+ 構文糖衣** — テンプレート (タグ付きも), default/rest/spread params,
  spread call/array/object, computed keys, shorthand, **destructuring** (配列/オブジェクト/ネスト/関数引数),
  `arguments`, `new.target`
- **Class** — 継承 (`extends` + `super(...)` + `super.method()`), getter/setter,
  static methods + **static initialization blocks**, **private fields** `#foo`
- **Promise / async / await** — 同期実行のセマンティクス (microtask 非対応)
- **`function*` / `yield`** — 構文受理 (true suspend は未実装; yield 値は no-op)
- **stdlib** — `console`, `Math.{...all common methods...}`,
  `Array.prototype.{find/findIndex/findLast/every/some/flat/flatMap/fill/at/...}`,
  `String.prototype.{padStart/padEnd/replaceAll/codePointAt/at/...}`,
  `Object.{entries/fromEntries/is/freeze/seal/defineProperty/...}`,
  `Number.prototype.{toFixed/toString(radix)/toPrecision/...}`,
  `Function.prototype.{call/apply/bind}`, `JSON.{parse/stringify}`,
  **`Map` / `Set`** (SameValueZero, full API + iterators), **`WeakMap`/`WeakSet`** (alias),
  `RegExp` + リテラル `/pattern/flags`, `Symbol`, `Promise.{resolve/reject/all/then}`,
  `Proxy(target, handler)` (`get`/`set` トラップ), `Reflect.*`, `eval(str)`,
  `new Function(args, body)`, **`require()`** (CommonJS), **ES `import`/`export`**

## 性能

フェアに勝ち負け両方並べた一覧 (`jstro -c` = AOT bake 後、node v18 の
inline `Date.now()` 計測):

| benchmark         | jstro -c (s) | node v18 (s) | 結果 |
|-------------------|--------------|--------------|------|
| **cold**          | 0.015        | 0.900        | **60× ahead** (V8 が tier-up しない 1000 関数 × 1 回) |
| **try_catch**     | 0.017        | 0.716        | **42× ahead** (longjmp throw vs V8 deopt) |
| **state**         | 2.27         | 6.96         | **3.07× ahead** (Redux 風 spread reducer) |
| **sieve_big(40M)**| 1.08         | 2.69         | **2.49× ahead** (flat JsValue vs V8 element-kind) |
| **sieve(1M)**     | 0.012        | 0.014        | **1.17× ahead** |
| map_coll          | 0.082        | 0.064        | 1.28× behind |
| poly              | 0.093        | 0.026        | 3.6× behind (我々は monomorphic IC のみ) |
| regex             | 0.037        | 0.010        | 3.7× behind (V8 Irregexp に勝てない) |
| fib(35)           | 0.28         | 0.09         | 3.1× behind |
| fact ×5M          | 0.31         | 0.07         | 4.6× behind |
| binary_trees(15)  | 0.36         | 0.06         | 6.4× behind |
| mandelbrot(500)   | 0.39         | 0.04         | 10.9× behind |
| nbody 100k        | 0.26         | 0.02         | 14× behind (TurboFan 完全 engage 領域) |

勝ちが効くのは **V8 の tier-up 閾値を踏まないワークロード** と
**deopt しがちなイディオム** と **構造的なランタイム差**。負けるのは
TurboFan の数値ホットループ、専用エンジン (Irregexp)、polymorphic IC
の領域。`make compiled_jstro` で bake 済みバイナリ生成可能。

主な高速化:
- **AOT bake (SD specialization)** — 各 AST ノードを SD_<hash> に
  specialize、`dlopen` 後に dispatcher を patch。詳細は
  [`docs/runtime.md`](./docs/runtime.md)。
- **JsObject inline 4-slot** — 小オブジェクト (≤4 props) は
  slots[] を別途 malloc せず JsObject 内蔵。binary_trees で大きな
  改善 (-38%)。
- **safepoint inline** — `jstro_gc_safepoint` の fast path を
  `static inline` 化。fact / mandelbrot で -10% 程度。

## 既知の制限

詳しくは [`docs/todo.md`](./docs/todo.md)。代表:

- **真のジェネレータ / async microtask** — 構文は受理するが、yield/await は同期実行
- **WeakMap/WeakSet は strong ref で代用** — GC 自体は実装済みだが weak reference は未対応
- **profile-driven kind swap 未実装** — AOT 経路は通っているが、PG bake は
  AOT と同じ SD を出力する (kind-swap がないため)。整数/double 専用ノードを
  足せば PG が AOT を上回る
- **BigInt 未実装** — `123n` リテラル構文は受理するが値は通常の Number に
  なる (`typeof 123n === "number"`)。独立した primitive type としての BigInt は未対応
- **`String.length`** が UTF-8 byte 長 (UTF-16 単位ではない)
- **enumerable/writable/configurable** — hasOwn 相当の 1bit のみ追跡
- **`with` 文** — パースしない (非推奨)

## ディレクトリ構成

```
sample/jstro/
├── README.md            この文書
├── Makefile
├── context.h            JsValue タグ表現、CTX、ランタイム API
├── node.def             AST ノード定義 (~85 ノード)
├── node.h / node.c      フレームワーク配線
├── jstro_gen.rb         ASTroGen 拡張
├── js_parser.c          手書き再帰下降パーサ + 字句解析器
├── js_runtime.c         ヒープ / 文字列インターン / 関数呼び出し / 変換規則
├── js_stdlib.c          console / Math / Array / String / Object / Map / Set
│                        / Symbol / Promise / Proxy / Reflect / JSON /
│                        require / eval / Function / Number / __helpers__
├── js_regex.c           最小 NFA バックトラッキング regex エンジン
├── main.c               CLI ドライバ
├── docs/                追加設計ドキュメント
├── test/                .js テスト + run_all.rb + fuzz.rb
└── benchmark/           .js ベンチ + run.rb
```

`node_alloc.c` / `node_dispatch.c` / `node_eval.c` / `node_hash.c` /
`node_specialize.c` / `node_dump.c` / `node_replace.c` / `node_head.h`
は ASTroGen が `node.def` から生成する。

## 関連ドキュメント

- [`docs/runtime.md`](./docs/runtime.md) — 値表現、関数呼び出しと frame、
  hidden class、IC、例外フロー
- [`docs/perf.md`](./docs/perf.md) — 性能向上の経緯。成功した最適化、
  試したが効かなかった最適化
- [`docs/done.md`](./docs/done.md) — 実装した言語機能・stdlib・最適化リスト
- [`docs/todo.md`](./docs/todo.md) — 未実装機能・将来の最適化アイデア
