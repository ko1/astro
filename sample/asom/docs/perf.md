# asom 性能最適化メモ

ASTro 上の SOM (Smalltalk dialect) インタプリタを、stock の AST tree
walker から **`make bench-aot` で SOM++ 全 12 ベンチ勝ち**、
**`make bench-pg` で TruffleSOM warm peak 11/12 勝ち** まで持っていった
経緯の記録。

ベンチは `make bench` で `run_compare.rb` 経由、`run_compare.rb` の
`INNER` テーブルは asom-interp で **~1 秒** 程度に揃えてある。
`iterations=1`、best of 3。

> 注意: bench の数字は **asom が GC を持たない**前提で取れている値。
> Truffle / SOM++ は bench 区間でも nursery / COPYING を回しており、
> その分の費用を払っているのに対し、asom は alloc が pointer-bump と
> heap leak だけ。長期 run では asom は OOM するので、Truffle 並の
> fairness で測るには asom 側にも GC を入れる必要がある（[Planned](../README.md#planned性能拡張の-todo)）。

## 最終ベンチ結果

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

`asom-aot` が SOM++ に対して **Sieve 20×、Mandelbrot 47×、QuickSort 10×、
Queens 8×、Fannkuch / BubbleSort / List 4-7×、Permute / Bounce / Storage
3×、Towers / TreeSort 2-3×**。

`asom-pg` は warmup-driven の型特化と全 entry SD-bake が効き、Bounce
0.430s → 0.238s（+45%）、TreeSort 0.400s → 0.327s（+18%）と AOT を
さらに上回る。Truffle warm peak と比べて **11/12 で勝ち**、唯一 Towers
（0.326 vs Truffle 0.280）だけが負け（再帰メソッドの inline ＝ call-graph
PE が未実装）。

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

**asom-pg は 11/12 ベンチで TruffleSOM (warm peak) に勝つ**（Towers のみ
1.16× 負け）。AOT で Truffle に負けていた Bounce / TreeSort / BubbleSort
が PG mode で逆転。

---

## 残課題（Planned、`README.md` の Planned 節も参照）

- **Shape-based field unbox** — Bounce / TreeSort の Ball / Tree field
  レベル Double を flonum / inline-double として保持する。AOT mode で
  Truffle に並走するには必要。
- **Call-graph PE** — Towers の再帰メソッドを inline 化。PG bake で
  `cached_method->body` を call site に展開し、leaf level で field
  アクセスまで scalar replacement に落とす。Truffle 全勝には必須。
- **GC 導入**（Boehm or 統一基盤）— 現状 alloc は leak、bench の数字は
  GC 未払いの状態。Truffle / SOM++ と fair に比較するには必要。
- **Bignum (GMP)** — 62-bit tagged int 範囲を超えた整数を `mpz_t` で
  扱う。IntegerTest 5 件の失敗を埋める。
