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

`run_compare.rb` の `INNER` テーブルは asom-interp で **~1 秒** 程度に揃え
ている（短い bench は per-call setup・IC prime・GC space init が支配して
誤解を招くため）。`iterations=1`、best of 3。

```
benchmark    |    interp |       aot |        pg |     SOM++ |   Truffle
------------------------------------------------------------------------
Sieve        |     0.438 |     0.128 |     0.127 |     1.338 |     0.161
Permute      |     0.445 |     0.322 |     0.311 |     0.601 |     0.419
Towers       |     0.390 |     0.309 |     0.297 |     0.335 |     0.308
Queens       |     0.455 |     0.382 |     0.378 |     1.229 |     0.458
List         |     0.424 |     0.353 |     0.342 |     0.422 |     0.447
Storage      |     0.371 |     0.302 |     0.295 |     0.659 |     0.427
Bounce       |     0.449 |     0.467 |     0.281 |     0.913 |     0.278
BubbleSort   |     0.475 |     0.250 |     0.249 |     0.664 |     0.229
QuickSort    |     0.460 |     0.157 |     0.162 |     0.829 |     0.304
TreeSort     |     0.414 |     0.410 |     0.331 |     0.471 |     0.292
Fannkuch     |     0.470 |     0.171 |     0.168 |     0.719 |     0.229
Mandelbrot   |     0.708 |     0.485 |     0.412 |     0.768 |     0.471
```

`sample/asom/SOMpp` (USE_TAGGING + COPYING) と `sample/asom/TruffleSOM`
(libgraal warm peak) が submodule。`make bench-setup` / `make
bench-setup-truffle` でセットアップ後、`make bench` から参照される。

#### take-aways

**asom-aot は 12 ベンチすべてで SOM++ より速い**（USE_TAGGING + Release）:

| ベンチ | asom-aot | SOM++ | asom 倍率 |
|---|---|---|---|
| Sieve | 0.128 | 1.338 | **10.5×** |
| QuickSort | 0.157 | 0.829 | 5.3× |
| Fannkuch | 0.171 | 0.719 | 4.2× |
| Queens | 0.382 | 1.229 | 3.2× |
| BubbleSort | 0.250 | 0.664 | 2.7× |
| Storage | 0.302 | 0.659 | 2.2× |
| Bounce | 0.467 | 0.913 | 2.0× |
| Permute | 0.322 | 0.601 | 1.9× |
| Mandelbrot | 0.485 | 0.768 | 1.6× |
| List | 0.353 | 0.422 | 1.2× |
| Towers | 0.309 | 0.335 | 1.08× |
| TreeSort | 0.410 | 0.471 | 1.15× |

整数中心ベンチ (Sieve / QuickSort / Queens) で大きく勝ち、Double 中心の
Mandelbrot や、ノード alloc 中心の TreeSort / Towers は接戦。

**asom-aot vs Truffle (libgraal warm peak)**:

| ベンチ | asom-aot | Truffle | 比 |
|---|---|---|---|
| QuickSort | 0.157 | 0.304 | asom **1.94×** |
| Storage | 0.302 | 0.427 | asom **1.41×** |
| Fannkuch | 0.171 | 0.229 | asom **1.34×** |
| Permute | 0.322 | 0.419 | asom **1.30×** |
| List | 0.353 | 0.447 | asom **1.27×** |
| Sieve | 0.128 | 0.161 | asom **1.26×** |
| Queens | 0.382 | 0.458 | asom **1.20×** |
| Towers | 0.309 | 0.308 | tie |
| Mandelbrot | 0.485 | 0.471 | tie |
| BubbleSort | 0.250 | 0.229 | Truffle 1.09× |
| TreeSort | 0.410 | 0.292 | Truffle **1.40×** |
| Bounce | 0.467 | 0.278 | Truffle **1.68×** |

12 ベンチ中 7 で asom-aot 勝ち、2 で tie、3 で Truffle 勝ち。負けてい
る Bounce / TreeSort は Ball / Tree object の field-resident Double が
boxed のまま発射される構造的問題。`make bench-pg` の warmup-driven
`node_send1_dbl*` 特化で局所的に逆転する。

**`make bench-pg` の伸び**:

| ベンチ | aot | pg | 改善 |
|---|---|---|---|
| Bounce | 0.467 | **0.281** | 1.66× |
| TreeSort | 0.410 | **0.331** | 1.24× |
| Mandelbrot | 0.485 | 0.412 | 1.18× |
| その他 | tie | tie | — |

PG は warmup-driven の `node_send1_dbl*` 型特化が hot path に入るので、
**Ball / Tree の field 経由 Double 演算が多い** Bounce / TreeSort で
顕著に効く。**asom-pg は Truffle に対し 10/12 で勝ち or tie**（負ける
のは TreeSort 0.331 vs 0.292、BubbleSort 0.249 vs 0.229 のみ）。

`make bench-aot` (`-c`) は parser-time で baked された SD chain で全エントリ
が SD-dispatched、整数中心ベンチで圧倒的。Double 演算でも flonum tagging が
中間 box を消すが、mantissa 低 2bit ≠ 00 の double は heap-fallback して
`asom_double` の bump arena に行く（厳密に bit-exact を保つため）。

interp 単独でも SOM++ に勝つベンチが多い（Sieve/Queens で 3× 以上）。
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

各 engine の **起動 / 一度きりコスト** がそのまま乗るため、long-run と
short-run で答えが変わる。例えば Sieve では asom-aot 0.130s vs Truffle
1.580s で `wall − inner` を取ると asom ~2 ms / Truffle ~1419 ms — 後者は
JVM bootstrap + Truffle runtime init + libgraal load + 最初の iter での
Graal JIT compile が固定費。bench loop を長く走らせるほど固定費は希釈
される。

「inner-loop の純粋なスループット」を比較したいときは上の inner-work 表
を、「コマンド一発打って結果が返ってくるまで」を比較したいときはこの
wall-clock 表を見る。どちらの数字も engine が正直に出している、用途で
読み分ける。

```
benchmark    |    interp |       aot |        pg |     SOM++ |   Truffle
------------------------------------------------------------------------
Sieve        |     0.440 |     0.130 |     0.130 |     1.340 |     1.580
Permute      |     0.450 |     0.320 |     0.310 |     0.600 |     1.860
Towers       |     0.390 |     0.310 |     0.300 |     0.340 |     1.750
Queens       |     0.450 |     0.380 |     0.380 |     1.230 |     1.910
List         |     0.420 |     0.350 |     0.340 |     0.420 |     1.910
Storage      |     0.370 |     0.300 |     0.300 |     0.660 |     1.900
Bounce       |     0.450 |     0.470 |     0.280 |     0.910 |     1.730
BubbleSort   |     0.480 |     0.250 |     0.250 |     0.660 |     1.680
QuickSort    |     0.460 |     0.160 |     0.160 |     0.830 |     1.880
TreeSort     |     0.410 |     0.410 |     0.330 |     0.470 |     1.760
Fannkuch     |     0.470 |     0.170 |     0.170 |     0.720 |     1.690
Mandelbrot   |     0.710 |     0.490 |     0.410 |     0.770 |     2.070
```

- **TruffleSOM** は wall-clock で常に **1.5–2.2 秒の JVM bootstrap +
  AST parse + libgraal JIT compile** が固定費として乗る（Sieve でも
  Mandelbrot でも一定）。short-run では割合が大きく、long-run で薄まる。
- **asom / SOM++** は C ネイティブで bare 起動が速いため、wall-clock が
  inner-work とほぼ同じ。
- どの engine も計測自体は正直、bench をどれだけ長く走らせるかで wall
  と inner の比が変わる、というだけ。ここでは inner=1秒級なので Truffle
  には不利な側に振れている。

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
