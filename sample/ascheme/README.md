# ascheme — R5RS Scheme on ASTro

ascheme は ASTro フレームワークの上に構築した **R5RS Scheme インタプリタ**。
ツリーウォーキングのインタプリタとして実行することも、ASTro の部分評価
+ C コンパイラ経由でネイティブコードに焼き直して走らせることもできる。
プロファイル誘導コンパイル (PGO) も `--pg-compile` で利用可能。

実装の詳細は [`docs/runtime.md`](./docs/runtime.md) を参照。
ASTro フレームワーク自体については [`../../docs/idea.md`](../../docs/idea.md)。

## ハイライト

- **完全な R5RS 数値タワー** — fixnum / bignum (GMP) / rational (GMP) /
  flonum (Ruby 流の inline 符号化付き) / complex。`(+ 1/2 1/3)` は `5/6`、
  `(expt 2 100)` は exact bignum、`(make-rectangular 3 4)` は `3+4i`。
- **適切な末尾呼出** — CTX のトランポリン (`tail_call_pending` +
  `next_body` + `next_env`) で C スタックを伸ばさず無限末尾再帰可能。
- **継続** — `call/cc` は脱出継続 (one-shot, downward) を `setjmp` /
  `longjmp` で実装。
- **多値** — `(values …)` と `(call-with-values producer consumer)`。
- **約束** — `(delay …)` / `(force …)`。memoize 付き。
- **ポート** — `open-input-file` / `with-output-to-file` / `read-char` /
  `peek-char` / `current-input/output-port` 他。`fd` を `dup`/`dup2`
  で save/restore してスコープ内だけリダイレクト。
- **特化ノード** — `(+ a b)` `(< a b)` `(car x)` `(vector-ref v i)`
  `(null? x)` 等を専用ノードに降ろし、`__builtin_*_overflow` + range
  check のみのホットパスに。R5RS の `(set! + my+)` 再定義検出付き。
- **AOT コンパイル** — 全エントリを ASTro の特化器で C 化し、`gcc -O3`
  でビルドして `dlopen`。`-c` フラグ。
- **PGO** — `--pg-compile` で 1 起動内に「インタプリタ実行 + ホットエントリ
  のみ AOT」を行う abruby 流のフロー。次回起動時は `code_store/profile.txt`
  を自動的に拾って cold エントリを skip。
- **GC** — Boehm-Demers-Weiser conservative GC (`libgc`)。GMP の内部割当も
  `mp_set_memory_functions` で `GC_malloc` 経由。
- **Boehm-スタイル の R5RS 互換** — chibi-scheme の `tests/r5rs-tests.scm`
  を機械変換した 179 テストを 100% パス。

## 試す

```sh
make            # ascheme バイナリ
make test       # 16 件の自前テスト + chibi r5rs-tests 179 件
make bench      # bench/small (interp / aot-first / aot-cached / pg-compile / pg-cached / chibi / guile)
make bench-big  # bench/big (重め — 数秒〜数十秒)
make clean
```

REPL:

```sh
$ ./ascheme
ascheme> (define (fact n) (if (= n 0) 1 (* n (fact (- n 1)))))
ascheme> (fact 50)
30414093201713378043612608166064768844377641568960512000000000000
ascheme> (force (delay (begin (display "computed!\n") 42)))
computed!
42
```

ファイル / 式 / stdin / AOT / PGO 実行:

```sh
./ascheme test/03_lambda.scm                    # interpret
./ascheme -e '(+ 1 2 3)'                        # one-liner
echo '(display (* 2 3))' | ./ascheme -          # stdin
./ascheme -c test/06_higher_order.scm           # AOT compile + run
./ascheme --clear-cs --pg-compile bench/big/fib35.scm  # PGO: interp + hot AOT
./ascheme -c bench/big/fib35.scm                # subsequent run uses code_store/
```

CLI:

```
-q, --quiet       静かに
-c, --compile     AOT コンパイル経由で実行 (code_store/ を使う)
-v, --verbose     AOT 進捗を stderr へ
    --clear-cs    code_store/ を空にしてから開始
    --pg-compile  abruby 流 PGO: インタプリタ実行 + 末尾でホットエントリ AOT
-e <expr>         式を評価して結果表示
-                stdin から読む
```

## ベンチマーク

`make bench` は ascheme の 5 モード (interp / aot-first / aot-cached /
pg-compile / pg-cached) と他処理系 2 つ (chibi-scheme 0.12, guile-3.0)
を一括計測。`bench/compare.sh` が初回実行で chibi-scheme をローカルに
fetch + build (`./.chibi/`) する。

例 (Linux x86_64, gcc -O2; small ベンチは interp で ~1 秒帯に揃えてある):

```
=== bench/small/ ===
benchmark        interp  aot-first aot-cached pg-compile  pg-cached      chibi      guile
ack               0.76s      0.64s      0.53s      0.89s      0.59s      0.74s      2.49s
fib               1.51s      1.10s      1.27s      1.73s      0.97s      1.40s      4.70s
list              1.38s      1.32s      1.15s      1.35s      1.16s      0.87s      1.30s
loop              1.07s      1.00s      0.92s      1.24s      0.92s      0.91s      3.31s
sieve             1.36s      1.13s      0.97s      1.48s      0.96s      1.48s      5.14s
sum               1.31s      1.43s      1.29s      1.35s      1.30s      1.27s      5.39s
tak               1.47s      1.11s      1.01s      1.57s      1.03s      1.59s      6.39s
```

列の意味:

| 列 | 1 起動分の内訳 |
|---|---|
| **interp** | プレーンインタプリタ |
| **aot-first** | `--clear-cs -c`: 全エントリを SD 化 → `gcc -O3` → `dlopen` → 実行 |
| **aot-cached** | `-c`: 既存 `code_store/` を再利用 (再リンク + dlopen のみ) |
| **pg-compile** | `--clear-cs --pg-compile` (abruby 流): インタプリタで実行 + 末尾でホットエントリのみ AOT |
| **pg-cached** | `-c` 後続起動: `profile.txt` を自動読み込み、コールドエントリは default dispatcher のまま |
| **chibi / guile** | 比較対象 (chibi 0.12, guile 3.0 with JIT) |

ascheme の最速モード (`aot-cached` または `pg-cached`) を他処理系と比較:

| bench | ascheme 最速 | chibi | guile | vs chibi |
|---|---:|---:|---:|---|
| ack | **0.53s** | 0.74 | 2.49 | 1.4× 速い |
| fib | **0.97s** | 1.40 | 4.70 | 1.4× 速い |
| list | 1.15s | **0.87** | 1.30 | 0.76× (chibi 勝ち — cons-heavy) |
| loop | 0.92s | **0.91** | 3.31 | 同等 |
| sieve | **0.96s** | 1.48 | 5.14 | 1.5× 速い |
| sum | 1.29s | **1.27** | 5.39 | 同等 |
| tak | **1.01s** | 1.59 | 6.39 | 1.6× 速い |

guile JIT には全戦勝。bench-big の更に大きい計算 (mandel / nbody /
fannkuch / nqueens / matmul / sieve_big …) でも傾向は同様。

## 制限・非対応

- **`dynamic-wind`** — `call/cc` は脱出のみ。再進入は明示エラー。
- **`syntax-rules`** — 一切のユーザマクロは未実装。`quasiquote` /
  `unquote` / `unquote-splicing` はリーダで認識しコンパイラで `cons` /
  `list` / `append` に展開する。
- **演算子の再定義** — `(set! + my+)` は正しく動く (各特化ノードの
  `arith_cache` が runtime で検出してフォールバック)。多値も同様。
- **ホットコード入替時の対応** — REPL で再定義された関数のキャッシュは
  `c->globals` の値を直接見るので即時反映。`profile.txt` の場所は
  `code_store/profile.txt` 固定。
- **R7RS 互換ではない** — chibi の R7RS テストは多くが `(import …)` を
  要求するので走らない (`bench/compare.sh` は chibi 起動時に `(scheme
  base) (scheme write)` を `-m` で読み込ませて回避)。

## ファイル構成

```
sample/ascheme/
├── README.md             この文書 (overview)
├── docs/runtime.md       実装詳細
├── node.def              AST ノード定義 (40 種)
├── ascheme_gen.rb        ASTroGen 拡張 (`@ref` cache 構造体のハッシュ/dump/specialize)
├── context.h             VALUE / sobj / CTX / GC + GMP プロトタイプ
├── node.h                NodeHead / NODE / EVAL マクロ
├── node.c                ランタイム配線 (allocate, OPTIMIZE, generated 取り込み)
├── main.c                リーダ・コンパイラ・プリミティブ・3 ドライバ (interp / AOT / PGO)
├── Makefile              build / test / bench / bench-big
├── test/                 16 件の自前テスト + chibi r5rs-tests 機械変換
├── bench/small/          ~1 秒帯のマイクロベンチ
├── bench/big/            数秒〜数十秒の重ベンチ
├── bench/compare.sh      ascheme 5 モード × {chibi, guile} 表
├── code_store/           AOT 生成物 (gitignore)
└── .chibi/               compare.sh が拾ってくる chibi-scheme (gitignore)
```

## ノード設計の要点

| カテゴリ | ノード |
|---|---|
| 定数 | `node_const_int(64)`, `node_const_double`, `node_const_str/sym/char/bool`, `node_const_nil`, `node_const_unspec`, `node_quote` |
| 変数 | `node_lref`, `node_lset`, `node_gref` (`gref_cache @ref`), `node_gset`, `node_gdef` |
| 制御 | `node_if`, `node_seq`, `node_lambda` |
| 呼出 | `node_call_0`〜`node_call_4`, `node_call_n`, `node_callcc` |
| 特化算術 | `node_arith_{add,sub,mul,lt,le,gt,ge,eq}` (`arith_cache @ref`) |
| 特化 pred / vec | `node_pred_{null,pair,car,cdr,not}`, `node_vec_{ref,set}` |

`(set! + my+)` 等の R5RS 再定義に対応するため、各特化ノードの `arith_cache`
は実行時に `c->globals[index].value == PRIM_*_VAL` を確認してから fast path
に入る。詳細は [`docs/runtime.md`](./docs/runtime.md) §3 参照。
