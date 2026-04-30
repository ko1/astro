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
- [docs/todo.md](docs/todo.md) — 未実装・既知の課題（shape-based field unbox / call-graph PE / AreWeFastYet 残り / etc）
- [docs/perf.md](docs/perf.md) — 性能最適化のステップごとの履歴と最終 bench 結果

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
                                   #   asom × SOM++ × TruffleSOM
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
| **SOM TestSuite** | 221 / 221 アサーション pass (100%)、24 / 24 ファイル clean |
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

### Inner-work（`make bench` 1 表目）

各エンジンの **Smalltalk 側 `system ticks`** がベンチループを計測した値。
プロセス起動・stdlib パース・JVM bootstrap・eager JIT compile などの固定費を
除外し、`iters timesRepeat: [bench benchmark]` の中身だけを比較する。

`run_compare.rb` は ITERS=5 outer × best-of-3 trials、各 trial で最初の
2 outer (warmup) を捨てて残り 3 outer を平均。これで Truffle の JIT
compile が抜けた **warm peak** だけを採取する。`INNER` テーブルは
asom-interp で 1 outer ≈ 1 秒程度になるよう調整。

```
benchmark    |    interp |       aot |        pg |     SOM++ |   Truffle
------------------------------------------------------------------------
Sieve        |     0.505 |     0.130 |     0.126 |     1.297 |     0.029
Permute      |     0.422 |     0.292 |     0.338 |     0.724 |     0.013
Towers       |     0.418 |     0.368 |     0.334 |     0.343 |     0.035
Queens       |     0.458 |     0.380 |     0.373 |     1.230 |     0.019
List         |     0.418 |     0.343 |     0.352 |     0.479 |     0.012
Storage      |     0.389 |     0.367 |     0.332 |     0.748 |     0.037
Bounce       |     0.481 |     0.487 |     0.276 |     0.954 |     0.027
BubbleSort   |     0.522 |     0.263 |     0.262 |     0.738 |     0.020
QuickSort    |     0.493 |     0.155 |     0.157 |     0.821 |     0.023
TreeSort     |     0.511 |     0.475 |     0.407 |     0.531 |     0.025
Fannkuch     |     0.477 |     0.170 |     0.172 |     0.727 |     0.041
Mandelbrot   |     0.764 |     0.520 |     0.445 |     0.754 |     0.120
```

`sample/asom/SOMpp` (USE_TAGGING + COPYING、bytecode interp、JIT なし)
と `sample/asom/TruffleSOM` (libgraal warm peak) が submodule。
`make bench-setup` / `make bench-setup-truffle` でセットアップ後、
`make bench` から参照される。

#### vs SOM++ (bytecode interpreter, JIT なし)

asom-aot は **11/12 で勝ち** (Towers のみ 7% 負け 0.368 vs 0.343、
asom-pg にすると 12/12 勝ち):

| ベンチ | asom-aot | SOM++ | asom 倍率 |
|---|---|---|---|
| Sieve | 0.130 | 1.297 | **10.0×** |
| QuickSort | 0.155 | 0.821 | 5.3× |
| Fannkuch | 0.170 | 0.727 | 4.3× |
| Queens | 0.380 | 1.230 | 3.2× |
| BubbleSort | 0.263 | 0.738 | 2.8× |
| Storage | 0.367 | 0.748 | 2.0× |
| Bounce | 0.487 | 0.954 | 2.0× (pg 0.276 → 3.5×) |
| Permute | 0.292 | 0.724 | 2.5× |
| Mandelbrot | 0.520 | 0.754 | 1.5× |
| List | 0.343 | 0.479 | 1.4× |
| TreeSort | 0.475 | 0.531 | 1.12× |
| Towers | 0.368 | 0.343 | SOM++ 1.07× (pg 0.334 で逆転) |

整数中心ベンチ (Sieve / QuickSort / Queens / Fannkuch) で大きく勝ち、
node alloc 中心の TreeSort / Towers は接戦。AST → 型特化 C → gcc -O3
native code は stack-machine bytecode interpreter よりは確実に速い。

#### vs Truffle warm peak (libgraal、Graal JIT)

**Truffle が 12/12 勝ち、4-28×**。これは asom がまだ実装していない
最適化 (method-level PE / escape analysis / speculative deopt) の効果
が素直に出ている結果:

| ベンチ | asom-aot | Truffle | Truffle 倍率 |
|---|---|---|---|
| List | 0.343 | 0.012 | **28.6×** |
| Permute | 0.292 | 0.013 | **22.5×** |
| Queens | 0.380 | 0.019 | **20.0×** |
| TreeSort | 0.475 | 0.025 | **19.0×** |
| Bounce | 0.487 | 0.027 | **18.0×** |
| BubbleSort | 0.263 | 0.020 | **13.2×** |
| Towers | 0.368 | 0.035 | **10.5×** |
| Storage | 0.367 | 0.037 | **9.9×** |
| QuickSort | 0.155 | 0.023 | **6.7×** |
| Sieve | 0.130 | 0.029 | **4.5×** |
| Mandelbrot | 0.520 | 0.120 | **4.3×** |
| Fannkuch | 0.170 | 0.041 | **4.1×** |

object alloc + field アクセスが多いベンチ (List 28.6×, Permute 22.5×,
Queens 20×, TreeSort 19×, Bounce 18×) で差が大きい — Graal の escape
analysis が一時 Cons / Element / Tree node を完全 scalarize するのに
対し asom は alloc + field deref が残る。純 array / Double 演算
(Sieve, Fannkuch, Mandelbrot) は 4× 台、差が比較的小さい。

差を埋める道筋は [docs/perf.md の Sieve cost-model 解析](docs/perf.md#truffle-warm-peak-との差を埋める道筋--sieve-cost-model)
を参照 — inline-frame locals の register-promote と PG-driven shape
guard hoisting の 2 つで Sieve は **interp 比 10×、Truffle 比 2×** まで
詰まる見込み。

#### asom-pg の効果

PG warmup で `node_send1_dbl*` 型特化が hot path に入るので、Ball /
Tree の field 経由 Double 演算が多い Bounce / TreeSort で顕著に効く:

| ベンチ | aot | pg | 改善 |
|---|---|---|---|
| Bounce | 0.487 | **0.276** | 1.76× |
| TreeSort | 0.475 | 0.407 | 1.17× |
| Mandelbrot | 0.520 | 0.445 | 1.17× |
| Storage | 0.367 | 0.332 | 1.11× |
| Towers | 0.368 | 0.334 | 1.10× |
| その他 | tie | tie | — |

ただ PG mode でも Truffle warm peak には全く届かない (Bounce 0.276 vs
0.027 = 10×差)。PG が個別 node-specialization のみで、cross-method の
PE をやってないので。

`make bench-aot` (`-c`) は parser-time で baked された SD chain で
全エントリが SD-dispatched、整数中心ベンチで SOM++ 比 10× 出る。
Double 演算でも flonum tagging が中間 box を消すが、mantissa 低 2bit ≠ 00
の double は heap-fallback して `asom_double` の bump arena に行く
（厳密に bit-exact を保つため）。

interp 単独でも SOM++ に勝つベンチがある（Sieve 0.505 vs 1.297、Queens
0.458 vs 1.230）。AST tree-walker でも IC + 型特化 + control-flow inline
+ frame pool が積み重なれば bytecode interpreter を上回れる。
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

### Wall-clock（`make bench` 2 表目）

プロセス起動 + クラス・パース + JIT compile + ベンチループ全部を含む
`/usr/bin/time -f '%e'` 計測。**ユーザ体感の "コマンドが返ってくるまで"
時間**。ITERS=5 outer の合計実行時間。

```
benchmark    |    interp |       aot |        pg |     SOM++ |   Truffle
------------------------------------------------------------------------
Sieve        |     2.520 |     0.650 |     0.640 |     6.540 |     1.740
Permute      |     2.150 |     1.500 |     1.720 |     4.450 |     2.070
Towers       |     2.100 |     1.860 |     1.660 |     1.720 |     1.970
Queens       |     2.280 |     1.900 |     1.870 |     6.130 |     1.990
List         |     2.120 |     1.730 |     1.750 |     2.380 |     2.190
Storage      |     2.010 |     1.780 |     1.670 |     3.760 |     2.340
Bounce       |     2.410 |     2.450 |     1.380 |     4.760 |     2.060
BubbleSort   |     2.620 |     1.350 |     1.340 |     3.690 |     1.880
QuickSort    |     2.460 |     0.780 |     0.790 |     4.280 |     2.070
TreeSort     |     2.870 |     2.390 |     2.010 |     2.630 |     2.150
Fannkuch     |     2.370 |     0.860 |     0.860 |     3.730 |     1.900
Mandelbrot   |     3.740 |     2.550 |     2.180 |     3.770 |     2.500
```

短時間タスクで asom が逆転する例:

| ベンチ | asom-aot | Truffle | 差 |
|---|---|---|---|
| Sieve | 0.65 | 1.74 | asom **2.7×** |
| QuickSort | 0.78 | 2.07 | asom **2.7×** |
| Fannkuch | 0.86 | 1.90 | asom **2.2×** |
| BubbleSort | 1.35 | 1.88 | asom 1.4× |
| Bounce (pg) | 1.38 | 2.06 | asom 1.5× |
| Mandelbrot (pg) | 2.18 | 2.50 | asom 1.15× |
| List | 1.73 | 2.19 | asom 1.27× |

- **TruffleSOM** は wall-clock で常に **1.5–2.5 秒の JVM bootstrap +
  AST parse + libgraal JIT compile** が固定費として乗る（Sieve でも
  Mandelbrot でも一定）。short-run では割合が大きく、long-run で薄まる。
- **asom / SOM++** は C ネイティブで bare 起動が速いため、wall-clock が
  inner-work × ITERS にほぼ比例。
- 「コマンド一発打って結果が返ってくるまで」を比較するならこの表、
  「inner-loop の純粋なスループット (warm peak)」を比較するなら上の
  inner-work 表。用途で読み分ける。

つまり **「コマンド一発で終わる短いタスク」では asom 有利、「長時間
batch で warm peak が支配」では Truffle 有利**、というクラシックな
AOT vs JIT のトレードオフ。

## 設計上の特徴

- **Tagged 62-bit SmallInteger + flonum-tagged Double** (低 2bit: `01` = int、`10` = flonum、`00` = pointer)。flonum エンコードは bit-exact（mantissa 低 2bit が 00 でないものは heap-boxed `asom_double` にフォールバック）
- **GMP-backed LargeInteger**（62-bit 超は `mpz_t` で boxed、`int_*` primitive で `__builtin_*_overflow` 検出 → 自動 promotion）
- **Boehm-Demers-Weiser GC**（`<gc.h>` を `malloc`/`calloc`/`realloc`/`strdup`/`free` マクロで一括 wrap、`__thread` 排除で TLS scan の問題回避）
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

## Limitations（SOM 仕様未充足）

実行する SOM プログラムが期待する振る舞いを満たせていない箇所。

- **AreWeFastYet 残り**: Havlak / CD / Knapsack / PageRank — 言語機能
  の不足で走らないか単に未追加かは未確認。

GC / Bignum は実装済み:
- **Boehm GC** で `asom_object` 系は GC 管理。long-running OK、bench でも
  GC コストを払っている（Truffle / SOM++ と同条件）
- **GMP LargeInteger** で 62-bit 超 Integer も透過的に動作。IntegerTest
  25/25 通過、2^100 等の `raisedTo:` も bignum で正確に計算

## Planned（性能・拡張の TODO）

仕様は満たすが、最適化や開発体験のためにやりたいこと。

- **Shape-based field unbox** — Ball / Tree の field レベル Double が
  boxed のまま。`make bench-aot` 単独だと Bounce / TreeSort で Truffle
  に負ける。`make bench-pg` の warmup-driven `node_send1_dbl*` 特化で
  逆転しているが、parser-time / AOT bake 時にも解決したい。
- **Call-graph PE** — Towers の再帰メソッドが inline されない。PG mode
  でも Truffle に勝てない唯一のベンチ。
- **`make compile` / JIT デモの配線** — ASTro JIT は naruby のような
  L0/L1/L2 構成、asom は未連携。
- **AreWeFastYet 残り（Havlak / CD / Knapsack / PageRank）の検証 +
  追加** — 動かしてみて、足りない言語機能があれば埋め、ベンチハーネス
  に追加する。

詳細は [docs/todo.md](docs/todo.md) 参照。
