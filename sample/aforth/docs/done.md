# done.md — aforth 実装済み機能

本書は **すでに動く** Forth サブセットを一覧する。
未実装は [todo.md](./todo.md) に分離。

## テストスイートの現状

`test/test_*.fs` 配下にスモークテストを配置。

```sh
$ make test
```

全 6 ファイル pass:
- test_arith.fs — 四則 + NEGATE / ABS
- test_stack.fs — DUP / SWAP / OVER / ROT / NIP / TUCK / 2DUP / 2DROP
- test_control.fs — IF/ELSE/THEN, BEGIN/UNTIL, ネスト DO/LOOP
- test_loop.fs — sum 0..N, +LOOP, ネスト I/J
- test_def.fs — `: word ;`, RECURSE で fib / ack
- test_var.fs — VARIABLE / CONSTANT / CREATE+ALLOT, `@ ! +!`
- test_fib.fs — fib(30) sanity check

## 言語機能

### リテラル
- 整数 (signed `int32_t` 範囲)、`0x` プレフィックス
- 文字列リテラルは `." ... "` の **印字専用** のみ (一般文字列値はなし)

### スタックマニピュレーション
| word | スタック効果 |
|------|------|
| `DUP` `?DUP` `DROP` `SWAP` `OVER` `ROT` `NIP` `TUCK` | 標準どおり |
| `2DUP` `2DROP` | 2-cell 版 |
| `DEPTH` | データスタック深さを push |

### 算術 / 論理
| group | words |
|-------|-------|
| 算術 | `+ - * / MOD NEGATE ABS 1+ 1- 2* 2/` |
| 比較 | `= <> < > <= >= 0= 0< 0>` (true は `-1`) |
| ビット | `AND OR XOR INVERT LSHIFT RSHIFT` |

### リターンスタック
`>R R> R@` (※ `2>R 2R@` 等は未実装)

### 制御フロー
- `IF body THEN` (else なし)
- `IF then-body ELSE else-body THEN`
- `BEGIN body UNTIL` (top が non-zero で抜ける)
- `BEGIN body AGAIN` (LEAVE でのみ抜ける)
- `BEGIN cond WHILE body REPEAT`
- `DO body LOOP` (limit start DO)
- `DO body +LOOP` (step は body 末尾で push)
- `I` (内側), `J` (外側) の loop index
- `LEAVE` (ctx->leave_flag を立てる; LOOP/AGAIN が観測して脱出)

### 定義
- `: name ... ;` で word 定義 (再定義は新エントリを足す Forth 標準)
- `RECURSE` で自己参照 (前方参照に相当; word_id テーブル経由で解決)

### 記憶域
- `VARIABLE name` — 1 cell の slot を `c->vars[]` に確保
- `<n> CONSTANT name` — parse-time 整数定数 (CONSTANT どうしの参照も可)
- `CREATE name <n> ALLOT` — `name` が起点 / `n` cell の領域確保
- `@ !` — VALUE 単位の load / store (アドレスは byte 単位)
- `+!` — VALUE 加算 store
- `CELLS CELL+` — cell ↔ byte 変換

### I/O
- `.` (空白付き 10 進印字), `EMIT` (1 文字), `CR`, `SPACE`, `BL` (32 push)
- `." some text"` — リテラル文字列の印字
- `TRUE` / `FALSE` (= -1 / 0)

### コメント
- `\` 行末まで
- `( ... )` 括弧内 (空白区切り `(` / `)` を要求 — `( foo)` 等は word 扱い)

## ベンチマーク

`benchmark/bm_*.fs` の 9 種、すべて持続スケール (~1 s on interp):

| bench | 内容 |
|-------|------|
| fib | recursive Fibonacci(36) — 純 RECURSE |
| ack | Ackermann(3,8) × 20 — 深い再帰 + R-stack |
| tak | Takeuchi(24,16,8) — 三重再帰 + scratch save |
| collatz | sum of Collatz lengths 1..200000 — 分岐ループ |
| factorial | fact(12) × 30M — DO/LOOP + word call |
| nested_loop | 8000×8000 二重 DO — pure dispatch + I/J |
| array_sum | 10000 cell の `@ +` × 8000 — メモリアクセス |
| gcd | euclid via `>` BEGIN..WHILE — 分岐 + word call |
| sieve | primes < 500000 × 20 — メモリ + ネスト DO + +LOOP |

## ASTro 統合

- 各 `: word ;` の body を `astro_cs_compile` の entry として登録 → 専用 SD を生成
- toplevel も entry として登録
- `--aot-compile` で 1-shot AOT (compile → build → reload → re-resolve)
- それ以降のラン (`./aforth file.fs`) は `code_store/all.so` を dlopen 済み AST に load
- `node_call` は `@noinline` + word_id テーブル経由 → 再帰 / 相互再帰の cycle 検出は不要
