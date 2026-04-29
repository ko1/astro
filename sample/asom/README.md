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

### Inner-work — `make compare ITERS=30` (デフォルト)

各エンジンの **Smalltalk 側 `system ticks`** がベンチループを計測した値。
プロセス起動・stdlib パース・JVM bootstrap・eager JIT compile などの固定費を
除外し、`iters timesRepeat: [bench benchmark]` の中身だけを比較する。
lazy JIT compile と GC pause はループ中で起きる分だけ含む。

```
benchmark    |   interp |      aot |       pg |    SOM++ |    Truffle |  PySOM-AST |   PySOM-BC |     CSOM
-------------+----------+----------+----------+----------+------------+------------+------------+---------
Sieve        |    0.088 |    0.092 |    0.089 |    0.002 |      0.000 |      0.093 |      0.255 |    0.097
Permute      |    0.132 |    0.133 |    0.130 |    0.004 |      0.000 |      0.512 |      0.770 |    0.087
Towers       |    0.377 |    0.374 |    0.377 |    0.007 |      0.000 |      0.320 |      0.690 |    0.135
Queens       |    0.117 |    0.112 |    0.116 |    0.006 |      0.000 |      0.248 |      0.938 |    0.097
List         |    0.268 |    0.264 |    0.263 |    0.005 |      0.000 |      0.126 |      0.350 |    0.133
Storage      |    0.085 |    0.090 |    0.094 |    0.004 |      0.000 |      0.123 |      0.279 |    0.519
Bounce       |    0.116 |    0.117 |    0.111 |    0.002 |      0.000 |      0.137 |      0.323 |    0.182
BubbleSort   |    0.070 |    0.069 |    0.073 |    0.001 |      0.000 |      0.084 |      0.211 |    0.122
QuickSort    |    0.113 |    0.111 |    0.115 |    0.002 |      0.000 |      0.154 |      0.307 |    0.196
TreeSort     |    0.254 |    0.259 |    0.256 |    0.004 |      0.000 |      0.157 |      0.298 |    0.488
Fannkuch     |    0.103 |    0.104 |    0.105 |    0.014 |          ? |      0.960 |      2.290 |    0.944
Mandelbrot   |    0.981 |    1.007 |    0.985 |    0.463 |      0.001 |     20.273 |     60.091 |   11.753
```

#### take-aways（inner-work）

- **TruffleSOM (Graal JIT) は ~μs**。Graal の partial evaluation が deopt
  含めて完全に効くと、整数演算ループは実質 native code。
- **SOM++ (USE_TAGGING) が asom より 30–60× 速い** (Sieve 44×, BubbleSort
  70×, Mandelbrot 2.1×)。これが本来の C ベースラインの実力で、**asom が
  型特化ノードを入れたら埋めに行くべき差**。
- **CSOM と asom の inner-loop はほぼ同水準** (~1.0× 〜 2× 程度の差で go か no go)。
  Storage/TreeSort/Mandelbrot のように GC が走るベンチでだけ asom が
  CSOM の 2–12× 速い (asom はリーク、CSOM は mark/sweep)。
- **PySOM-AST も asom と同水準**。CPython の interp overhead は意外と
  小さく、ベンチによっては asom より速い（Storage/List/TreeSort）。
- **asom interp / aot / pg はノイズレベル差**。AOT/PG インフラは動作する
  が型特化ノード未実装のため、primitive 関数ポインタ間接呼出しが律速。
  [docs/todo.md](docs/todo.md) 参照。

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
