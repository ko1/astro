# jstro — JavaScript on ASTro

ASTro 上に構築した JavaScript (ECMAScript) インタプリタ。
ツリー・ウォーキング型ながら、V8 風の hidden class + inline cache、CRuby 風の SMI/inline-flonum 値表現、`longjmp` ベースの例外伝播、単形性の関数呼び出し IC、shape 遷移 IC など動的言語の典型的な高速化を多数取り込んでいる。

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
jstro [-q] [-v] [--show-result] [--dump] [--dump-ic] file.js
```

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

ツリーウォーキング+IC ベースとしては妥当。詳しくは [`docs/perf.md`](./docs/perf.md)。

| benchmark         | jstro (s) | node.js (s) | 倍率 |
|-------------------|-----------|-------------|------|
| fib(35)           | 0.55      | 0.08        | 7×   |
| fact ×5M          | 1.40      | 0.06        | 23×  |
| sieve(1M primes)  | 0.09      | 0.01        | 9×   |
| mandelbrot(500)   | 0.79      | 0.04        | 22×  |
| nbody 100k steps  | 0.38      | 0.02        | 21×  |
| binary_trees(14)  | 0.49      | 0.05        | 10×  |

## 既知の制限

詳しくは [`docs/todo.md`](./docs/todo.md)。代表:

- **真のジェネレータ / async microtask** — 構文は受理するが、yield/await は同期実行
- **自動 GC は未実装** — 長時間プロセスではメモリが増え続ける
- **ASTro 特化モード未駆動** — フレームワークは整備済みだが SD bake は未稼働
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
