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
make bench ITERS=30                # cross-engine bench
                                   #   asom × SOM++ × TruffleSOM × PySOM (×AST/×BC)
                                   #   inner-work + wall-clock の 2 表
make bench-suite                   # asom 単独で Suite.som（verifyResult のみ）
make bench-aot BENCH=Sieve         # 個別 AOT bake & 計測
make bench-pg  BENCH=Sieve         # 個別 PG bake & 計測
make testsuite                     # SOM-st/SOM TestSuite (24 ファイル)
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
| **AOT/PG** | 配線完了。`-c` で `code_store/all.so` を全 method body 分生成、`-p` で post-run hot-only bake。SD-bake は static inline で child node を全部展開しているが、modern x86 の BTB が interp の `(*head.dispatcher)(c, n)` indirect call を学習してしまうため、bench スループットでは interp と差が出ない（命令数も同じ）。差は cold start と icache 局所性で出る程度。 |

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
benchmark    |  interp |     aot |   SOM++ | aot vs SOM++
-------------+---------+---------+---------+----------------
Sieve        |  1.36 s |  0.80 s |  5.57 s | asom 6.95× faster
Queens       |  1.08 s |  1.09 s |  4.83 s | asom 4.45× faster
Fannkuch     |  0.75 s |  0.40 s |  1.70 s | asom 4.22× faster
BubbleSort   |  1.03 s |  0.62 s |  2.17 s | asom 3.50× faster
Bounce       |  1.02 s |  1.00 s |  3.20 s | asom 3.20× faster
QuickSort    |  1.07 s |  0.71 s |  1.80 s | asom 2.53× faster
Permute      |  1.03 s |  0.91 s |  1.86 s | asom 2.04× faster
List         |  1.22 s |  1.14 s |  2.25 s | asom 1.98× faster
Storage      |  1.18 s |  1.13 s |  1.87 s | asom 1.66× faster
Towers       |  1.20 s |  1.05 s |  1.59 s | asom 1.51× faster
Mandelbrot   |  0.55 s |  0.45 s |  0.51 s | asom 1.13× faster
TreeSort     |  1.14 s |  1.19 s |  1.27 s | asom 1.07× faster
```

#### take-aways（sustained）

**asom-aot は 12 ベンチすべてで SOM++ (`USE_TAGGING` + COPYING GC,
g++ -O3 -flto) より速い** — Sieve で **6.95×**、Fannkuch / Queens で 4×
台、BubbleSort / Bounce で 3× 台。AST-PE の SD-bake が **method / block
boundary の indirect call を消して** GCC に LICM / scalar replacement の
機会を作るのが効いていて、interp 比で sustained Sieve が 1.36s → 0.80s
(1.7×) になり、SOM++ の bytecode dispatch loop を抜く。

interp 単独でも SOM++ に勝つベンチが多い（Sieve/Queens で 4× 以上）。
原因の積算:

- **型特化 send** で `+ - * < > <= >= = at: at:put:` の 1-2 引数送信が
  IC + primitive 関数ポインタ呼出しを一切経由せず inline 算術に縮む。
- **制御フロー inline** (`ifTrue: / whileTrue: / to:do:` 系) で
  `asom_block_invoke` の calloc + setjmp が消える。`whileTrue:` は
  ループ全体で 1 frame を再利用、`to:do:` は C-level for ループ。
- **frame pool / double arena / block arena** で残った calloc も pool pop
  か bump alloc に。
- **`m->no_nlr` setjmp スキップ** で、本体に nested block を含まない
  メソッドは `setjmp` 自体を実行しない（再帰系ベンチ Towers/Queens に効く）。
- **AOT/PG が cached load でも効くようになった** — 制御フロー inline ノード
  の operand に body subtree を直接出すよう修正、block body も
  cs_compile entry に登録するよう修正、`node_string_val / node_array_lit`
  をハッシュ決定論的に。これで SD shard が cs_load で正しくマッチして
  SD-bake が interp に対して実速度差を生むようになった。

短い (~ms) 計測では SOM++ の per-call setup（class load、IC prime、
COPYING space init）が表に出てしまって SOM++ が速く見えるが、sustained
で測れば asom が速い。

#### vs その他エンジン（参考、canonical 小規模）

`make bench ITERS=5`（rebench.conf 由来の小さな inner、各エンジンの
per-call setup を含む）。warmup-blind な「処理系一式・冷たい状態の単発」
を見るには有用だが、interp 系は sub-100ms 領域で setup-bound になり
易いので絶対値は信用しすぎないこと:

```
benchmark    |    interp |       aot |        pg |     SOM++ |    Truffle |  PySOM-AST |   PySOM-BC
-------------+-----------+-----------+-----------+-----------+------------+------------+-----------
Sieve        |     0.004 |     0.004 |     0.004 |     0.003 |      0.000 |      0.107 |      0.295
Permute      |     0.016 |     0.014 |     0.015 |     0.006 |      0.000 |      0.678 |      1.005
Towers       |     0.034 |     0.034 |     0.032 |     0.009 |      0.000 |      0.412 |      0.911
Queens       |     0.010 |     0.011 |     0.010 |     0.009 |      0.000 |      0.336 |      1.248
List         |     0.019 |     0.016 |     0.016 |     0.006 |      0.000 |      0.151 |      0.450
Storage      |     0.013 |     0.013 |     0.013 |     0.006 |      0.000 |      0.183 |      0.332
Bounce       |     0.005 |     0.005 |     0.003 |     0.003 |      0.000 |      0.173 |      0.373
BubbleSort   |     0.005 |     0.004 |     0.004 |     0.002 |      0.000 |      0.106 |      0.265
QuickSort    |     0.007 |     0.006 |     0.007 |     0.002 |      0.000 |      0.190 |      0.367
TreeSort     |     0.022 |     0.022 |     0.022 |     0.005 |      0.001 |      0.210 |      0.362
Fannkuch     |     0.040 |     0.040 |     0.040 |     0.020 |          ? |      1.319 |      2.776
Mandelbrot   |     0.595 |     0.571 |     0.598 |     0.584 |      0.001 |     24.714 |     68.032
```

- **TruffleSOM (Graal JIT)** は ~μs scale。warm peak は asom より
  100-1000× 速いが、JVM bootstrap + AST parse + libgraal compile に
  毎回 2.2–3.0 秒の固定費を払う（次節）。
- **vs PySOM-AST**: 全ベンチで asom が **5-50× 速い**。
- **vs PySOM-BC**: 全ベンチで asom が **10-100× 速い**。
- **interp / aot / pg は同水準**。AOT/PG の SD-bake は static inline で
  child node を全部展開しているが、modern x86 の BTB が interp の
  `(*head.dispatcher)(c, n)` indirect call を学習して実質ゼロコストに
  なるため、bench スループットでは差が出ない（命令数も同じ:
  interp 624M / aot 629M for Sieve 100 iter）。

### Wall-clock 表（`make bench` の 2 つ目の表、参考）

プロセス起動 + クラス・パース + JIT compile + ベンチループ全部を含む
`/usr/bin/time -f '%e'` 計測。**ユーザ体感の "コマンドが返ってくるまで" 時間**。
実装ごとの起動コストの差で大きくスキューするため、フェアな比較ではない。
（ITERS=5、canonical inner、上の表とまったく同じ trial から取得した値）

```
benchmark    |   interp |      aot |       pg |    SOM++ |    Truffle |  PySOM-AST |   PySOM-BC
-------------+----------+----------+----------+----------+------------+------------+-----------
Sieve        |    0.000 |    0.010 |    0.010 |    0.020 |      2.300 |      0.640 |      1.570
Permute      |    0.020 |    0.020 |    0.020 |    0.030 |      2.310 |      4.140 |      4.990
Towers       |    0.040 |    0.040 |    0.030 |    0.050 |      2.440 |      2.290 |      4.790
Queens       |    0.010 |    0.010 |    0.010 |    0.050 |      2.950 |      2.080 |      6.610
List         |    0.020 |    0.020 |    0.020 |    0.030 |      2.350 |      0.950 |      2.320
Storage      |    0.010 |    0.010 |    0.010 |    0.030 |      2.340 |      0.950 |      1.760
Bounce       |    0.000 |    0.010 |    0.000 |    0.020 |      2.260 |      0.930 |      2.120
BubbleSort   |    0.010 |    0.010 |    0.010 |    0.010 |      2.400 |      0.640 |      1.500
QuickSort    |    0.010 |    0.010 |    0.010 |    0.010 |      2.270 |      1.060 |      2.090
TreeSort     |    0.020 |    0.020 |    0.020 |    0.030 |      2.260 |      1.560 |      1.930
Fannkuch     |    0.040 |    0.040 |    0.040 |    0.100 |      2.210 |      6.650 |     13.890
Mandelbrot   |    0.640 |    0.610 |    0.640 |    0.590 |      2.200 |     24.830 |     68.130
```

#### take-aways (wall-clock)

- **TruffleSOM** は wall-time でほぼ常に **2.2–3.0 秒の JVM bootstrap +
  AST parse + libgraal JIT compile** 固定費を払う（Sieve でも Mandelbrot でも一定）。
  短いベンチでは不利だが、N iterations を増やすほど amortise される。
- **PySOM** も CPython 起動 + .som パースで 0.5–2 秒の固定費。
- **asom / SOM++** は C ネイティブで bare 起動が速いため、wall-time が
  inner-work とほぼ同じ。比較的 honest。
- 「**起動が速いと wall-time で得をする**」という当たり前の事実が
  出ているだけで、Engine 同士の inner-loop 性能を見るには上の inner-work
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
