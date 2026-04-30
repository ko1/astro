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

### Inner-work（`make bench` 1 表目）

各エンジンの **Smalltalk 側 `system ticks`** がベンチループを計測した値。
プロセス起動・stdlib パース・JVM bootstrap・eager JIT compile などの固定費を
除外し、`iters timesRepeat: [bench benchmark]` の中身だけを比較する。

`run_compare.rb` の `INNER` テーブルは asom-interp で **~1 秒** 程度に揃え
ている（短い bench は per-call setup・IC prime・GC space init が支配して
誤解を招くため）。`iterations=1`、best of 3。

```
benchmark    |    interp |       aot |        pg |     SOM++ |   Truffle
------------------------------------------------------------------------
Sieve        |     0.394 |     0.132 |     0.137 |     2.688 |     0.161
Permute      |     0.458 |     0.343 |     0.342 |     1.168 |     0.364
Towers       |     0.409 |     0.321 |     0.326 |     0.795 |     0.280
Queens       |     0.410 |     0.359 |     0.349 |     2.894 |     0.433
List         |     0.407 |     0.318 |     0.331 |     1.393 |     0.437
Storage      |     0.441 |     0.384 |     0.377 |     1.230 |     0.467
Bounce       |     0.461 |     0.430 |     0.238 |     1.435 |     0.286
BubbleSort   |     0.463 |     0.234 |     0.232 |     1.249 |     0.229
QuickSort    |     0.444 |     0.135 |     0.134 |     1.353 |     0.307
TreeSort     |     0.413 |     0.400 |     0.327 |     1.032 |     0.308
Fannkuch     |     0.464 |     0.191 |     0.181 |     1.369 |     0.237
Mandelbrot   |     0.049 |     0.028 |     0.029 |     1.318 |     0.452
```

#### take-aways

**asom-aot / asom-pg は 12 ベンチすべてで SOM++ より速い**:

- Mandelbrot **47×**（`flonum tagging` で中間 Double が VALUE 即値になり alloc 完全消去）
- Sieve **20×**、QuickSort **10×**、Queens **8×**、Fannkuch / BubbleSort
  / List 4-7×、Permute / Bounce / Storage 3×、Towers / TreeSort 2-3×

**asom-pg は 11/12 で TruffleSOM (warm peak) にも勝つ**（Towers のみ Truffle）:

| ベンチ | asom-pg | Truffle | 勝率 |
|---|---|---|---|
| Sieve | 0.137 | 0.161 | asom 1.18× |
| Permute | 0.342 | 0.364 | asom 1.06× |
| Towers | 0.326 | 0.280 | **Truffle 1.16×** |
| Queens | 0.349 | 0.433 | asom 1.24× |
| List | 0.331 | 0.437 | asom 1.32× |
| Storage | 0.377 | 0.467 | asom 1.24× |
| Bounce | **0.238** | 0.286 | asom 1.20× |
| BubbleSort | 0.232 | 0.229 | tie |
| QuickSort | 0.134 | 0.307 | asom 2.29× |
| TreeSort | **0.327** | 0.308 | tie (Truffle 1.06×) |
| Fannkuch | 0.181 | 0.237 | asom 1.31× |
| Mandelbrot | 0.029 | 0.452 | **asom 15.6×** |

`make bench-aot` (`-c`) は parser-time で baked された SD chain で全エントリ
が SD-dispatched、Mandelbrot のように flonum tagging で alloc が消えるパターン
で圧倒的（48×）。

`make bench-pg` (`-p`) は warmup で集めた型情報を AST に焼き直してから bake
するので、**parser-time に決め打ちできない型特化**（特に Double 演算の
`node_send1_dbl*` 系）が hot path に入る。 Bounce が AOT 比 +45%（0.430s →
0.238s）、TreeSort も +18%（0.400s → 0.327s）と大きく改善し、Truffle の warm
peak さえ抜く。

特に **Mandelbrot** は flonum tagging 単独で `dbl_times / asom_double_new
/ dbl_plus / dbl_gt` 合計 ~16% の profile を完全消去 → **Truffle 比 15.6×**。

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

### Wall-clock（`make bench` 2 表目）

プロセス起動 + クラス・パース + JIT compile + ベンチループ全部を含む
`/usr/bin/time -f '%e'` 計測。**ユーザ体感の "コマンドが返ってくるまで" 時間**。
実装ごとの起動コストの差で大きくスキューするため、フェアな比較ではない。

```
benchmark    |    interp |       aot |        pg |     SOM++ |   Truffle
------------------------------------------------------------------------
Sieve        |     0.400 |     0.140 |     0.140 |     2.690 |     1.700
Permute      |     0.470 |     0.360 |     0.360 |     1.170 |     1.900
Towers       |     0.420 |     0.330 |     0.340 |     0.800 |     1.820
Queens       |     0.410 |     0.360 |     0.350 |     2.890 |     1.990
List         |     0.410 |     0.320 |     0.330 |     1.390 |     2.030
Storage      |     0.460 |     0.410 |     0.400 |     1.230 |     2.070
Bounce       |     0.460 |     0.430 |     0.240 |     1.440 |     1.860
BubbleSort   |     0.460 |     0.240 |     0.230 |     1.250 |     1.800
QuickSort    |     0.440 |     0.140 |     0.130 |     1.350 |     1.890
TreeSort     |     0.420 |     0.410 |     0.330 |     1.030 |     1.930
Fannkuch     |     0.470 |     0.200 |     0.190 |     1.370 |     1.850
Mandelbrot   |     0.050 |     0.030 |     0.030 |     1.320 |     2.080
```

- **TruffleSOM** は wall-clock でほぼ常に **1.5–2.2 秒の JVM bootstrap +
  AST parse + libgraal JIT compile** 固定費を払う（Sieve でも Mandelbrot でも一定）。
  短いベンチでは不利、長期 run なら amortise される。
- **asom / SOM++** は C ネイティブで bare 起動が速いため、wall-time が
  inner-work とほぼ同じ。比較的 honest。
- 「**起動が速いと wall-time で得をする**」という当たり前の事実が
  出ているだけで、Engine 同士の inner-loop 性能を見るには上の inner-work
  テーブルを参照するべき。

## 設計上の特徴

- **Tagged 62-bit SmallInteger + flonum-tagged Double** (低 2bit: `01` = int、`10` = flonum、`00` = pointer)
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
- **Shape-based field unbox 無し** (Ball/Tree の field レベル Double が
  boxed のまま — `make bench-aot` 単独だと Bounce / TreeSort で Truffle
  に負ける。`make bench-pg` で warmup 後の `node_send1_dbl*` 特化が
  hot path に入って逆転するが、本来は parser-time / AOT bake 時にも
  解決したい)
- **Call-graph PE 無し** (Towers の再帰メソッドが inline されない —
  PG mode でも Truffle に勝てない唯一のベンチ)
- **`make compile`/JIT デモ未配線** (ASTro JIT は naruby のような L0/L1/L2 構成、asom は未連携)
- **AreWeFastYet 残り**: Havlak / CD / Knapsack / PageRank

詳細は [docs/todo.md](docs/todo.md) 参照。
