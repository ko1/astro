# asom 性能最適化メモ

ASTro 上の SOM (Smalltalk dialect) インタプリタを、stock の AST tree
walker から **`make bench-aot` で SOM++ 全 12 ベンチ勝ち**まで持っていった
経緯の記録。

ベンチは `make bench` で `run_compare.rb` 経由、`run_compare.rb` の
`INNER` テーブルは asom-interp で **~1 秒** 程度に揃えてある。
`iterations=1`、best of 3。

> 注意: 過去の version 数（特に Mandelbrot AOT 0.029s, SOM++比 47×、
> Truffle 比 15×）は **flonum encoder のバグ**で計算が早期終了して
> garbage 値で抜けていた幻の数字でした。flonum bias 定数の修正
> ([§10](#10--flonum-encoder-bias-定数のバグと-bit-exact-tightening) 参照) の後、
> 正しい数字は Mandelbrot AOT 0.473s / SOM++ 比 1.7× となります。

## 最終ベンチ結果

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

Truffle 列は TBD: TruffleSOM submodule（`sample/asom/TruffleSOM`）の
ビルドが未完了（GraalVM + mx required）。SOM-st/SOMpp は
`sample/asom/SOMpp` 配下に submodule で格納し、build/ 配下に out-of-tree
build した binary を `make bench` が参照する。

`asom-aot` が SOM++ に対して **Sieve 10.4×、QuickSort 5.4×、Fannkuch
4.3×、Queens 3.3×、BubbleSort 2.6×、Storage 2.3×、Bounce 2.1×、Permute
1.9×、Mandelbrot 1.7×、List 1.2×、Towers 1.1×、TreeSort 1.1×**。
全 12 ベンチで勝ち。

`asom-pg` は warmup-driven の型特化と全 entry SD-bake が効き、Bounce
0.450s → 0.267s（1.69×）、TreeSort 0.427s → 0.341s（1.25×）と AOT を
さらに上回る。Mandelbrot は flonum tagging で AOT 段階でほぼ底打ち
なので PG の追加効果は小さい。

---

## 1 — `m->no_nlr` で `setjmp` スキップ（method）

メソッド本体に `node_block` がひとつも無ければ、そのメソッドに対する
non-local return（`^expr` from a nested block）は文法的に発生しえない。
`subtree_creates_block(body)` をパース時に走らせ、結果を
`asom_method->no_nlr` に焼き、`asom_invoke` がフラグを見て `setjmp`
設置を完全にスキップ（unwind 構造体も作らない）。

per-call ~50 ns の節約だが、再帰系の Towers / Queens / List では数百万回
呼ばれるので Towers 1.2×、Queens 1.15×、List 1.28× の追加スピードアップ。

## 2 — `m->no_nlr` を block にも広げる（commit `e6e287c`）

method の `no_nlr` と同じことを block にも適用。block-body 内に
`node_return_nlr` が無ければ `asom_block_invoke` の escape-catcher
`setjmp` を完全にスキップする。

walker は `subtree_has_nlr` を新設：`node_block` 境界で打ち切る（子
ブロックは独自の catcher を持つので関係なし）、inline CF ノード
（`node_iftrue` 等）は `body_block` ではなく `stmts` 子に潜る。
`ALLOC_node_block(...)` の `no_nlr` operand 経由で `asom_make_block` →
`asom_method->no_nlr` に焼き付け、`asom_block_invoke` で `setjmp` 省略。

Mandelbrot プロファイルで `asom_block_invoke` が **18% → 5.5%**、
`__sigsetjmp` が top profile から消失。

## 3 — `and:` / `or:` の inline（commit `de62b74`）

`Boolean>>and:` / `or:` の引数が inlinable な block literal のときは
`node_and` / `node_or` に書き換える。意味的には
`^ self ifTrue: aBlock ifFalse: false`（`or:` は逆）と同じだが、
true / false 受信時に `aBlock value` を呼ぶ代わりに stmts を inline 評価
する。

Mandelbrot 内側ループ `[notDone and: [z < 50]] whileTrue: [...]` で、
`[z < 50]` が外側 while のイテレーションごとに `asom_make_block` され、
`frame->captured = true` を立てて enclosing frame を pool 外に固定して
いた。inline 後は内側ブロックがそもそも具現化されないので frame が
pool を循環し続け、`frame_alloc → __libc_calloc` 経由のメモリ使用が
プロファイルから消える。

**Mandelbrot AOT-cached: 2050ms → 1380ms（1.48×）**。

## 4 — `whileTrue:` / `whileFalse:` の pool 版（commit `cda5739`）

既存の `node_whiletrue` / `whilefalse` はスタック割当の inline frame を
使うため、cond / body のサブツリーが closure を作りうる
（`subtree_creates_block` が真）と inline できず、代わりに
`asom_block_invoke` per iter にフォールバックしていた。inline CF 子
ノード（`node_iftrue` / `node_and` / etc.）は稀な non-Boolean
fallback 用に `body_block` を保持するので、bool 系を含む body は
ほぼ「block 生成あり」と判定されてしまう。

pool 版は inline frame を per-bucket frame pool から heap に確保する。
closure escape があれば `frame->captured = true` で pool 復帰を抑止する
— `node_iftrue_pool` / `node_to_do_pool` と同じ正当性。1 回の `whileTrue:`
呼び出しにつき pool fetch + return が 1 回で、un-inlined 版の per-iter
`asom_block_invoke` ペアを置き換える。

Mandelbrot の 3 重 nested whileTrue: が全て inline 化される（最内
`[notDone and: [z < 50]] whileTrue: [...]` を含む）。

**Mandelbrot AOT-cached: 1380ms → 860ms（1.6×、no_nlr-blocks 修正からの
累積で 2.4×）**。クロスベンチ:

| ベンチ | 前 | 後 | 倍率 |
|---|---|---|---|
| Sieve | 7742 us | 5581 us | 1.39× |
| Bounce | 38881 us | 30191 us | 1.29× |
| **List** | 138606 us | **52524 us** | **2.64×** |
| Permute | 83375 us | 60083 us | 1.39× |
| Storage | 65721 us | 48214 us | 1.36× |
| TreeSort | 93406 us | 66488 us | 1.41× |
| **QuickSort** | 24764 us | **8281 us** | **2.99×** |
| Towers | 186216 us | 146689 us | 1.27× |
| BubbleSort | 15936 us | 11142 us | 1.43× |
| Fannkuch | 223148 us | 112833 us | 1.98× |
| Queens | 49863 us | 36806 us | 1.35× |

## 5 — Flonum tagging（commit `f0bca38`）

VALUE エンコーディングを 1-bit int タグから 2-bit に拡張し、CRuby 流の
biased-exponent 圧縮で **Double を VALUE word 内に即値で持つ**:

```
...x1   -> SmallInteger  (low bit 1, top 63 bits 値)
...10   -> Flonum        (low 2 bits 10, biased double bits)
...00   -> object pointer
```

代表値 (biased exponent ∈ [897, 1150]、abs ≈ 2^-126 〜 2^127) はロスレス
で 62 bit に詰まる。範囲外（denormal、巨大 exponent）は従来通り
`struct asom_double` を bump arena に boxed allocation。

Mandelbrot のような **中間 Double が無数に作られて即捨て** のパターンで
:

- `asom_double_new` の bump 進行 + class field write が消える
- heap-double の pointer-deref 経由のフィールド read が消える
- L1 cache 圧が下がる
- 結果値が register に乗ったまま次の演算に流れる

**性能効果** (5 iters × inner=350、AOT-cached、best-of-3):

| ベンチ | flonum 前 | flonum 後 | 倍率 |
|---|---|---|---|
| Mandelbrot interp | 724 ms | **52 ms** | **14×** |
| **Mandelbrot AOT** | **503 ms** | **33 ms** | **15×** |

Mandelbrot AOT は Truffle (425 ms) を **12.9× 引き離す** に変化。
他のベンチ（Sieve / Bounce / Storage / TreeSort / Towers / QuickSort）は
±5% で同等 — 中間 Double を作らないので flonum の出番がない。Bounce が
変わらなかったことで「Ball field の boxed double はそのまま」と判明、
ここを潰すには **shape-based field unbox** が要る（Planned）。

実装は `context.h` の VALUE マクロ群と `asom_runtime.{h,c}` /
`asom_primitives.c` の限定的な分岐追加だけ。AST / Node 層は無改造。
SD shard の Merkle hash も変わらず、AOT cache は再 bake で互換。

## 6 — `node_send1_dbl*` 型特化（abruby ミラー、commit `8801b08`）

abruby の `node_flonum_plus / minus / mul / div` を asom に持ち込んだ
形で、`node_send1_dbl{plus, minus, times, lt, gt, le, ge, eq}` を 8 個
追加。`node_send1_int*` の枠組みをそのまま流用し、`@canonical=node_send1`
で SD ハッシュを共有、guard miss は generic `node_send1` への
`swap_dispatcher`。

```c
NODE_DEF @canonical=node_send1
node_send1_dblplus(...) {
    VALUE r = EVAL_ARG(c, recv);
    VALUE a = EVAL_ARG(c, arg0);
    if (LIKELY(ASOM_IS_FLO(r) && ASOM_IS_FLO(a))) {
        return asom_double_new(c, asom_val2flo(r) + asom_val2flo(a));
    }
    if (!n->head.flags.is_specialized) swap_dispatcher(n, &kind_node_send1);
    /* ...fallback to generic node_send1 + asom_send... */
}
```

配線:

- `enum asom_prim_kind` に `ASOM_PRIM_DBL_PLUS` 〜 `ASOM_PRIM_DBL_EQ`
- `asom_primitives.c`: `def_prim_kind(c->cls_double, "+", dbl_plus, 1, ASOM_PRIM_DBL_PLUS)` 等
- `asom_send1_specialization`: `PRIM_DBL_*` → `kind_node_send1_dbl*`
- `node_send1` slow path: 両 operand が flonum なら dbl_* に rewrite

| モード | Mandelbrot 前 | Mandelbrot 後 | 備考 |
|---|---|---|---|
| **interp (`--plain`)** | 52 ms | 46-49 ms | type-feedback で初回呼び出しから dbl_* に rewrite |
| **PG (`-p`)** | 45 ms | 28 ms | warmup 中に rewrite → bake で dbl_* が SD として焼かれる（[次節](#7--pg-mode-で-cold-entry-も-aot-bake-する) で修正後の数字） |
| **AOT (`-c`)** | 33 ms | 33 ms（変化なし） | parser-time int_* が baked、SD-baked node の runtime swap は構造的に効かない |

AOT モードで dbl_* の効果を出すには parser-time の syntactic hint
（double リテラルが片方にある等）か、AOT bake 前に warmup pass を
入れる構造変更が要る。**PG mode が型特化 AOT の正規パス** で、
[次節](#7--pg-mode-で-cold-entry-も-aot-bake-する)で PG 自体を直してから
本来の効果が出る。

## 7 — PG mode で cold entry も AOT bake する（commit `7230fdf`）

PG bake (`-p`) は本来 hot entry のみ profile-aware (`PGSD_<Hopt>.c`) ま
たは plain (`SD_<Horg>.c`) で焼くが、`--pg-threshold` 未満の cold entry
は **bake せず interp dispatcher のまま** にしていた。Mandelbrot の
`Bench Mandelbrot 5 350` warmup で hot 判定された entry は 5 個、残り 135
は cold で interp。SD chain が cold 境界で途切れるたびに interp 経由に落ち、
**PG が AOT より 1.6× 遅かった**（45ms vs 28ms）。

修正: cold entry も `cs_compile_one(body, label, NULL)` で plain AOT SD と
して焼く。これで全 entry が SD-dispatched になり、SD chain が連続。hot は
従来通り `PGSD_<Hopt>.c` で profile-aware bake。

効果（`make bench`、ITERS=1、best of 3）:

| ベンチ | AOT (`-c`) | PG (`-p`) 修正前 | PG (`-p`) 修正後 |
|---|---|---|---|
| Mandelbrot | 0.028 | 0.045 | **0.029** |
| **Bounce** | 0.430 | 0.281 | **0.238** |
| **TreeSort** | 0.400 | 0.385 | **0.327** |
| BubbleSort | 0.234 | 0.490 | **0.232** |

特に **Bounce** で AOT 比 +45% 改善（0.430s → 0.238s）。理由は:

1. Ball オブジェクトの field アクセスが多く、parser-time の `node_send1`
   は Double operand を見ても `node_send1_intplus` に decay
   （AOT bake で int_* が SD として固定）
2. PG warmup で `node_send1_dblplus` 等に swap → 2 回目 run で hot path が
   flonum fast path、`asom_double_new` も alloc-free
3. 全 cold entry も SD-baked なので chain 連続、hybrid dispatch が消える

PG mode は AOT で Truffle に負けていた Bounce / TreeSort などで逆転を
生む（field 経由 Double 演算が多く、warmup で `node_send1_dbl*` 特化が
入る効果）。

## 8 — Boehm GC（commit `ebc7201`）

それまで `asom_object` 系は alloc 後 free しないリークモデル。bench は
inner ~1 秒で alloc 量が bounded なので走り切るが、Truffle / SOM++ は
同区間で nursery / COPYING を回しているのに対し asom は alloc が pure
pointer-bump（GC コストゼロ）で、bench 倍率に GC 未払い分の下駄が乗って
いた。Boehm-Demers-Weiser conservative GC を導入してフェアな比較に。

実装は最小:

- `context.h` の末尾で `malloc` / `calloc` / `realloc` / `strdup` /
  `free` をマクロで `GC_MALLOC` / `GC_REALLOC` / `GC_STRDUP` /
  `((void)(p))` に wrap。既存呼び出し点はソース無変更。
- `main()` 冒頭に `GC_INIT()`。
- `Makefile` に `-lgc`。
- `__thread` 修飾子を `g_double_arena` / `g_block_arena` /
  `g_frame_pool[]` / `g_unwind_top` から削除。Boehm のデフォルトは TLS
  をスキャンしないので、TLS に置いた global pointer 経由で reachable な
  オブジェクトが collect されてしまうため。asom は single-threaded なの
  で `__thread` は元々不要だった。

驚いた点: bench 数字は変わらないか、むしろ若干速くなった（`Sieve aot
0.132 → 0.123`、`Storage aot 0.384 → 0.289`）。inner ~1 秒の bench window
では major collection が走らないので、`GC_MALLOC` は TLAB-like の bump
pointer + `free` 不要、で `calloc`+ 明示 free よりも軽いケースがある。

これで「GC 未払い」caveat が消え、Truffle / SOM++ と同条件の比較に。
long-running も leak しなくなった。

## 9 — GMP-backed LargeInteger / Bignum（commit `1510df2` + `e3cd3de`）

それまで Integer は 62-bit tagged immediate のみ、overflow 時に silent
wrap。SOM 仕様は任意精度（SmallInteger / LargeInteger 透過 hierarchy）を
要求し、IntegerTest が `9223372036854775807` や `2 raisedTo: 100` 等の
大きな値で 9 件失敗していた。

GMP を bignum バックエンドに採用:

- `struct asom_bignum { hdr; mpz_t value }` を `cls_bignum` として
  cls_integer の subclass に登録 (= "LargeInteger")
- `asom_int_norm(c, mpz)` で 62-bit に収まれば `ASOM_INT2VAL`、超えれば
  bignum 化（運用上の "constructor"）
- `INT_BIN_PRIM` を `__builtin_*_overflow` 検出付きに改修。overflow パス
  で mpz arithmetic + `asom_int_norm` 経由 → 透過 promotion
- Comparator も bignum 対応（`mpz_cmp`）
- big_* primitive 一式（`+`, `-`, `*`, `<`, `>`, `=`, `negated`, `abs`,
  `asString`, `print`, `println`, `asDouble`, `asInteger`）
- Parser: 大きな literal を `node_bignum_lit(bytes, cached)` で parse-time
  に mpz 化、cached VALUE はハッシュから除外（既存 `node_string_val` の
  パターンを再利用）
- `Integer fromString:` も `mpz_set_str` 経由で任意精度に

`raisedTo:` は SOM stdlib (`Integer.som`) の `output := output * self`
ベース実装で、`int_*` primitive の overflow promotion で自然に bignum 化
する。primitive 登録は撤去（stdlib 任せ）。

結果: **IntegerTest 25/25 通過**（修正前 16/25）。`2 raisedTo: 100 =
1267650600228229401496703205376` も正確に計算。

## 10 — Flonum encoder bias 定数のバグと bit-exact tightening（commit `f669d70`）

§5 で flonum tagging を入れた時、CRuby 流の bias 定数を `(uint64_t)0x3C
<< 60` と書いていた。これは実は `0xC000000000000000`（60-bit 左シフトで
0x3C の低 4bit `0xC` だけ残る）になっていて、`-2.0`（bit パターン
`0xC000000000000000`）が `bits - 0xC000... = 0` → 0.0 special encoding
（VALUE = 2）と衝突。`2.0`（`0x4000000000000000`）も `0x4 - 0xC = -8`
→ `0x8000000000000000` という不正な値になっていた。

Mandelbrot の inner loop で `(zrzr + zizi > 4.0)` の値が壊れるため、
`escape := 1.` が誤って早期発火し loop が短く終わって**速く見えていた**
だけだった（result も 50 ではなく 168）。

修正:

1. bias を **literal** `0x3C00000000000000ULL` に
2. `union` type-punning を `__builtin_memcpy` に置き換え（strict-aliasing 対策）
3. encoder のレンジチェックを bit-exact にするため **mantissa 低 2bit が
   00** の double のみ flonum 化、それ以外は heap-boxed `asom_double` に
   フォールバック。OR で書き込む tag bit が原 mantissa を上書きする問題を
   完全回避（lossless round-trip）

結果:

- Mandelbrot result が正しく **50** を返す（inner=750 の場合）
- IntegerTest 25/25
- DoubleTest 27/27（修正前 24/27 — 同じバグだった）
- bench 数字: Mandelbrot AOT は実値 0.473s（虚偽の 0.029s から退行に
  見えるが、0.029s が異常値だったので「正しい数字は実は 1/2 程度」が
  正しい解釈）

§5 の Mandelbrot 倍率 (15× / 14×) は **revoked**。実値は 1.7× vs
SOM++、Truffle 比は再 bench 待ち。

---

## 残課題（Planned、`README.md` の Planned 節も参照）

- **Shape-based field unbox** — Bounce / TreeSort の Ball / Tree field
  レベル Double を flonum / inline-double として保持する。AOT mode で
  さらに伸ばすために必要。
- **Call-graph PE** — Towers の再帰メソッドを inline 化。PG bake で
  `cached_method->body` を call site に展開し、leaf level で field
  アクセスまで scalar replacement に落とす。
- **TruffleSOM submodule の build** — `sample/asom/TruffleSOM` で参照
  バイナリを揃えて Truffle 列の比較を復活させる（GraalVM + mx required）。
- **AreWeFastYet 残り**（Havlak / CD / Knapsack / PageRank）の追加。
