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
Sieve        |     0.458 |     0.140 |     0.135 |     1.451 |       TBD
Permute      |     0.464 |     0.352 |     0.348 |     0.656 |       TBD
Towers       |     0.426 |     0.336 |     0.330 |     0.370 |       TBD
Queens       |     0.459 |     0.383 |     0.375 |     1.272 |       TBD
List         |     0.410 |     0.351 |     0.355 |     0.421 |       TBD
Storage      |     0.360 |     0.324 |     0.334 |     0.760 |       TBD
Bounce       |     0.471 |     0.450 |     0.267 |     0.966 |       TBD
BubbleSort   |     0.506 |     0.271 |     0.270 |     0.692 |       TBD
QuickSort    |     0.460 |     0.150 |     0.154 |     0.814 |       TBD
TreeSort     |     0.440 |     0.427 |     0.341 |     0.467 |       TBD
Fannkuch     |     0.430 |     0.170 |     0.171 |     0.724 |       TBD
Mandelbrot   |     0.706 |     0.473 |     0.453 |     0.826 |       TBD
```

Truffle 列は TBD — `sample/asom/TruffleSOM` を submodule で立ち上げ中
（重い: GraalVM + mx）。SOM-st/SOMpp は `sample/asom/SOMpp` 配下に
submodule で再 build 済み。

#### take-aways

**asom-aot は 12 ベンチすべてで SOM++ より速い**（USE_TAGGING + Release）:

| ベンチ | asom-aot | SOM++ | asom 倍率 |
|---|---|---|---|
| Sieve | 0.140 | 1.451 | **10.4×** |
| QuickSort | 0.150 | 0.814 | 5.4× |
| Fannkuch | 0.170 | 0.724 | 4.3× |
| Queens | 0.383 | 1.272 | 3.3× |
| BubbleSort | 0.271 | 0.692 | 2.6× |
| Storage | 0.324 | 0.760 | 2.3× |
| Bounce | 0.450 | 0.966 | 2.1× |
| Permute | 0.352 | 0.656 | 1.9× |
| Mandelbrot | 0.473 | 0.826 | 1.7× |
| List | 0.351 | 0.421 | 1.2× |
| Towers | 0.336 | 0.370 | 1.10× |
| TreeSort | 0.427 | 0.467 | 1.09× |

整数中心ベンチ (Sieve / QuickSort / Queens) で大きく勝ち、Double 中心の
Mandelbrot や、ノード alloc 中心の TreeSort / Towers は接戦。

**`make bench-pg` の伸び**:

| ベンチ | aot | pg | 改善 |
|---|---|---|---|
| Bounce | 0.450 | **0.267** | 1.69× |
| TreeSort | 0.427 | **0.341** | 1.25× |
| Mandelbrot | 0.473 | 0.453 | 1.04× |
| その他 | tie | tie | — |

PG は warmup-driven の `node_send1_dbl*` 型特化が hot path に入るので、
**Ball / Tree の field 経由 Double 演算が多い** Bounce / TreeSort で
顕著に効く。Mandelbrot は flonum tagging で AOT 段階でほぼ底打ちなので
PG の追加効果は小さい。

`make bench-aot` (`-c`) は parser-time で baked された SD chain で全エントリ
が SD-dispatched、整数中心ベンチで圧倒的。Double 演算でも flonum tagging が
中間 box を消すが、mantissa 低 2bit ≠ 00 の double は heap-fallback して
`asom_double` の bump arena に行く（厳密に bit-exact を保つため）。

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

各 engine の **起動 / 一度きりコスト** がそのまま乗るため、long-run と
short-run で答えが変わる。例えば Sieve では asom-aot 0.140s vs Truffle
1.700s で `wall − inner` を取ると asom ~5 ms / Truffle ~1539 ms — 後者は
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
Sieve        |     0.460 |     0.140 |     0.140 |     1.450 |       TBD
Permute      |     0.460 |     0.350 |     0.350 |     0.660 |       TBD
Towers       |     0.430 |     0.340 |     0.330 |     0.370 |       TBD
Queens       |     0.460 |     0.380 |     0.380 |     1.270 |       TBD
List         |     0.410 |     0.350 |     0.360 |     0.420 |       TBD
Storage      |     0.360 |     0.330 |     0.340 |     0.760 |       TBD
Bounce       |     0.470 |     0.450 |     0.270 |     0.970 |       TBD
BubbleSort   |     0.510 |     0.270 |     0.270 |     0.690 |       TBD
QuickSort    |     0.460 |     0.150 |     0.160 |     0.810 |       TBD
TreeSort     |     0.440 |     0.430 |     0.340 |     0.470 |       TBD
Fannkuch     |     0.430 |     0.170 |     0.170 |     0.720 |       TBD
Mandelbrot   |     0.710 |     0.480 |     0.460 |     0.830 |       TBD
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
