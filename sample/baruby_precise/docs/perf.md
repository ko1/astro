# baruby_precise 性能ノート

仕様は [spec.md](spec.md)、実装は [runtime.md](runtime.md)、
未対応・残タスクは [todo.md](todo.md) を参照。

baruby_precise は **precise *moving* GC (semi-space) の testbed** で、
姉妹サンプル `sample/baruby` (conservative libgc) と同じテスト・ベンチ
スクリプトで動かして「precise rooting + 移動 GC のオーバーヘッドは
どれくらいか」を測ることを目的にしている。 設計の経緯は
[`docs/gc_design.md`](../../../docs/gc_design.md) を参照。

## 0. Fairness contract (iter 35)

iter 35 で user から fairness 観点の指摘を 7 件受け、 比較契約を以下に固定:

- **Build**: `make GC=<backend> ASTRO_DEBUG=0`  全 backend に同じフラグ
  (`-O3 -flto=auto -fno-plt -march=native`)。 `ASTRO_DEBUG=0` は perf
  build 用の release shape。 dev は明示的に `ASTRO_DEBUG=1` で opt-in。
- **Mode**: `--plain` 固定。 AOT (`-c`) は code_store ビルド経路が壊れて
  いて再検証中 ([todo.md](todo.md) 参照)。
- **Repeats / policy**: 各 (backend × bench) を `median of N=3`。
  iter 34 までの best-of-3 は (ノイズで運の影響大)、 iter 35 から median。
- **Charging model**: 全 gen backend で **alloc-bytes** で trigger を計測
  (`sizeof(GCHeader) + ALIGN8(payload_size)`)。 payload bytes と nursery
  occupancy bytes が混在していたのを統一。
- **Trigger threshold**: 全 gen backend の minor を統一 16 MiB
  (`mark_gen` / `mark_gen_inc` の旧 4 MiB から修正)。 全 gen backend の
  major adaptive threshold MIN を統一 16 MiB。 全 gen backend の major は
  **old growth** で発火 (`old_alloc_since_major > major_threshold`)。
  immix_gen の「all alloc で major fire」 バグも修正。
- **Header sizing**: iter 31 で全 backend GCHeader 8 B or 16 B に packing。
  iter 33 で mark_gen / mark_gen_inc の 16 → 8 B 化 (young_next →
  external array)。 全 backend BaArray (24 B payload + 8 B header) が
  slab class 32 に収まり、 cache footprint が揃った。
- **Backends excluded**: `copy_gen_inc` は実体が `copy_gen` の clone
  (inc_step / SATB なし)。 公平な比較として「独立 algorithm」 を主張
  できないため matrix runner / table から **除外**。 ファイルは
  `gc_copy_gen_inc.c` 冒頭に明記 (iter 35 honesty note)。
- **GC timer**: 全 backend で gc_collect_internal / minor_gc / major_gc
  に加え、 `mark_gen_inc` の `inc_step` も `aro_gc_time_begin/_end` の中。
  `mark_seconds` / `reclaim_seconds` の phase 分割は backend 個別。
- **Runner**: `bench/matrix.rb` が canonical。 backend ごと rebuild、
  `strings` で `baruby_gc=<name>` stamp 検証、 result を `oracle.json`
  に対して checksum、 CSV + JSON + Markdown で出力。 過去の Makefile bug
  (iter 32) の再発を防ぐ。

## 1. 計測環境

| 項目 | 値 |
|---|---|
| CPU | AMD Ryzen 9 5900HX |
| OS | Linux 6.8 (x86_64) |
| Compiler | gcc 13.3.0 |
| GC (precise) | 自前 13 backend、 `gc_<name>.c` |
| GC (conservative 比較対象) | Boehm libgc 8.2.6 (`sample/baruby` 由来) |
| Build flags | `-O3 -flto=auto -ggdb3 -march=native -fno-plt -DASTRO_DEBUG=0` |
| GC backend  | `make GC=<name>` で選択。 default = `copy` (semispace Cheney) |
| Run policy  | `ruby bench/matrix.rb` — median of 3, plain mode |

**比較対象**: `sample/baruby/` (libgc 経由の conservative scanning) を
baseline にする。 iter 35 で baruby 側にも `-flto=auto` を追加して build
flags を揃えた。 ベンチスクリプト (`bench/*.ba.rb`) は両者で共通 — baruby
を copy したのでファイル単位で同一。 binary 名のみ異なる
(`./baruby` vs `./baruby_precise`)。 plain mode = AST インタプリタ
(code_store なし)。 AOT mode は moving GC 移行後に未再検証 + 別件で broken。

⚠ **「libgc との比較」 caveat**: いま測っているのは「collector のみの
差」 ではなく **「runtime + rooting + collector の合計差」**。 baruby_precise
は precise rooting (sp_top scan) と moving GC の組合せ、 baruby は
conservative scanning。 同じ言語 / 同じベンチ / 同じ build flags にした
ので **環境としては公平**だが、 数値の差を「GC algorithm の差」 と読み
切るのは過剰解釈。 collector-only 比較が欲しいなら同じ runtime に
backend を差し込む必要がある (現状は別バイナリ)。

## 2. 全 GC backend のベンチ実測 (plain mode, fairness contract 適用後)

iter 36 で再計測した median-of-3 (`ruby bench/matrix.rb`)。 14 backend
× 16 bench + libgc column。 `copy_gen_inc` は実体が copy_gen の clone
なので除外。 iter 36 追加 backend: **`mark_card_gen`** (page-level
remset、 §0 fairness contract で言及した bounded remset 設計)。 iter 36
追加 bench: **`ast_eval`** (AST builder + evaluator)、 **`remset_pressure`**
(50K cell chain + 200K sparse updates、 adversarial old→young write)。

| Bench | none | mark | mark_gen | mark_gen_inc | copy | copy_gen | mark_compact | mark_compact_gen | bump | mark_bump_gen | immix | immix_gen | mark_bitmap_gen | mark_card_gen | libgc |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| ast_eval | 0.38 | 0.38 | 0.39 | 0.37 | 0.36 | 0.36 | 0.37 | 0.36 | 0.37 | 0.36 | **0.35** | 0.36 | 0.37 | 0.36 | 0.38 |
| binary_trees | 0.64 | 1.07 | 1.18 | 1.19 | 0.79 | 1.02 | 0.81 | 0.97 | **0.51** | 0.88 | 0.82 | 0.97 | 1.46 | 1.47 | 0.88 |
| cons_list | 1.29 | 0.95 | 0.97 | 1.01 | 0.78 | 0.78 | 0.94 | 0.82 | 0.98 | 0.80 | 0.76 | **0.74** | 1.01 | 1.04 | 0.98 |
| fannkuch | 0.79 | 0.75 | 0.73 | 0.76 | 0.73 | **0.70** | 0.72 | 0.72 | 0.76 | 0.72 | 0.71 | 0.72 | 0.79 | 0.80 | 0.70 |
| fib_pair | 1.61 | 1.07 | 1.09 | 1.14 | 0.87 | 0.90 | 1.05 | 0.88 | 1.16 | 0.88 | 0.87 | **0.87** | 1.14 | 1.14 | 1.10 |
| gc_combined | 1.45 | 1.06 | 1.08 | 1.16 | **0.86** | 0.91 | 1.07 | 0.99 | 1.12 | 0.93 | 0.94 | 0.86 | 1.17 | 1.22 | 1.12 |
| hash_chain | 1.60 | 1.65 | 1.51 | 1.51 | 1.90 | 1.27 | 1.70 | **1.23** | 1.47 | 1.24 | 1.48 | 1.38 | 1.45 | 1.46 | 1.72 |
| interp_calc | 1.41 | 1.06 | 1.10 | 1.16 | 0.95 | 0.95 | 1.05 | 0.95 | 1.11 | 1.00 | **0.94** | 0.94 | 1.15 | 1.17 | 1.14 |
| life | 1.35 | 1.44 | 1.32 | 1.36 | 1.36 | 1.33 | 1.36 | **1.25** | 1.30 | 1.33 | 1.29 | 1.28 | 1.34 | 1.44 | — |
| list_alloc | 1.36 | 1.03 | 1.04 | 1.08 | **0.82** | 0.89 | 1.00 | 0.93 | 1.11 | 0.87 | 0.92 | 0.82 | 1.12 | 1.11 | 1.04 |
| list_sort | 1.24 | 1.25 | 1.23 | 1.26 | 1.03 | 1.05 | 1.10 | 1.05 | 1.15 | 1.07 | 1.07 | **1.03** | 1.30 | 1.30 | 1.08 |
| nqueens | 1.00 | 0.98 | 0.98 | 0.99 | 0.99 | 0.96 | 0.94 | 0.94 | 0.96 | 0.95 | 0.96 | **0.93** | 1.00 | 1.00 | 0.96 |
| remset_pressure | 0.49 | 0.39 | 0.38 | 0.38 | 0.38 | **0.30** | 0.39 | 0.31 | 0.38 | 0.31 | 0.34 | 0.31 | 0.39 | 0.39 | 0.44 |
| sieve | 1.35 | 1.46 | 1.39 | 1.39 | 1.73 | 1.36 | 1.58 | 1.40 | 1.53 | 1.45 | 1.55 | 1.34 | 1.44 | 1.47 | **1.34** |
| string_concat | 1.64 | 0.76 | 0.81 | 0.89 | 0.49 | 0.54 | 0.85 | 0.55 | 0.84 | 0.53 | 0.54 | **0.48** | 0.78 | 0.79 | 0.95 |
| substr_churn | 1.72 | 1.07 | 1.09 | 1.18 | 0.94 | 0.90 | 1.14 | 0.91 | 1.11 | 0.89 | 0.96 | **0.88** | 1.31 | 1.30 | 1.28 |

**勝者分布** (iter 36 fair contract、 median-of-3):
- `immix_gen` — **6** (cons_list / fib_pair / interp_calc / list_sort /
  nqueens / string_concat / substr_churn) — line allocator + gen で広く
  バランス良し
- `bump` — **1** (binary_trees) — pure alloc-only floor
- `copy` — **1** (gc_combined)
- `copy_gen` — **2** (fannkuch / remset_pressure)
- `immix` — **1** (ast_eval)
- `list_alloc` 最速 `copy` (0.82) ↔ `immix_gen` (0.82) tied
- `mark_compact_gen` — **2** (hash_chain / life)
- `libgc` — **1** (sieve、 sieve は GC が走らない mutator-bound bench)

注目点 (iter 36 新発見):
- **`mark_card_gen` ≈ `mark_bitmap_gen`**: 同 layout で remset entry が
  object→page なだけの差。 多くの bench で ±2% 以内。 fundamental win は
  「remset 上限が page count に bounded」 (= 安全性) で raw 速度の差ではない。
- **`hash_chain` で `copy_gen` / `mark_compact_gen` / `mark_bump_gen` の
  gen 系が圧勝** (1.23-1.27 vs `mark` 1.65)。 hash bucket sparse update が
  WB を活用できるため。 mark_card_gen (1.46) は若干劣る — page-level
  remset の inner-walk overhead が顕在化。
- **`remset_pressure` で `copy_gen` が最速** (0.30s)。 Cheney semispace は
  promotion 時にすべて再走査するため remset cost を回避 (代わりに minor
  自体が重い — でも 50K cell 程度では win)。
- **`binary_trees` で `mark_bitmap_gen` / `mark_card_gen` の per-page
  bitmap 系が最遅** (1.46-1.47)。 4M Array 全部に mark を 立てる作業で
  `locate()` の overhead がかさむ。
- **`bump` が binary_trees で他に倍速** (0.51 vs `copy` 0.79) — GC ゼロ。

iter 31-34 までの「best-of-3」 結果と iter 35 fairness contract 後の数値は
threshold 統一 / charging 統一 / inc_step timer 入れたことで意味的に
不連続なので、 過去 iter 比較表は per-iter done.md で保持し、 perf.md は
fair 数値だけを正本にする。

### 観察 (fair contract 下):

- **`bump` が binary_trees で圧勝**: 4M Array 全部生きてる workload で
  GC 不要。 他は皆 GC overhead を払う。
- **`copy` 系 / `mark_compact_gen` がバランス良し**: 多くの bench で
  上位。 Cheney と compact が ASTro の「ほぼ全部 short-lived」 と
  「ほぼ全部 long-lived」 の両極でそれぞれ強い。
- **`mark_gen` / `mark_gen_inc` は binary_trees で苦戦**: 1.18-1.23 vs
  mark の 1.00。 promotion をしているが long-lived workload では非
  moving の page locality が compact 系より劣る。 hash_chain では逆に
  1.45-1.50 で `mark` の 1.62 より速い (適度に死ぬ workload で gen が
  効く)。
- **`mark_bitmap_gen` は依然底辺寄り**: binary_trees 1.48, substr_churn 1.25
  と worst-of-13。 per-page bitmap の locate() が per-mark で重い。 iter 31
  で flags packing 後は density 差で他に並べた、 と思っていたが iter 34
  adaptive threshold + iter 35 charging 統一を入れたら再び劣勢が露呈。
  bitmap GC は SIMD 向けの bulk 操作で勝つ設計のため、 散発 mark の
  ASTro 用途では構造的に不利。
- **`mark_compact` / `mark_compact_gen` は long-lived で勝つ**: heap が
  compacted 状態のまま増えるので cache locality が高い。
- **`hash_chain` で軒並み苦戦** (1.24-1.86): bucket chain 走査が pointer-
  chasing で cache-cold。 mark_compact_gen が **1.24** で最速。
- **`life` で `mark_bump_gen` が勝つ** (1.21): mutator-bound だが alloc
  pattern が「bump 連発 + 周期的 GC」 で nursery bump+ tenured mark sweep の
  両方が刺さる。


**勝者分布** (2026-05-18 (31) refresh、 GCHeader flags-packing 後の正しい数値):

- **`bump`** が **2 bench で最速** (binary_trees / string_concat tied):
  alloc-heavy & 短寿命の GC-less floor。
- **`copy`** が **4 bench で最速** (cons_list / fib_pair / gc_combined / interp_calc):
  Cheney semispace の安定した強さ
- **`copy_gen_inc`** が **1 bench で最速** (hash_chain) — incremental Cheney が
  large-buckets pointer chase で勝つ
- **`immix_gen`** が **1 bench で最速** (list_alloc) と **1 bench tied** (string_concat)
- **`mark_compact_gen`** が **1 bench tied** (string_concat tied with immix_gen と mark_bump_gen)
- **`mark_bump_gen`** が **2 bench で最速** (list_sort / substr_churn / string_concat tied)
- **`none`** が **4 bench で最速** (fannkuch / life / nqueens / sieve) — mutator-bound
  bench で GC overhead が最大の損失

note: hash_chain は `mark_gen` / `mark_gen_inc` が 2.0+ で大きく劣勢。
それ以外の backend は 1.07-1.41 帯。 packing で `mark` 単独は 2.13 → 1.24
(-42%) で大幅改善したが、 gen 系 (mark_gen / mark_gen_inc) は young_next
linked list 走査が依然 cache-cold で改善が小さい。

iter 31 packing の効果 (主要 bench、 iter 30 → iter 31 正値):
- `mark` hash_chain: 2.13 → **1.24** (-42%) — header 縮小で cache footprint 圧縮
- `mark` binary_trees: 1.07 → **0.89** (-17%)
- `mark` string_concat: 0.86 → **0.70** (-19%)
- `mark` substr_churn: 1.20 → **0.97** (-19%)
- `copy` binary_trees: 0.86 → **0.75** (-13%)
- `copy` fib_pair: 0.87 → **0.72** (-17%)
- `mark_bump_gen` string_concat: 0.53 → **0.41** (-23%)
- `mark_bump_gen` substr_churn: 0.92 → **0.78** (-15%)
- `bump` binary_trees: 0.57 → **0.45** (-21%) — Makefile bug 修正前は
  iter 29 数値 0.55 もそもそも別 backend の数値だった可能性

引っかかった改善点 (2026-05-17 unified realloc_payload, commit e5b237f):
- `mark` の string_concat 2.41 → 1.68 s (-30%)、 substr_churn 1.53 →
  1.44 s。 buf 中間撤廃で realloc あたり malloc/free を 1 ペア節約。
- `bump` の hash_chain 1.50 → 1.11 s (-26%)。 同上。

引っかかった改善点 (2026-05-17 (18)(20)、 slab/page allocator、
commits bc61b22 / 6a8b10f):
- `mark` 全 11 bench で per-object malloc → slab/page。 string_concat
  1.68 → **0.68 s (-60%)**、 fib_pair 1.46 → 0.93 s (-36%)、 cons_list
  1.20 → 0.87 s (-28%)、 list_alloc 1.15 → 0.98 s (-15%)、 substr_churn
  1.44 → 1.15 s (-20%)、 interp_calc 1.31 → 1.02 s (-22%)、 binary_trees
  0.96 → 0.88 s (-8%)。 ただし hash_chain は変動 (旧表 2.20 / 新表 2.48)
  だが計測ノイズと判明 (A/B で同等)。
- `mark_gen` 同様: string_concat 1.47 → **0.78 s (-47%)**、 fib_pair
  1.43 → 1.06 s (-26%)、 binary_trees 1.28 → 1.11 s (-13%)。
- `mark_gen_inc` 同様: string_concat 1.51 → **0.83 s (-47%)**、
  fib_pair 1.47 → 1.12 s (-35%)、 hash_chain 2.29 → 1.73 s (-24%)。
  実装中に「inc_marking 中の新 alloc が stack WB 不在で漏れる」 古典的な
  バグ (binary_trees で 4194301 vs 正解 4194303) を発見・修正
  (finish_sweep で root 再走査追加)。

`copy_gen` と `copy_gen_inc` は ABI 同一だが、 inc 側は SATB flag check
(現状は STW fallback パスのみ) の最適化ヒントで bench 依存に
3-10% 違う。

**`mark_bump_gen` 分析** (2026-05-16 (13) 追加 → (15) tenured bump 化
→ (16) 線形リスト撤廃 + region 走査 sweep):

| Bench | mark\_gen | mark\_bump\_gen | bump nursery 効果 |
|---|---:|---:|---|
| string_concat | 1.47 | **0.60** | -59% |
| fib_pair | 1.43 | **0.99** | -31% |
| list_alloc | 1.19 | **0.97** | -18% |
| substr_churn | 1.46 | **0.99** | -32% |
| binary_trees | 1.28 | **0.92** | -28% (v1 の 1.41 から (16) で 0.92 まで) |

short-lived workload では bump nursery + 「ほぼ全部 nursery で死ぬ」 の
組合せが大勝。 long-lived (binary_trees) は v3 で大幅改善: tenured が
bump 化 + sweep が region 走査 (header-size-prefix の sequential scan)
になり、 linked list pointer chasing の cache miss を除去。 GCHeader も
40 → 24 bytes に縮小。

`mark_compact_gen` との比較で残る差 (binary_trees で 0.79 vs 0.92) は
「compaction するか」 のみ — mark_compact_gen は major で slide compact
して領域を再利用、 mark_bump_gen は累積するだけ (1 GiB で OOM)。
compaction による cache locality 改善 + region 再利用が ~15% の差を
生んでいる。

**2026-05-16 (10) 改善**: `mark` の binary_trees が 7.54 s → **0.97 s
(7.8×)** に劇的改善。 原因は major threshold を fixed 4 MiB → 適応的
(`max(MIN, 2 × live_bytes_post_sweep)`) に変更。 binary_trees の 200 MiB
live heap では旧 threshold が 50 回 GC を発火していたが、 適応版は 4 回
で済む (各 sweep が O(heap) なのでこの削減が直接効く)。 同じ修正を
`mark_gen` / `mark_gen_inc` の major threshold (64 MiB → 適応的) にも
適用、 binary_trees で 10-13% 改善。 short-lived workload では heap が
MIN を超えないので動作は不変。

**2026-05-16 (12) 改善**: parser バグ修正により `n = n + foo(a, b, c, d, e)`
形 (binop の RHS に >3-arg call) が壊れず動くようになった
([done.md](done.md) (12) 参照)。 `bench/life.ba.rb` で 8 個の隣接セル
取得を inline 化できるようになり 1.54 s → 1.30 s に縮んだ (workaround
撤去の bonus)。

- **`none`** は GC を全く行わない (= leak)。 sp[] rooting / WB / alloc API
  間接化のオーバーヘッド単体が見える baseline
- **`mark`** は per-object malloc + linked list 走査の sweep。 オブジェクト数
  に比例して binary_trees で爆死 (7.2s)
- **`mark_gen` / `mark_gen_inc`** は nursery / tenured 分離 + 明示
  remembered set (dirty list)。 過去版の lazy dirty scan (O(|old|)) を
  解消して binary_trees / interp_calc が ~30〜50% 改善
- **`copy`** (semispace Cheney) は default。 small heap でも binary_trees
  でも安定して速い
- **`copy_gen`** は string-heavy で大勝 (string_concat 0.57s = libgc の
  0.60×)。 短命 string の churn が nursery で完結。 binary_trees も
  remset 導入で 0.79s に
- **`copy_gen_inc`** は infra のみ用意 (incremental marking の SATB
  barrier + gray queue)。 stack-WB が無いため STW で運用
- **`mark_compact`** は単一 region bump alloc + Lisp-2 sliding compactor。
  per-object malloc を回避しつつ非 moving (compact 時のみ移動)。
  binary_trees で mark の 7.06s → 0.61s (12×) — region 化の威力
- **`mark_compact_gen`** は nursery (copy) + tenured (mark+compact) の hybrid。
  short-lived alloc は nursery で完結、 long-lived のみ tenured へ promote。
  tenured は single region (vs copy_gen の 2×) で in-place compact。
  **8 bench 中 6 つで全 backend 最速** (libgc 含む全体トップ)
- **`bump`** は bump alloc only (no GC, leak)。 baseline floor として、
  「rooting + WB + dispatch + alloc」の最小コストを示す。 binary_trees が
  0.53s — copy より速い (GC 自体が無いので)。 OOM 時 abort

### ベンチカタログ (全 14 種)

各ベンチの「何を / どう alloc して / lifetime はどんな形か」 を一覧。
GC 評価の観点で workload 分類を意識して揃えている。

#### `binary_trees.ba.rb` — 構造的 long-lived tree

- **What**: Computer Language Benchmarks Game 風の binary tree。
  `make_tree(depth)` で `[left, right]` の 2-要素 BaArray を再帰生成、
  `check_tree` で全 node を walk して合計。 depth=21 で root を 1 つ、
  depth=22 (stretch tree) も作る。 計 ~ 2M nodes / 224 MB alloc。
- **Alloc pattern**: 各 node は 2-要素 BaArray (40 B 含 header) ×
  2M ≈ pure short-burst alloc。 木を作り終わるまでは全 node が live。
- **Lifetime**: 全 node が tree 構築中はずっと live → long-lived。
  walk 後 root を解放すると一気に die。
- **テスト対象**: long-lived heap、 mark/sweep 系の sweep walk コスト、
  Cheney コピーコスト、 compact の領域再利用効果。
- **特性的な数値**: ベスト 0.52 s (`copy` / `bump`)、 ワースト 1.41 s
  (`mark_bump_gen` v1 で 1.41 → v3 で 0.92 まで改善)。 全 backend が
  major を 2 回程度走らせる。

#### `cons_list.ba.rb` — deep linked-list chain

- **What**: 5000 セルの cons-list を build & walk × 2000 回。
  各セル = `[value, next-cell]` (2-要素 BaArray)。 sentinel 0 で終端。
- **Alloc pattern**: 1 iter で 5000 cells (~120 KB) alloc → walk →
  全部 die。 計 ~534 MB alloc 全体。
- **Lifetime**: 1 iter 内では全 cell が live、 iter 完了で一斉に die。
  「深い alloc chain → walk → discard」 の典型。
- **テスト対象**: nursery vs mark+sweep。 iterative walk なので C stack
  は浅いまま、 long chain だけ作れる (binary_trees の代替)。
- **特性的な数値**: ベスト 0.77 s (`copy_gen`)、 多くの backend で
  0.9-1.1 s。

#### `fannkuch.ba.rb` — CLBG fannkuch-redux (mutator-bound macro)

- **What**: Computer Language Benchmarks Game の fannkuch-redux。 1..N の
  順列を全列挙し、 各順列で「prefix `[0, p[0]-1]` を反転」 を `p[0]==1` まで
  繰り返した時のフリップ数の最大値を求める。 N=9 ⇒ 362880 順列、 max=30。
- **Alloc pattern**: 順列ごとに作業用コピー `w[]` (~10 要素 BaArray)
  を alloc。 ホットループは反転 + 配列回転で integer-heavy。 全体 ~362k
  小 array alloc ≈ 14 MB。
- **Lifetime**: `w[]` は数十〜数百 mutator op で死ぬ短命。 enumeration
  state (`p[]`, `count[]`) のみ long-lived。
- **テスト対象**: nursery alloc / promotion 高速性。 ただし alloc/CPU 比率は
  低く、 GC 戦略の差は小さい (全 12 構成で 0.66-0.76 s、 差 15%)。 macro
  だが mutator-bound なので CLBG 上の baseline 的位置付け。
- **特性的な数値**: ベスト 0.66 s (`mark_bump_gen`)、 ワースト 0.76 s
  (`mark_compact`)。 libgc は 0.71 s。

#### `fib_pair.ba.rb` — 再帰 frame escape

- **What**: 再帰 fib variant で各 call が `[a+c, b+d]` の 2-要素
  pair を return。 depth=28、 ループ 13 回で ~ 4.1M pair allocs。
- **Alloc pattern**: 各 call が 1 pair alloc、 parent return とともに die。
  pure short-lived + 深い C stack。
- **Lifetime**: 全 alloc は frame escape — call return で die。
- **テスト対象**: nursery 完結率 (LIFO 形 alloc)。 generational 系が
  大勝するパターン。
- **特性的な数値**: ベスト 0.88 s (`copy_gen`)、 ワースト 1.73 s
  (`mark_gen_inc`)。

#### `gc_combined.ba.rb` — long-lived + 短命の steady-state

- **What**: 50k-要素の long-lived array を維持しつつ、 内ループで
  1M 個の short-lived 4-要素 array を churn。 long array へは
  index アクセスのみ (mutation なし)。
- **Alloc pattern**: 535 MB short-lived + 持続 200 KB long-lived。
- **Lifetime**: 2 層 — permanent long-array + 1-iter で die する short。
- **テスト対象**: 「permanent dataset + hot alloc path」 の現実形。
  generational benefit が出るが、 long-lived の存在で minor scan が
  remset を踏む。
- **特性的な数値**: ベスト 0.94 s (`copy_gen`)。

#### `hash_chain.ba.rb` — chained bucket hash table (3 層 lifetime)

- **What**: 2048 buckets の chained hash table を Array on Array で
  実装。 150k keys を 3 rounds 挿入 (重複 key は値更新) → 全 key lookup。
- **Alloc pattern**: 各 insert で `[k, v]` pair alloc、 chain.items が
  push で grow。 全 12 MB alloc (long-lived ~10 MB)。
- **Lifetime**: 3 層 — bucket array (long-lived) / chain arrays
  (medium-lived) / `[k, v]` pairs (short-lived)。
- **テスト対象**: WB heavy (chain.push が long-lived bucket 経由で
  short-lived pair を参照)、 remset の old→young 仲介、 chain.items の
  realloc-payload の正しさ (本 bench が realloc の latent stale-ptr バグを
  発掘した — done.md (8))。
- **特性的な数値**: ベスト 1.11 s (`bump`)、 ワースト 2.20 s (`mark`)。
  mark family が遅いのは per-object malloc で heap が断片化 → cache
  miss 多発のため。

#### `interp_calc.ba.rb` — AST 構築 → 評価のミニインタプリタ

- **What**: depth=12 の balanced AST (kind/lhs/rhs の 3-要素 BaArray) を
  構築 → 再帰評価 → 合計。 1000 回反復。
- **Alloc pattern**: 構築 phase で O(2^12) = 4096 sub-array burst、
  評価 phase で alloc なし。
- **Lifetime**: 1 iter 内で構築 → 評価 → 全体 die。
- **テスト対象**: alloc burst → 静止状態 の generational benefit。
- **特性的な数値**: ベスト 1.00 s (`mark_compact_gen`)。

#### `life.ba.rb` — Conway's Life simulation

- **What**: 80×80 grid を 200 tick simulate。 各 tick で 6400 cell
  ×ick: `[0/1]` を含む row Array を H 個生成 → outer Array に push。
- **Alloc pattern**: tick あたり H + 1 = 81 alloc (row + outer)。
  total ~31 MB / 200 ticks。
- **Lifetime**: tick lifetime ((H+1) × W = 6481 cells)、 完了直後 die。
- **テスト対象**: GC pressure が低い (allocate << region size)、 mutator
  支配的なケースの差を見る。 全 backend が ~1.3 s で集まる (GC 影響なし)。
- **特性的な数値**: ベスト 1.31 s (`mark_gen_inc`)。 全 backend 1.31-1.41 s
  に集中。
- **歴史**: 2026-05-16 追加。 実装中に baruby parser の
  「binop + >3-arg call でオペランド競合」 バグを発掘 (done.md (11)/(12))。

#### `list_alloc.ba.rb` — pure allocation pressure

- **What**: 1000 万回ループで毎 iter 4-要素 array alloc + sum。
  array は次 iter で捨てる。
- **Alloc pattern**: 534 MB total / 10M iter。 1 iter = 1 alloc。
- **Lifetime**: 1 iter (即 die)。
- **テスト対象**: 純粋 alloc throughput。 bump alloc 系が圧勝。
- **特性的な数値**: ベスト 0.87 s (`copy_gen_inc`)、 ワースト 1.33 s
  (`none`)。

#### `list_sort.ba.rb` — merge sort with allocation-heavy merge

- **What**: 2000 要素整数配列を merge sort、 350 回反復。 各 merge が
  2 つの half + 出力 array を alloc。
- **Alloc pattern**: 287 MB total。 各 merge が中規模 alloc burst。
- **Lifetime**: parent merge return で全部 die — recursion stack 深さに
  比例する lifetime。
- **テスト対象**: 中規模 burst alloc → 親 return で die、 nursery 系
  benefit。
- **特性的な数値**: ベスト 1.05 s (`mark_bump_gen`)、 ワースト 1.27 s。
  全 backend が比較的近い (1.05-1.27)。

#### `nqueens.ba.rb` — backtracking with per-frame array

- **What**: N=11 の N-queens を backtracking で解く (2680 solutions)。
  各 call で column set を functional コピー (Array) して pass-down。
- **Alloc pattern**: 探索木の各 node で 1 array alloc。 ~26 MB total。
- **Lifetime**: 厳密 LIFO で短命 (backtrack で即 die)。
- **テスト対象**: deep recursion + LIFO 短命 alloc。 nursery 完結率
  100% に近い形。
- **特性的な数値**: ベスト 0.90 s (`mark_compact_gen`)、 全 backend
  0.90-1.07 s。 2026-05-16 追加。

#### `sieve.ba.rb` — Sieve of Eratosthenes for primes up to 10M

- **What**: 0..N の boolean 配列で素数判定。 sweep で合成数を `false` に
  上書き、 最後に survivor を `result` array に push。 N = 10^7 で primes
  数 = 664579。
- **Alloc pattern**: 1 つの **long-lived 大配列** (sieve: 80 MB の VALUE[])
  + 1 つの medium 配列 (result: 5.3 MB)。 alloc 頻度は低いが 1 つの payload
  が huge。 mark_compact_gen の pretenure 路や immix の medium 路を踏む。
- **Lifetime**: 全 alloc が long-lived。 sieve と result 両方が end まで live。
- **テスト対象**: 「単一 big object」 を扱う allocator の挙動。 scattered
  write pattern (合成数 cross-off の `j += i` で page-spread) で cache locality
  も負荷。
- **特性的な数値**: ベスト 1.36 s (`none`)、 GC-less が勝つ = mutator
  支配。 spread 比較的狭く 1.36 - 1.89 (-39% 〜 max)。 sieve は GC の差より
  alloc strategy 差が支配的。

#### `string_concat.ba.rb` — small string concat hot loop

- **What**: `"abc" + "def" + "ghi"` を 5M iter。 毎 iter 文字列リテラル
  3 つ + 連結結果 2 つ = 5 BaString alloc + 2 bytes payload。
- **Alloc pattern**: 710 MB total。 String 専用 alloc が dominant。
- **Lifetime**: 1 iter (即 die)。
- **テスト対象**: BaString + bytes payload の alloc 最適化、 generational
  完結率。
- **特性的な数値**: ベスト 0.53 s (`copy_gen_inc` / `mark_compact_gen`
  tied)、 ワースト 2.41 s (`mark`、 後に 1.68 s に改善)。

#### `substr_churn.ba.rb` — 長寿命 String を sliding window で読む

- **What**: 18 MB の 1 String (`repeat("abcdef", 3M)`) を生成、
  全 offset から 5-byte substring を切る。 ~3M substr alloc。
- **Alloc pattern**: 532 MB total (短命 substr) + 持続 18 MB (text)。
- **Lifetime**: 1 iter 短命 substr + permanent text。
- **テスト対象**: BaString slice の sp ref pattern、 long-lived + short-
  lived の混在 (gc_combined と類似だが string 軸)。
- **特性的な数値**: ベスト 0.87 s (`copy_gen_inc`)、 ワースト 1.85 s
  (`none`)。

### マクロベンチ評価軸

12 bench がカバーする観点を整理:

| 観点 | 代表 bench |
|---|---|
| pure alloc throughput | list_alloc, fib_pair |
| long-lived heap | binary_trees |
| 2 層 lifetime (long + short) | gc_combined, substr_churn, hash_chain |
| LIFO 短命 (nursery 完結率) | fib_pair, nqueens |
| burst → 静止 | interp_calc, list_sort |
| String 専用 alloc | string_concat, substr_churn |
| Write barrier heavy | hash_chain |
| mutator dominant (GC 影響薄) | life, nqueens |
| deep alloc chain | cons_list, binary_trees |
| Realloc-payload heavy | string_concat (str grow), hash_chain (chain.items grow) |

### Backend 選択ガイド

ワークロードの性質ごとの推奨:

| パターン | 推奨 backend | 理由 |
|---|---|---|
| 短命 alloc 多 (大半が捨てられる) | `mark_compact_gen` または `copy_gen` | nursery で完結、 tenure cost 最小 |
| 長寿命 heap が大半 (binary_trees 等) | `copy` または `mark_compact` | gen 無しで in-place / semispace の単純さ勝ち |
| string-heavy (concat / slice 多) | `copy_gen_inc` または `mark_compact_gen` | nursery + sp ref pattern の組合せ |
| 仮想空間を節約したい | `mark_compact_gen` | tenured 1× region (vs copy_gen の 2×) |
| GC レイテンシ最小化 | (現状) `bump` (no GC) または gen 系 minor | major のみ stop-the-world |
| 純粋な alloc コスト測定 | `bump` (leak base) または `none` (libc malloc) | rooting + dispatch のみ |

11 backend のうち default は `copy` (semispace Cheney) で、 全 backend
は plain mode と stress mode (`BARUBY_GC_STRESS=1`) の test 3 種を PASS、
bench 12 種が全完走。

### GC 時間計測 (`gc_seconds` / `gc_pct` / `max_pause_ms`)

`BARUBY_GC_STATS=1` で各 backend がミューテータ時間と GC 時間を分けて
出す。 8 backends (none / bump を除く) の collect entry を
`baruby_gc_time_begin/end` で挟み、 `CLOCK_MONOTONIC` で累計。
minor→major の re-entrant ケースは depth guard で最外側だけ計測。

2026-05-17 (17) で `max_pause_ms` も追加: 1 回の collect で最大の
wall clock time。 latency upper-bound として有用。

例 (binary_trees, plain):

```
backend=mark_gen          max_pause_ms=288.55  ← 単一の major sweep が支配
backend=mark_gen_inc      max_pause_ms=53.84   ← inc で start / finish_sweep 分割
backend=copy_gen          max_pause_ms=17.62   ← 各 minor、 major なし
backend=mark_bump_gen     max_pause_ms=54.98   ← major (promote + sweep)
```

mark_gen vs mark_gen_inc で max_pause が 5.4× 違う点に注目: 同じ
gc_seconds 規模でも単発 pause time は大きく異なり、 latency 重視
ワークロードでの選択基準になる (現状 INC_WORK_PER_ALLOC=SIZE_MAX なので
真の incremental ではないが、 mark / sweep の 2 段に分かれる効果)。

## 2.5 GC phase 内訳 (mark 時間 vs sweep / copy / compact 時間)

iter 32 で `BARUBY_GC_STATS=1` 出力に **`mark_seconds` / `reclaim_seconds`** を
追加。 各 backend の collect 関数で phase ごとに `clock_gettime` を取って
集計。 phase semantics:
- **mark&sweep** (`mark` / `mark_gen` / `mark_gen_inc` / `mark_bitmap_gen`):
  `mark` = root scan + gray queue 処理、 `reclaim` = sweep
- **mark&compact** (`mark_compact` / `mark_compact_gen` / `mark_bump_gen`):
  `mark` = trace, `reclaim` = forward-pass + update_pointers + slide
- **copy** (`copy` / `copy_gen` / `copy_gen_inc`): Cheney は trace と
  relocate が **交錯した単一 loop**。 mark phase を分離計上できないので
  `mark` = 0、 全部 `reclaim` に計上
- **bump / none**: GC 走らないので両方 0

binary_trees (depth 21, ~4M live Array、 mark-heavy):

| backend | mark (s) | reclaim (s) | gc total | gc % | wall (s) |
|---|---:|---:|---:|---:|---:|
| mark | 0.161 | 0.032 | 0.194 | 21% | 0.912 |
| mark_gen | 0.216 | 0.043 | 0.259 | 26% | 1.000 |
| copy | 0 | 0.252 | 0.252 | 34% | 0.746 |
| copy_gen | 0 | 0.565 | 0.565 | 61% | 0.933 |
| mark_compact | 0.127 | 0.127 | 0.254 | 33% | 0.773 |
| mark_compact_gen | 0.055 | 0.514 | 0.568 | 61% | 0.925 |

cons_list (短命 deep chain、 reclaim-heavy):

| backend | mark (s) | reclaim (s) | gc total | gc % | wall (s) |
|---|---:|---:|---:|---:|---:|
| mark | 0.001 | 0.072 | 0.073 | 9% | 0.789 |
| mark_gen | 0.006 | 0.060 | 0.067 | 9% | 0.763 |
| copy | 0 | 0.004 | 0.004 | 1% | 0.714 |
| copy_gen | 0 | 0.021 | 0.021 | 3% | 0.691 |
| mark_compact | 0.002 | 0.180 | 0.182 | 20% | 0.896 |
| mark_compact_gen | 0 | 0.022 | 0.022 | 3% | 0.696 |

fib_pair (再帰 stack、 ほぼ全部 garbage):

| backend | mark (s) | reclaim (s) | gc total | gc % | wall (s) |
|---|---:|---:|---:|---:|---:|
| mark | 0 | 0.095 | 0.095 | 11% | 0.885 |
| mark_gen | 0 | 0.068 | 0.068 | 8% | 0.865 |
| copy | 0 | 0 | 0 | 0% | 0.726 |
| copy_gen | 0 | 0 | 0 | 0% | 0.763 |
| mark_compact | 0 | 0.230 | 0.230 | 22% | 1.028 |
| mark_compact_gen | 0 | 0 | 0 | 0% | 0.781 |

考察:
- **mark&sweep の mark vs sweep 比は live ratio に直結**:
  - binary_trees (生存率 99%): mark=0.16s vs sweep=0.03s → mark 5×
  - cons_list (生存率 < 1%): mark=0.001s vs sweep=0.07s → sweep 70×
  - mark&sweep は alloc-rate より **|live heap|** に line up
- **Cheney (copy / copy_gen) の "tracing" コストは reclaim に flat に乗る**:
  binary_trees で copy が 0.25s 払う vs mark の 0.19s。 全 live を to-space
  に memcpy するので |live| linear。
- **mark_compact は mark と reclaim が概ね同等** (0.13s + 0.13s on binary_trees):
  forward pass + update + slide は heap-walk × 3 倍コスト。 mark の cost と
  ほぼ同程度。
- **gen 系の copy_gen / mark_compact_gen は major が 5-6× 重い** (binary_trees
  で 0.57s vs copy の 0.25s)。 binary_trees は long-lived なので promotion
  したオブジェクトを毎 major 全部 Cheney する loss。

## 3. libgc 列の `life` 不在について

`sample/baruby` (libgc) は top-level の long while loop 後の最終式値が
壊れる独立バグを抱えていて `bench/life.ba.rb` で誤った結果を返す。 GC
側のバグではないが fair 比較にならないため §2 table の libgc 列は `life`
を空欄 (`—`) としている。 baruby_precise の `life` 行 (13 backend で測定)
は table に残してある。 残り 13 bench (= 14 - life) は baruby (libgc) も
同一スクリプトを走らせて取得した数値で、 GC algorithm 以外の差分は無い。
parser fix (commit 34be8d2)、 fannkuch (iter 24)、 sieve (iter 28) は baruby
に port 済。

## 4. Stress mode

`BARUBY_GC_STRESS=1` で「毎 alloc で GC」 + 「古い from-space を恒久
PROT_NONE + MADV_DONTNEED」 のデバッグモードに切替。 stale pointer を
deref した瞬間 SIGSEGV するので moving GC 特有のバグ (rooting 漏れ、
helper 内 C local の更新漏れ) が即発覚する。 詳細は
[runtime.md §5.3](runtime.md)。

開発中に表面化した代表的バグ:

- `baruby_ary_push(VALUE av, VALUE x, ...)` の `x` が realloc トリガで
  stale → 新 items[len] に旧アドレス書込 → 子 GC で再 free 失敗で破綻
  → `VALUE *x_ref` に変更して post-GC 再 read
- `node_eq` / `_neq` / `_lt` / `_le` / `_gt` / `_ge` / `_mul` /
  `_spaceship` / `_call_aget` / `_call_aget2` の `VALUE l = EVAL_ARG`
  パターンを sp[] spill に直す
- `baruby_str_concat` / `_repeat` / `_append`, `baruby_ary_plus` /
  `_repeat` の helper を VALUE による値引数から `VALUE *ref` に変更
- 特に `baruby_str_concat` は malloc/memcpy/free で source bytes を
  バッファリングしていたのを ref pattern に切り替え、 **1.468 s → 1.160 s
  (-21%)** に短縮

## 5. 効いた最適化 (履歴)

### 4.1 `baruby_str_concat` を ref pattern に
旧版: 内部 alloc 前に source bytes を libc malloc 領域に退避してから
alloc 後にコピー (helper が VALUE 値受けの制約)。
新版: `VALUE *av_ref` / `*bv_ref` で受け、 alloc 後に `*ref` 再 deref
で post-GC アドレスを取り直す。 malloc/memcpy/free を 2 回ずつ削減。
→ string_concat 1.468 s → 1.160 s (-21%)。

### 4.2 `baruby_str_new` の malloc バッファ撤去
旧版: source bytes が heap interior pointer の場合に備えて毎回 malloc
バッファコピー。 実際の呼び出し元の大半は rodata 文字列 (literal) で、
無駄。
新版: `baruby_str_new` は source が survive することを前提とし、
malloc を撤去。 heap source 用には `baruby_str_slice(*src_ref, off, len)`
を新設して `node_call_aget` / `_aget2` の substring 経路を分離。
→ string_concat の malloc/free が消えて 1.160 s → 0.961 s (-17%)、
   substr_churn は 1.594 s → 1.354 s (-15%)。

### 4.3 `KIND_PAYLOAD_BYTE` の memset スキップ
String の bytes ペイロードは GC が pointer として読まないので、 alloc
直後の memset は不要。 `baruby_gc_alloc_byte` を新設して分岐。 caller
(`baruby_str_*` 系) は即座に bytes を書き込む。

### 4.4 LTO (`-flto=auto`) 有効化
`baruby_gc_alloc` を含む小関数がコールサイトに inline され、 size 引数
が定数畳み込みされる。 fib_pair 等の小型 alloc が多いベンチで効く。

## 6. 既知の問題

- **AOT mode は moving GC 移行後に未検証** — SD bake された経路で
  precise rooting が成立しているかは要再 audit (`-c` 動作含む)
- `mark` family の `hash_chain` slab 配置 locality (BaArray と items[] が
  別 page) は `mark_bitmap_gen` で 8 B header にして class 32 に同梱できる
  と確認したが、 24 B header の既存系は構造改変が必要

## 7. 次の段階で試したいこと

- **Immix v2 opportunistic evacuation** (fragmentation 解消)
- **mark_bitmap_gen minor sweep の最適化**: 64-bit-wise old_bm scan、
  per-page "all old" flag で binary_trees regression を縮小
- AOT mode の再検証 (`make CCACHE_DISABLE=1` で `-c` 経路を回す)
- `astrogen.rb` 拡張で `@locals` を機械化 (手書きの error-prone を減らす)
- `mark_gen_inc` / `copy_gen_inc` を真の incremental に (stack write
  barrier + work budget)

(iter 29 で完了: `mark_compact` / `copy` / `copy_gen` / `copy_gen_inc` /
`mark_compact_gen` に adaptive 16 MiB threshold 追加で fair 化。 全 backend
の MIN を 16 MiB に統一。 詳細は [gc_runtime.md §6](gc_runtime.md) と
done.md iter 29。)
