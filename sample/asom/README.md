# asom: ASTro SOM

[SOM](https://som-st.github.io/) (Simple Object Machine — 教育用 Smalltalk
dialect) を **ASTro** AST 部分評価フレームワーク上に実装したサンプル。

公式の [SOM-st/SOM](https://github.com/SOM-st/SOM) を git submodule として取り
込み、その標準ライブラリ・テストスイート・AreWeFastYet ベンチマークを直接実行
できる。

```
sample/asom/SOM/
  Smalltalk/    — 標準ライブラリ (Object, Integer, String, Block, ...)
  TestSuite/    — TestHarness.som + ~30 *Test.som
  Examples/     — Hello.som, Snake, AreWeFastYet, Benchmarks/
  specification/— SOM.g4 (ANTLR 文法。これを読んで parser を書いた)
```

ドキュメント:

- [docs/runtime.md](docs/runtime.md) — ランタイム構造（VALUE 表現 / クラス階層 / メソッドディスパッチ / ブロック / AOT・PG）
- [docs/done.md](docs/done.md) — 実装済み機能の網羅リスト
- [docs/todo.md](docs/todo.md) — 未実装・既知の課題（Bignum / 型特化ノード / GC / etc）

## ビルド & 実行

```sh
git submodule update --init sample/asom/SOM
cd sample/asom
make
```

```sh
./asom Hello                       # interp → "Hello, World from SOM"
./asom -c MyClass                  # AOT-compile every method, then run
./asom -p MyClass                  # run, then PG-compile hot entries
./asom --plain MyClass             # bypass code store entirely

make test                          # ローカルスモークテスト
make bench                         # 11 AreWeFastYet ベンチマーク (interp)
make bench-aot BENCH=Sieve         # 個別 AOT bake & 計測
make bench-pg  BENCH=Sieve         # 個別 PG bake & 計測
make testsuite                     # SOM-st/SOM TestSuite (24 ファイル)
make compare ITERS=30              # asom × SOM++ × TruffleSOM × PySOM × CSOM
```

`-c` / `-p` のフラグは abruby の語彙を踏襲。詳しい使い方は
[docs/done.md#astro-統合](docs/done.md#astro-統合) を参照。

## 状態スナップショット

| 項目 | 状態 |
|------|------|
| **SOM TestSuite** | 216 / 221 アサーション pass (97.7%)、23 / 24 ファイル clean。残る 5 失敗は全部 Bignum (>2⁶²) |
| **AreWeFastYet** | 16 ベンチマーク全部 verifyResult 込みで OK（Sieve, Permute, Towers, Queens, List, Storage, Bounce, BubbleSort, QuickSort, TreeSort, Mandelbrot, Fannkuch, Richards, DeltaBlue, Json, NBody, GraphSearch） |
| **AST ノード** | 25 種（int/double/string/symbol/array literal、send 0–8、super send 0–8、block、return-local/NLR、class field 等） |
| **プリミティブ** | ~200 個（Object/Class/Method/Boolean/Integer/Double/String/Symbol/Array/Block × 4/Nil/System/Random） |
| **AOT/PG** | 配線完了。`-c` で `code_store/all.so` を全 method body 分生成、`-p` で post-run hot-only bake。実効速度は ~3% 程度（律速は `asom_send` 経由の primitive 関数ポインタ間接呼出し、[todo.md](docs/todo.md#型特化ノード--aot-を効かせる主要施策) 参照） |

## 性能

asom (interp / aot / pg) を、同じ `SOM-st/SOM/Examples/Benchmarks/<Name>.som`
を実行する他の SOM 実装と並べる。秒、best of 3、`ITERS=30`。

| Column | Engine | Build |
|--------|--------|-------|
| **interp** | asom `--plain` — tree-walking AST interpreter, no code store | gcc -O2 |
| **aot** | asom after `-c`: `cs_compile(NULL)` 全エントリ → `cs_build` → cached run | gcc -O2, AOT shard `-O3 -fPIC` |
| **pg** | asom after `-p` (threshold 1): post-run hot bake | 同上 |
| **SOM++** | SOM-st/SOMpp, C++ bytecode VM, **`USE_TAGGING` + COPYING GC** | g++ -O3 -flto |
| **Truffle** | TruffleSOM (Java + GraalVM libgraal JIT) | Graal JDK 25 |
| **PySOM-AST** | SOM-st/PySOM AST interpreter, plain CPython (no RPython) | CPython 3.12 |
| **PySOM-BC** | SOM-st/PySOM bytecode interpreter, plain CPython | 同上 |
| **CSOM** | SOM-st/CSOM, plain C bytecode VM, mark/sweep GC (参考値) | gcc -O3 |

### Inner-work — sustained run (~1 秒)

各エンジンの **Smalltalk 側 `system ticks`** がベンチループを計測した値。
プロセス起動・stdlib パース・JVM bootstrap・eager JIT compile などの固定費を
除外し、`iters timesRepeat: [bench benchmark]` の中身だけを比較する。

upstream の `rebench.conf` にあるカノニカル `extra_args`（inner iterations）は
warm な TruffleSOM 上で ~1 秒で済むよう調律されているため、interp 系で測ると
sub-100 ms となり per-call overhead が支配する。エンジン間の **steady-state
スループット**を比較するには両エンジンが秒オーダーで走る規模が必要。
以下は `inner` を **両方が ~1 秒程度走る** よう調整した値（`iterations=1`、best of 3）:

```
benchmark    | inner |    asom |   SOM++ | 比
-------------+-------+---------+---------+------
Sieve        |  2000 |  1.32 s |  5.30 s | asom 4.0× faster
Queens       |   700 |  1.04 s |  4.69 s | asom 4.5× faster
Bounce       |  1250 |  1.01 s |  3.08 s | asom 3.0× faster
Fannkuch     |     9 |  0.68 s |  1.73 s | asom 2.5× faster
BubbleSort   |  1500 |  1.07 s |  2.17 s | asom 2.0× faster
Permute      |   400 |  1.01 s |  1.89 s | asom 1.9× faster
List         |   400 |  1.10 s |  2.04 s | asom 1.9× faster
QuickSort    |   900 |  1.02 s |  1.79 s | asom 1.75× faster
Storage      |   500 |  1.05 s |  1.71 s | asom 1.6× faster
Towers       |   200 |  1.12 s |  1.51 s | asom 1.3× faster
TreeSort     |   300 |  1.19 s |  1.30 s | tied
Mandelbrot   |   200 |  0.55 s |  0.51 s | tied
```

#### take-aways（sustained）

**asom は SOM++ (`USE_TAGGING` + COPYING GC, g++ -O3 -flto) を**
**ほぼ全てのベンチで上回る** — 整数中心（Sieve/Queens/Bounce で 3-4.5×）から
double 計算（Fannkuch 2.5×、Mandelbrot 並走）まで。原因の積算:

- **型特化 send** で `+ - * < > <= >= = at: at:put:` の 1-2 引数送信が
  IC + primitive 関数ポインタ呼出しを一切経由せず inline 算術に縮む。
- **制御フロー inline** (`ifTrue: / whileTrue: / to:do:` 系) で
  `asom_block_invoke` の calloc + setjmp が消える。`whileTrue:` は
  ループ全体で 1 frame を再利用、`to:do:` は C-level for ループ。
- **frame pool / double arena / block arena** で残った calloc も pool pop
  か bump alloc に。
- **`m->no_nlr` setjmp スキップ** で、本体に nested block を含まない
  メソッドは `setjmp` 自体を実行しない（再帰系ベンチ Towers/Queens に効く）。

短い (~ms) 計測では SOM++ の per-call setup（class load、IC prime、
COPYING space init）が表に出てしまって SOM++ が速く見えるが、sustained
で測れば asom が速い。

#### vs その他エンジン（参考、canonical 小規模）

upstream rebench の小さな inner で `make compare ITERS=10` を走らせた
出力。`iterations=1, inner_iterations=1` 程度なので各エンジンの
per-call setup を含む。warmup-blind な「処理系一式・冷たい状態の単発」を
見るには有用だが、絶対値は信用しすぎないこと:

```
benchmark    |   interp |    SOM++ |    Truffle |  PySOM-AST |   PySOM-BC |     CSOM
-------------+----------+----------+------------+------------+------------+---------
Sieve        |    0.006 |    0.002 |      0.000 |      0.090 |      0.243 |    0.099
Permute      |    0.024 |    0.004 |      0.000 |      0.505 |      0.740 |    0.085
Towers       |    0.051 |    0.007 |      0.000 |      0.302 |      0.679 |    0.128
Queens       |    0.014 |    0.006 |      0.000 |      0.249 |      0.909 |    0.099
List         |    0.025 |    0.004 |      0.000 |      0.119 |      0.345 |    0.129
Storage      |    0.020 |    0.004 |      0.000 |      0.128 |      0.266 |    0.488
Bounce       |    0.007 |    0.002 |      0.000 |      0.137 |      0.282 |    0.171
BubbleSort   |    0.006 |    0.001 |      0.000 |      0.080 |      0.212 |    0.121
QuickSort    |    0.010 |    0.002 |      0.000 |      0.153 |      0.309 |    0.191
TreeSort     |    0.034 |    0.004 |      0.000 |      0.166 |      0.290 |    0.486
Fannkuch     |    0.033 |    0.014 |          ? |      0.980 |      2.303 |    0.916
Mandelbrot   |    0.482 |    0.452 |      0.001 |     20.233 |     58.944 |   11.558
```

- **TruffleSOM (Graal JIT)** は ~μs scale。warm peak は asom より
  100-1000× 速いが、JVM bootstrap + AST parse + libgraal compile に
  毎回 1.7-2.0 秒の固定費を払う（次節）。
- **vs PySOM-AST**: 全ベンチで asom が **5-60× 速い**。
- **vs CSOM**: 全ベンチで asom が **3-25× 速い**。CSOM は教育用なので
  公平比較ではないが、参考までに。
- **interp / aot / pg は同水準**。AOT/PG の SD-bake は static inline で
  child node を全部展開しているが、modern x86 の BTB が interp の
  `(*head.dispatcher)(c, n)` indirect call を学習して実質ゼロコストに
  なるため、bench スループットでは差が出ない（命令数も同じ:
  interp 624M / aot 629M for Sieve 100 iter）。

### Wall-clock — `make compare-wall ITERS=30` (参考)

プロセス起動 + クラス・パース + JIT compile + ベンチループ全部を含む
`/usr/bin/time -f '%e'` 計測。**ユーザ体感の "コマンドが返ってくるまで" 時間**。
実装ごとの起動コストの差で大きくスキューするため、フェアな比較ではない。

```
benchmark    |   interp |      aot |       pg |    SOM++ |   Truffle  |  PySOM-AST |   PySOM-BC |     CSOM
-------------+----------+----------+----------+----------+------------+------------+------------+---------
Sieve        |     0.10 |     0.10 |     0.10 |     0.08 |       1.89 |       2.81 |       7.28 |     3.32
Permute      |     0.15 |     0.15 |     0.15 |     0.15 |       1.90 |      20.66 |      24.79 |     2.94
Towers       |     0.43 |     0.43 |     0.43 |     0.24 |       1.99 |      10.46 |      22.22 |     4.76
Queens       |     0.14 |     0.14 |     0.14 |     0.22 |       2.03 |       9.75 |      31.97 |     3.56
List         |     0.32 |     0.32 |     0.31 |     0.18 |       2.04 |       4.03 |      11.09 |     4.22
Storage      |     0.10 |     0.10 |     0.10 |     0.11 |       1.79 |       4.19 |       8.85 |    16.47
Bounce       |     0.12 |     0.12 |     0.12 |     0.07 |       1.85 |       3.85 |       9.17 |     5.80
BubbleSort   |     0.07 |     0.08 |     0.07 |     0.05 |       1.85 |       2.57 |       6.42 |     3.95
QuickSort    |     0.13 |     0.12 |     0.13 |     0.06 |       1.84 |       4.72 |       9.34 |     6.78
TreeSort     |     0.31 |     0.32 |     0.30 |     0.13 |       2.07 |       7.65 |       9.18 |    14.99
Fannkuch     |     0.11 |     0.11 |     0.11 |     0.08 |       1.82 |       5.22 |      11.88 |     4.96
Mandelbrot   |     1.17 |     1.09 |     1.08 |     0.47 |       1.69 |      21.07 |      61.09 |    11.82
```

#### take-aways (wall-clock)

- **TruffleSOM** は wall-time でほぼ常に 1.7–2.0 秒の **JVM bootstrap +
  AST parse + libgraal JIT compile** 固定費を払う。短いベンチでは不利だが、
  N iterations を増やすほど amortise される。
- **PySOM** も CPython 起動 + .som パースで 1–2 秒の固定費。
- **CSOM** の wall-time が大きいのは、ほとんどが mark/sweep の class
  load + parse のコスト。inner-loop は asom と同水準（上の表）なので、
  「asom が CSOM の 14–170× 速い」というのは **wall-time のミスリード**。
- **asom / SOM++** は C ネイティブで bare 起動が速いため、wall-time が
  inner-work と近い値になる。比較的 honest。

要するに「**起動が速いと wall-time で得をする**」という当たり前の事実が
表に出ているだけで、Engine 同士の inner-loop 性能 を見るには上の inner-work
テーブルを参照するべき。

## 設計上の特徴

- **Tagged 62-bit SmallInteger** (低 1bit = 1 が整数、0 が pointer)
- **Per-callsite IC** (`struct asom_callcache *cc@ref`) — Merkle ハッシュから除外
- **`asom_send` / `asom_invoke_method` の IC fast path を `asom_runtime.h` で `static inline`** 化、SD コードに直接インライン展開
- **Per-class metaclass + metaclass-of-metaclass** (`1 class class superclass = Object class`)
- **Block1 / Block2 / Block3 split** (0/1/2-arg blocks の独自クラス)
- **Lexical scope chain** (block の `lexical_parent` 経由で `node_local_get(scope, idx)` を辿る)
- **escapedBlock catcher** (home が return 済みの `^` を per-block setjmp で受けて `escapedBlock:` dispatch)
- **`doesNotUnderstand:arguments:` / `unknownGlobal:`** dispatch
- **stdlib overlay merge** (bootstrap class に `<.som>` を後付けで上乗せ、primitive を残す)
- **ws-aware symbol literal** (`#between:and: withArguments:` の境界判定)
- **`-rdynamic`** リンクで SD `.so` から `asom_send_slow` 等を resolve

## Limitations / 未実装

- **GC 無し** (リーク)。bench は走り切るが long-running は危険
- **Bignum 無し** (62-bit tagged で打ち止め — IntegerTest 5 失敗の要因)
- **Unboxed double 無し** (Mandelbrot で SOM++ 比 1.9× 遅い主因)
- **型特化ノード未実装** (`node_fixnum_plus` 系。AOT が現状 ~3% しか効かない主因)
- **`make compile`/JIT デモ未配線** (ASTro JIT は naruby のような L0/L1/L2 構成、asom は未連携)
- **AreWeFastYet 残り**: Havlak / CD / Knapsack / PageRank

詳細は [docs/todo.md](docs/todo.md) 参照。
