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
- **Mode**: `--plain` を正本とする (default)。 iter 36 で AOT (`-c`) も修復
  済 (Makefile 絶対パス macro + main.c が `-I` を extra_cflags 経由で渡す、
  node.c の cs_init version に `BARUBY_GC` を渡して backend 切替で
  code_store invalidation)。 AOT 数値は §2 後半の AOT matrix。
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

iter 36-49 で再計測した median-of-3 (`ruby bench/matrix.rb`)。 15 backend
× 19 bench + libgc column。 `copy_gen_inc` は実体が copy_gen の clone
なので除外 (iter 41 で `mark_freelist` を追加し total 16、 列で 15)。

iter 36-40 で追加:
- **`mark_card_gen` backend** (#15): page-level remset (bounded by page count)
- **`ast_eval` bench**: AST builder + evaluator (long-lived tree + short-lived intermediate)
- **`remset_pressure` bench**: 50K cell chain + 200K sparse young store (adversarial old→young write)
- **`string_concat_dyn` bench**: 関数経由 chunk + 動的 concat (string_concat が parse-time fold で 1-alloc/iter に縮んだので、 本来の N-alloc/iter pattern を保持する別 bench を追加)
- **`node_ary_lit_N` (N=1..4) optimization**: 配列リテラル N=1..4 を 1-shot 化 (chain → direct)
- **String literal concat fold** (iter 37): parser で `node_str_lit + node_str_lit` を parse-time fold

iter 40 で追加:
- **`dll_walk` bench**: doubly-linked list build + forward/backward walk。
  cons_list の bidirectional 版 (3-要素 node、 2 refs/node)。 mark phase の
  pointer-count スケーリングと `cur[2] = nxt` の WB stress を測定。

iter 41 で追加:
- **`mark_freelist` backend** (#16): region + per-class freelist + non-compact
  M&S。 gc_mark (slab page) と gc_mark_compact (region + slide) の中間。
  page metadata / malloc 不要、 ただし fragmentation あり。

iter 48 で追加:
- **`tokenize` bench**: CSV-like 文字列分割 (20 × "red,blue,...,purple" を `,`
  で 120 tokens に分解 + tokens.push、 17500 iter)。 `baruby_str_slice` と
  `baruby_ary_push` を一緒に exercise する macro。 mark_compact_gen が
  winner 0.88 s。 副次: bench 投入時に **mark_freelist の memset 抜けバグ
  発覚** (iter 41 以来の dormant bug、 freelist popped slot の stale data が
  BaString.bytes に残って GC mark crash)。 fix も iter 48 で commit。

iter 43-45 inline-friendly optimization series (3 段階):
- **iter 43**: region-based 9 backend (copy / copy_gen / copy_gen_inc /
  mark_compact / mark_compact_gen / mark_bump_gen / immix / immix_gen /
  mark_freelist) の bump path に `__attribute__((noinline, cold))` +
  `__builtin_expect(..., 0)` を適用。 `aro_gc_alloc.constprop.0` (str_new
  系の non-inline callee) を 75 LOC → 54 LOC (-28%) 縮小。 string 系 bench
  で 8-18% 改善。
- **iter 44**: slab 系 5 backend (mark / mark_gen / mark_bitmap_gen /
  mark_card_gen / mark_freelist) の `size_class_for` 9-cmp linear scan を
  `__builtin_clz` based O(1) に置換。 aro_gc_alloc body 21% 縮小、
  baruby_ary_new に **aro_gc_alloc 完全 inline** 成立。 mark backend で
  list_alloc -8%、 gc_combined -7% など。
- **iter 45**: 残る 4 slab-gen backend (mark_gen / mark_gen_inc /
  mark_bitmap_gen / mark_card_gen) の `maybe_collect` に cold-split。
  mark_gen_inc string_concat -7% など、 marginal だが consistent な改善。

3 iter で全 15 backend (copy_gen_inc 除く) の `aro_gc_alloc` が caller 側で
inline (定数 size) または slim な constprop clone 経由になり、 共通の
`baruby_ary_new` / `baruby_str_new` で広範な improvement。

iter 38 (correctness; v2 で perf neutral):
- **`immix_gen` / `mark_bitmap_gen` の remset overflow 対応**: iter 36 で
  これらの 2 backend だけは abort だったが、 iter 38 で解決。
  - `mark_bitmap_gen`: per-page `dirty_bm` を直接 scan する heap-walk
    fallback (overhead 0)
  - `immix_gen` v1: per-promotion `tenured_objs[]` push → cache pressure で
    5-15% regression
  - `immix_gen` v2: **pressure-triggered minor** — `remset_push` で cap-1 に
    達したら flag、 次 alloc safepoint で minor を強制 → remset drain。
    promotion path に新コードなし、 regression なし。
  - 詳細は [done.md (38)](done.md) と [gc_runtime.md §3](gc_runtime.md)。

### Plain mode matrix (iter 49)

| Bench | none | mark | mark_gen | mark_gen_inc | copy | copy_gen | mark_compact | mark_compact_gen | bump | mark_bump_gen | immix | immix_gen | mark_bitmap_gen | mark_card_gen | mark_freelist | libgc |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| ast_eval | 0.35 | 0.34 | 0.35 | **0.34** | 0.36 | 0.36 | 0.37 | 0.39 | 0.36 | 0.36 | 0.34 | 0.34 | 0.35 | 0.35 | 0.38 | 0.37 |
| binary_trees | 0.74 | 0.79 | 0.83 | 0.81 | 0.75 | 0.77 | 0.76 | 0.71 | **0.44** | 0.92 | 0.53 | 0.76 | 0.94 | 0.95 | 0.78 | 0.79 |
| cons_list | 1.12 | 0.73 | 0.84 | 0.88 | 0.67 | 0.72 | 0.80 | 0.72 | 0.83 | 0.72 | 0.67 | **0.64** | 0.80 | 0.83 | 0.72 | 0.88 |
| dll_walk | 0.97 | 0.73 | 0.83 | 0.84 | 0.71 | 0.78 | 0.80 | 0.76 | 0.84 | 0.76 | **0.70** | 0.70 | 0.80 | 0.78 | 0.76 | 0.85 |
| fannkuch | 0.75 | 0.73 | 0.76 | 0.76 | 0.70 | 0.71 | 0.70 | **0.68** | 0.71 | 0.71 | 0.71 | 0.70 | 0.74 | 0.77 | 0.74 | 0.71 |
| fib_pair | 1.37 | 0.83 | 0.94 | 1.02 | 0.74 | 0.77 | 0.92 | 0.78 | 0.97 | 0.87 | 0.84 | **0.69** | 0.88 | 0.90 | 0.77 | 1.02 |
| gc_combined | 1.22 | 0.77 | 0.89 | 0.94 | 0.70 | 0.74 | 0.87 | 0.75 | 0.97 | 0.74 | 0.73 | **0.69** | 0.83 | 0.88 | 0.83 | 0.93 |
| hash_chain | 1.14 | 1.15 | 1.10 | 1.15 | 1.21 | 1.20 | 1.17 | 1.19 | 1.11 | 1.19 | **1.09** | 1.11 | 1.14 | 1.12 | 1.30 | 1.13 |
| interp_calc | 1.19 | 0.92 | 0.97 | 1.01 | **0.81** | 0.87 | 0.99 | 0.87 | 0.98 | 0.91 | 0.83 | 0.81 | 0.92 | 0.94 | 0.88 | 1.01 |
| life | 1.32 | 1.30 | 1.31 | 1.30 | 1.32 | 1.29 | 1.31 | 1.32 | 1.31 | 1.36 | **1.26** | 1.31 | 1.32 | 1.32 | 1.27 | — |
| list_alloc | 1.16 | 0.75 | 0.81 | 0.85 | 0.68 | 0.71 | 0.84 | 0.68 | 0.92 | 0.70 | 0.69 | **0.66** | 0.78 | 0.79 | 0.79 | 0.85 |
| list_sort | 1.16 | 1.18 | 1.21 | 1.23 | 1.04 | 1.07 | 1.03 | 1.03 | 1.16 | 1.03 | 1.03 | **1.01** | 1.27 | 1.28 | 1.20 | 1.11 |
| nqueens | 0.96 | 0.98 | 0.98 | 0.97 | 0.93 | 0.95 | 0.93 | 0.95 | 0.93 | 0.94 | 0.93 | **0.90** | 0.96 | 1.00 | 0.99 | 0.94 |
| remset_pressure | 0.48 | 0.34 | 0.35 | 0.37 | 0.36 | 0.30 | 0.41 | 0.30 | 0.35 | 0.30 | 0.33 | **0.29** | 0.34 | 0.34 | 0.36 | 0.44 |
| sieve | **1.22** | 1.35 | 1.46 | 1.40 | 1.61 | 1.42 | 1.61 | 1.42 | 1.55 | 1.42 | 1.63 | 1.40 | 1.45 | 1.44 | 1.51 | 1.36 |
| string_concat | 0.42 | 0.24 | 0.26 | 0.28 | 0.21 | 0.19 | 0.27 | 0.18 | 0.27 | 0.19 | 0.21 | **0.18** | 0.26 | 0.25 | 0.22 | 0.29 |
| string_concat_dyn | 2.30 | 1.21 | 1.36 | 1.41 | 0.98 | 0.98 | 1.35 | 0.98 | 1.38 | 0.99 | 1.02 | **0.96** | 1.28 | 1.26 | 1.15 | 1.47 |
| substr_churn | 1.70 | 0.98 | 1.04 | 1.20 | 0.94 | 0.79 | 1.12 | 0.82 | 1.10 | **0.78** | 1.05 | 0.78 | 1.00 | 1.00 | 0.99 | 1.30 |
| tokenize | 2.40 | 1.14 | 1.34 | 1.38 | 0.91 | 0.91 | 1.38 | **0.88** | 1.34 | 0.89 | 0.97 | 0.89 | 1.28 | 1.26 | 1.08 | 1.39 |

**勝者分布** (plain, iter 43、 median of 3、 18 bench、 15 backend、
**iter 43 で cold-path split を 9 backend に展開**):
- `immix_gen` — **8 wins** (cons_list 同位、 dll_walk 同位、 fib_pair /
  gc_combined / list_alloc / list_sort / remset_pressure / string_concat /
  string_concat_dyn)
- `immix` — **3 wins** (cons_list / hash_chain / nqueens)
- `bump` — **3 wins** (ast_eval 同位 / binary_trees / sieve は none と
  tied だが winner none)
- `mark_bump_gen` — **2 wins** (fannkuch / substr_churn)
- `none` — **1 win** (sieve)
- `mark_freelist` (新 #16) — **1 win** (dll_walk 同位 0.67-0.69 と immix_gen と
  かなり close)
- `mark` — **1 win** (life)
- `copy` — **1 win** (interp_calc)

iter 43 の cold-path split で **string 系で 8-18% 改善**: string_concat
immix_gen 0.20 → 0.17、 string_concat_dyn copy_gen 1.11 → 0.95、
substr_churn copy_gen 0.95 → 0.79、 substr_churn mark_compact_gen
0.89 → 0.77 など。 仮説通り、 baruby_str_new が aro_gc_alloc を call する
経路 (constprop clone) で hot body 縮小の効果が出た形。

iter 44 で slab 系 5 backend (mark / mark_gen / mark_bitmap_gen /
mark_card_gen / mark_freelist) の `size_class_for` を 9-cmp linear scan
から `__builtin_clz` based O(1) に置換。 aro_gc_alloc body が mark で
0x1b8 → 0x15c (-21%) 縮小、 **baruby_ary_new から aro_gc_alloc が完全
inline されるようになった** (`call aro_gc_alloc` が `call slab_alloc` 1 つ
に消化)。 効果 (mark backend):
- list_alloc 0.77 → 0.71 (**-8%**)
- gc_combined 0.84 → 0.78 (**-7%**)
- cons_list 0.74 → 0.70 (**-5%**)
- string_concat 0.25 → 0.23 (**-8%**)
- binary_trees / fib_pair / dll_walk 3-5%

mark_gen も list_alloc 0.86 → 0.79 (-8%) など同様。 mark_bitmap_gen /
mark_card_gen は bitmap/card 操作で body 残量があり inline 閾値を越えず
noise 内。 mark_freelist は元から lean で変化なし。

iter 45 で残る 4 slab-gen backend (mark_gen / mark_gen_inc /
mark_bitmap_gen / mark_card_gen) の `maybe_collect` に cold-split を適用
(collect dispatch 部分を `__attribute__((noinline, cold))` の helper に
extract)。 mark_bitmap_gen aro_gc_alloc body が 0x197 → 0x152 (-22%)。
効果は marginal だが consistent な方向:
- mark_gen_inc string_concat -7%、 cons_list/fib_pair/list_alloc -4%
- mark_bitmap_gen cons_list -4%
- mark_card_gen list_alloc -5%
- mark_gen list_alloc -3%

iter 43 region cold-split、 iter 44 slab clz、 iter 45 slab-gen cold-split
で全 region/slab backend が aro_gc_alloc の inline 候補に揃った。
- `copy_gen` — **1 win** (fannkuch)
- `none` — **1 win** (sieve)
- `mark_bump_gen` — tied/close on substr_churn (0.85)

**`mark_freelist` の dll_walk 勝利** (0.71 vs immix_gen 0.72 / copy 0.72) は
構造的な理由: dll_walk は build phase で `cur[2] = nxt` を多用するが、
非 gen の mark_freelist には WB がない (no-op)。 一方 immix_gen / copy_gen 等
の gen backend は WB + remset push を payy する。 non-gen で region 持ち
かつ malloc 不要なのは mark_freelist だけで、 bidirectional structure 構築
の overhead を最小化。 `mark` (slab malloc 経由) より速いのも同 reason +
page metadata なし。

### AOT mode matrix (iter 49, `ruby bench/matrix.rb --mode aot`)

| Bench | none | mark | mark_gen | mark_gen_inc | copy | copy_gen | mark_compact | mark_compact_gen | bump | mark_bump_gen | immix | immix_gen | mark_bitmap_gen | mark_card_gen | mark_freelist | libgc |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| ast_eval | 0.06 | 0.06 | 0.06 | 0.06 | 0.06 | 0.07 | 0.07 | 0.07 | **0.06** | 0.07 | 0.06 | 0.06 | 0.06 | 0.06 | 0.06 | 0.36 |
| binary_trees | 0.39 | 0.48 | 0.53 | 0.55 | 0.53 | 0.50 | 0.50 | 0.46 | **0.21** | 0.68 | 0.30 | 0.48 | 0.67 | 0.67 | 0.48 | 0.78 |
| cons_list | 0.60 | 0.25 | 0.32 | 0.37 | 0.20 | 0.21 | 0.32 | 0.21 | 0.36 | 0.21 | 0.21 | **0.17** | 0.34 | 0.33 | 0.23 | 0.84 |
| dll_walk | 0.95 | 0.72 | 0.24 | 0.27 | 0.73 | 0.17 | 0.81 | 0.17 | 0.85 | 0.18 | 0.74 | **0.15** | 0.26 | 0.24 | 0.79 | 0.83 |
| fannkuch | 0.73 | 0.73 | 0.14 | 0.15 | 0.72 | 0.12 | 0.73 | 0.11 | 0.73 | **0.11** | 0.73 | 0.13 | 0.17 | 0.16 | 0.78 | 0.70 |
| fib_pair | 0.83 | 0.35 | 0.44 | 0.49 | 0.27 | 0.26 | 0.45 | 0.32 | 0.50 | 0.27 | 0.30 | **0.24** | 0.42 | 0.42 | 0.32 | 1.04 |
| gc_combined | 0.67 | 0.29 | 0.34 | 0.38 | 0.20 | 0.20 | 0.35 | 0.20 | 0.43 | 0.20 | 0.22 | **0.18** | 0.34 | 0.34 | 0.28 | 0.91 |
| hash_chain | 1.13 | 1.10 | 0.16 | 0.16 | 1.25 | 0.18 | 1.20 | 0.17 | 1.12 | 0.17 | 1.11 | **0.15** | 0.16 | 0.17 | 1.22 | 1.34 |
| interp_calc | 0.56 | 0.28 | 0.33 | 0.38 | 0.23 | 0.26 | 0.37 | 0.25 | 0.38 | 0.28 | 0.25 | **0.22** | 0.35 | 0.34 | 0.26 | 0.98 |
| life | **0.14** | 0.17 | 0.16 | 0.17 | 0.15 | 0.15 | 0.15 | 0.15 | 0.15 | 0.15 | 0.15 | 0.15 | 0.18 | 0.18 | 0.16 | — |
| list_alloc | 0.66 | 0.29 | 0.33 | 0.38 | 0.19 | 0.19 | 0.34 | 0.22 | 0.42 | 0.19 | 0.21 | **0.17** | 0.33 | 0.33 | 0.27 | 0.84 |
| list_sort | 0.33 | 0.35 | 0.38 | 0.42 | 0.22 | 0.22 | 0.27 | 0.23 | 0.33 | 0.22 | 0.23 | **0.21** | 0.43 | 0.42 | 0.37 | 1.10 |
| nqueens | 0.08 | 0.09 | 0.09 | 0.09 | 0.08 | 0.08 | 0.08 | **0.07** | 0.08 | 0.08 | 0.08 | 0.07 | 0.10 | 0.09 | 0.09 | 0.94 |
| remset_pressure | 0.46 | 0.34 | 0.13 | 0.16 | 0.37 | **0.08** | 0.41 | 0.08 | 0.37 | 0.09 | 0.35 | 0.08 | 0.13 | 0.12 | 0.37 | 0.43 |
| sieve | 1.22 | 1.32 | 0.42 | 0.43 | 1.64 | 0.43 | 1.62 | 0.43 | 1.60 | 0.41 | 1.61 | **0.38** | 0.47 | 0.46 | 1.56 | 1.33 |
| string_concat | 0.29 | 0.11 | 0.14 | 0.15 | 0.09 | 0.07 | 0.15 | 0.07 | 0.15 | 0.07 | 0.08 | **0.06** | 0.13 | 0.13 | 0.10 | 0.29 |
| string_concat_dyn | 1.49 | 0.56 | 0.68 | 0.76 | 0.38 | 0.36 | 0.73 | 0.37 | 0.76 | 0.35 | 0.42 | **0.35** | 0.63 | 0.62 | 0.49 | 1.44 |
| substr_churn | 1.08 | 0.45 | 0.50 | 0.55 | 0.42 | 0.26 | 0.57 | 0.25 | 0.76 | **0.25** | 0.59 | 0.28 | 0.47 | 0.47 | 0.52 | 1.33 |
| tokenize | 1.94 | 0.73 | 0.89 | 0.99 | 0.49 | 0.46 | 0.94 | **0.46** | 0.94 | 0.46 | 0.55 | 0.47 | 0.84 | 0.82 | 0.64 | 1.36 |

**勝者分布** (AOT, iter 44、 15 backend × 18 bench):
- `immix_gen` — **9 wins** (cons_list tied / dll_walk / fib_pair /
  gc_combined / hash_chain / list_alloc / sieve / string_concat) +
  immix_gen ≦ 0.01 で他多数の tie
- `mark_compact_gen` — **2 wins** (fannkuch tied 0.11 / nqueens 0.07)
- `copy_gen` — **2 wins** (list_sort 0.21 / remset_pressure 0.08)
- `copy` — **2 wins** (interp_calc 0.23 / string_concat_dyn 0.38)
- `bump` — **1 win** (binary_trees 0.21)
- `mark` — **1 win** (ast_eval 0.06)
- `immix` — **1 win** (life 0.14、 多数 tied)
- `mark_bump_gen` — **1 win** (substr_churn 0.30)
- `mark_freelist` (新 #16) — 1 tie (cons_list 0.20 with immix_gen) +
  競争的だが直接の勝利なし。 AOT で gen backend の minor 効率が支配的に
  なる workload では gen 系に届かない。

`mark_freelist` AOT 評価: 短命 alloc 多めの workload (cons_list 0.20、
gc_combined 0.24、 list_alloc 0.23) で gen に近い数値を出す。 一方
hash_chain / sieve など非 gen が苦手な領域では同じく苦手 (1.19 / 1.55)。
「非 gen で freelist の reuse 効率を測る」 backend として位置付け確立。

### iter 44 AOT 効果サマリ (vs iter 41 AOT baseline)

slab `mark` backend (size_class_for clz 化 + aro_gc_alloc inline 成立):
- cons_list AOT: 0.29 → 0.25 (**-14%**)
- gc_combined AOT: 0.33 → 0.30 (**-9%**)
- fib_pair AOT: 0.40 → 0.36 (**-10%**)
- interp_calc AOT: 0.31 → 0.28 (**-10%**)
- list_alloc AOT: 0.32 → 0.29 (**-9%**)
- string_concat_dyn AOT: 0.62 → 0.56 (**-10%**)
- substr_churn AOT: 0.49 → 0.45 (**-8%**)

iter 43 cold-split + iter 44 clz の合わせ技で region-based + slab 系の
両方が改善: copy_gen / mark_bump_gen / mark_compact_gen で string_concat
AOT 0.08 → 0.06 (-25%)、 substr_churn AOT 0.30 → 0.23 (-23%) など。
plain matrix と AOT matrix で同方向の signal が出るのは inline 化が
本物の改善である裏付け。

### CRuby 比較 (iter 42 追加)

baruby benches は意図的に Ruby サブセットで書かれているので、 同じ `.ba.rb`
を `ruby` (CRuby 3.4) でも実行できる。 ref として median-of-3 を取り、
baruby_precise の plain 最速 backend / AOT 最速 backend と並べる:

| Bench | CRuby (s) | plain best (backend) | plain 倍率 | AOT best | AOT 倍率 |
|---|---:|---:|---:|---:|---:|
| ast_eval | 0.51 | 0.34 (immix) | **1.5×** | 0.06 (mark) | **8.5×** |
| binary_trees | 1.01 | 0.44 (bump) | **2.3×** | 0.21 (bump) | **4.8×** |
| cons_list | 1.66 | 0.67 (immix_gen) | **2.5×** | 0.20 (immix_gen) | **8.3×** |
| dll_walk | 1.72 | 0.71 (mark_freelist) | **2.4×** | 0.17 (immix_gen) | **10.1×** |
| fannkuch | 0.86 | 0.69 (copy_gen) | **1.2×** | 0.11 (mark_compact_gen) | **7.8×** |
| fib_pair | 1.20 | 0.73 (immix_gen) | **1.6×** | 0.26 (immix_gen) | **4.6×** |
| gc_combined | 1.02 | 0.70 (immix_gen) | **1.5×** | 0.20 (immix_gen) | **5.1×** |
| hash_chain | 1.46 | 1.10 (bump) | **1.3×** | 0.15 (immix_gen) | **9.7×** |
| interp_calc | 1.38 | 0.80 (immix) | **1.7×** | 0.23 (copy/immix_gen) | **6.0×** |
| life | 1.19 | 1.25 (bump) | **0.95×** | 0.14 (immix) | **8.5×** |
| list_alloc | 0.97 | 0.68 (immix_gen) | **1.4×** | 0.19 (immix_gen) | **5.1×** |
| list_sort | 7.26 | 1.01 (immix_gen) | **7.2×** | 0.21 (copy_gen) | **34.6×** |
| nqueens | 0.85 | 0.92 (immix_gen) | **0.92×** | 0.07 (mark_compact_gen) | **12.1×** |
| remset_pressure | 0.58 | 0.28 (immix_gen) | **2.1×** | 0.08 (copy_gen) | **7.3×** |
| sieve | 1.44 | 1.31 (none) | **1.1×** | 0.36 (immix_gen) | **4.0×** |
| string_concat | 1.39 | 0.20 (immix_gen) | **7.0×** | 0.07 (immix_gen) | **19.9×** |
| string_concat_dyn | 2.17 | 1.03 (copy) | **2.1×** | 0.38 (copy) | **5.7×** |
| substr_churn | 1.95 | 0.85 (immix_gen) | **2.3×** | 0.30 (mark_bump_gen) | **6.5×** |

幾何平均 (18 bench): plain で **CRuby 比 1.83×**、 AOT で **CRuby 比 7.77×**。
plain で CRuby より遅いのは `life` (0.95×) と `nqueens` (0.92×) — どちらも
mutator-bound (recursion + integer-only) で baruby の dispatch overhead が
GC win を相殺。 AOT mode では dispatch が SD bake で消えて全 bench で
CRuby に勝つ (最低 4×、 最高 34.6×)。

注意:
- CRuby は JIT 無効 (`ruby` default) で測定。 YJIT/MJIT で測ると数値が
  変わる可能性
- list_sort は CRuby が 7.3 s と特に遅い (mark-sort + merge の C extension
  路を踏まず interpreter 内で動作)。 baruby は同 algorithm を直接インタプリタ
  で実行している
- 言語サブセット (no OO / no proc / no eval) なので CRuby と完全公平では
  ない。 が同一ソースを動かせる意味では参考値として有用

AOT mode は dispatch overhead が SD bake で消えるので plain 比 **2-5× 高速化**
(bench / backend による)。 GC + memmove が相対的に支配的に。 iter 37 の
string literal const-fold で AOT string_concat が **0.34 → 0.07s (-79%)**
と劇的に縮んだ — alloc 5 個 / iter → 1 個 / iter になり、 残るは固定文字列
の参照のみ (実質 GC を測っていない)。 動的版 (string_concat_dyn) の方が
今後の string allocator 評価には適切。

iter 38 v2 (pressure-triggered minor) は AOT でも overhead 無し: immix_gen
は plain の WB cmp+jmp 1 つだけ追加で、 AOT bake 後の hot dispatch path
には現れない (WB は SD 外の helper)。 数値変化は system noise 範囲内
(string_concat_dyn 0.39 で copy と同位など)。

### iter 36 Perf 1 (array literal 1-shot) の効果

plain mode (copy backend、 主な改善):

| Bench | before | after | Δ |
|---|---:|---:|---:|
| fib_pair | 0.87 | 0.77 | **-11%** |
| gc_combined | 0.86 | 0.76 | **-12%** |
| list_alloc | 0.82 | 0.72 | **-12%** |
| interp_calc | 0.95 | 0.86 | **-9%** |
| binary_trees (bump) | 0.51 | 0.46 | **-10%** |

AOT mode (immix_gen backend、 主な改善):

| Bench | before | after | Δ |
|---|---:|---:|---:|
| gc_combined | 0.28 | 0.19 | **-32%** |
| list_alloc | 0.30 | 0.19 | **-37%** |
| fib_pair | 0.31 | 0.26 | **-16%** |
| ast_eval | 0.35 (immix) | 0.06 | **-83%** (大部分は fairness fix 由来も含む) |

### iter 37 Perf 2 (string literal concat fold) の効果

`baruby_parse.c::alloc_binop` で `lhs->kind == node_str_lit && rhs->kind ==
node_str_lit && op == +` を parse-time fold (両 byte 列を `malloc` で連結
して 1 つの `node_str_lit` に縮約)。 `"a" + "b" + "c"` のような完全リテラル
連結が 5 個の string alloc / iter → 1 個の static reference / iter になる。

plain mode (immix_gen backend、 主な改善):

| Bench | before (iter 36) | after (iter 37) | Δ |
|---|---:|---:|---:|
| string_concat | 0.48 | 0.20 | **-58%** |

AOT mode (immix_gen backend、 主な改善):

| Bench | before (iter 36) | after (iter 37) | Δ |
|---|---:|---:|---:|
| string_concat | 0.34 | 0.07 | **-79%** |

**注意**: この最適化は bench の意図 (string allocator 評価) を侵食する。
本来の N-alloc / iter pattern を保存する `string_concat_dyn.ba.rb` (関数
経由で chunk を作って動的に concat) を追加。 iter 37 以降は string_concat
を「const-fold 後のフロア」、 string_concat_dyn を「dynamic concat の実コスト」
として両方測る。

### 注目点 (iter 37)

- **`mark_card_gen` ≈ `mark_bitmap_gen`**: 同 layout で remset entry が
  object→page なだけの差。 多くの bench で ±2% 以内。 fundamental win は
  「remset 上限が page count に bounded」 (= 安全性) で raw 速度の差ではない。
- **`hash_chain` で `bump` が頂点** (plain 1.29): 短命 hash bucket alloc
  ばかりなので GC ゼロが効く。 `mark_gen` 系 (1.33) も copy 系 (1.51-1.62)
  に対し勝つ — non-moving + gen の remset が hash bucket sparse update に刺さる。
- **`binary_trees` で `mark_bitmap_gen` / `mark_card_gen` が最遅** (0.92-0.94)。
  per-page bitmap の locate() が per-mark で重い。 構造的に bitmap GC は
  SIMD-friendly な bulk 操作向きで、 ASTro の散発 mark には向かない。
- **`bump` が binary_trees で他に倍速** (0.47 vs copy 0.79) — GC ゼロ。
- **`immix_gen` 一強化** (iter 37): 17 bench 中 11 で plain 最速、 AOT では
  14。 iter 36 final の 7 wins から大幅拡大。 string fold (iter 37 Perf 2)
  と string_concat_dyn 追加で string 系 bench が増えたことが主因 —
  immix_gen の line allocator が medium-sized string alloc に強い。
- **AOT mode で gen backend が圧倒的**: hash_chain で immix_gen 0.16 vs
  mark 1.35 (8×)、 fannkuch で mark_bump_gen 0.12 vs mark 0.74 (6×)。 SD bake
  で mutator path が薄くなり、 GC efficiency の差が露出。

### 旧データの扱い

iter 31-34 までの「best-of-3」 結果と iter 35 fairness contract 後の数値は
threshold 統一 / charging 統一 / inc_step timer 入れたことで意味的に
不連続なので、 過去 iter 比較表は per-iter done.md で保持し、 perf.md は
fair 数値だけを正本にする。


(iter 31 / 33 / 34 までの勝者分布や per-iter delta は old measurement
contract での比較なので削除。 現在の正本は §0 の fairness contract に
基づく iter 36 数値のみ。 履歴は [done.md](done.md) を参照)

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
- **`copy_gen_inc`** は **placeholder** (実体 copy_gen の clone)。
  matrix runner / 表から除外。 詳細は `gc_copy_gen_inc.c` の冒頭コメント
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

### ベンチカタログ (全 19 種)

各ベンチの「何を / どう alloc して / lifetime はどんな形か」 を一覧。
GC 評価の観点で workload 分類を意識して揃えている。 アルファベット順。

#### `ast_eval.ba.rb` — AST builder + repeated evaluator (macro pattern)

- **What**: 木形 AST を作り (depth=N、 op = const / add / mul / sub / ite)、
  その後 evaluator で繰り返し評価。 各 evaluation は intermediate Array を
  alloc して結果を組み立てる (macro pattern: build phase + repeated eval)。
- **Alloc pattern**: build 時に 3-要素 Array が大量に promote される。
  eval 時は短命 intermediate が大量に。
- **Lifetime**: AST nodes は long-lived (promote 対象)、 eval intermediate は
  短命 (nursery で完結)。 gc_combined の親戚で「実 interpreter ループ」 寄り。
- **テスト対象**: gen backend の世代分離効果、 deep recursion mark cost、
  remset 圧 (eval から AST nodes へは read のみ、 WB 圧は低い)。
- **特性的な数値**: 0.34-0.39 s で backend 差小 (mutator-bound)。 oracle=0。

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

#### `cons_list.ba.rb` — deep linked-list chain (single-linked)

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

#### `dll_walk.ba.rb` — doubly-linked list, fwd + bwd walk (iter 40)

- **What**: 4000 セルの DLL (each node = `[val, prev, next]` 3-要素 BaArray)
  を build、 forward walk と backward walk で合計を出す。 1500 iter。
- **Alloc pattern**: 1 iter で 4000 × 3-要素 array (~128 KB) alloc。 全体
  ~192 MB alloc。 ノード alloc 中に `cur[2] = nxt` で next pointer を後置きで
  書き込むため、 gen backend では old→young store (WB) も発生 (cur が
  prev iter の何回かで promote されている場合)。
- **Lifetime**: 1 iter 内で全 cell live、 iter 完了で die (cons_list と同じ
  ライフサイクル)。 違いは「3-要素 node」 と「bidirectional refs」。
- **テスト対象**:
  - mark phase の pointer-count スケーリング (cons_list 1 ref/node に対し
    dll_walk は 2 refs/node)
  - `cur[2] = nxt` の WB stress (gen backend remset)
  - 3-要素 alloc (size class 32 → 40 移行コスト、 mark_gen / mark_bitmap_gen
    で slot 効率に差)
  - backward walk による reverse pointer chase の cache 挙動
- **特性的な数値**: `immix_gen` 0.76 s 最速、 多くの backend で 0.8-0.9 s。
  Oracle = `2 × Σ(0..3999) × 1500 = 23994000000`。

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
- **特性的な数値**: ベスト ~0.85-0.90 s 帯、 ワースト ~1.30 s (none)
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

#### `remset_pressure.ba.rb` — adversarial sparse old→young writes (iter 36)

- **What**: 50K cell cons chain を作って全 promote させた後、 200K iter で
  ランダム位置 (deterministic step) のセル head を新規 young array で
  上書き。 各 store が old→young write barrier を発火させる。
- **Alloc pattern**: 50K cell long-lived + 200K × 2-要素 young array =
  短命 alloc 800 K + WB push 200 K。
- **Lifetime**: 50K cells は全 long-lived。 young arrays は immediate die。
- **テスト対象**: 各 gen backend の remset 実装の質。
  - cap 128K + heap-walk fallback の発火 (`mark_gen` 系、 `copy_gen` 系)
  - `mark_card_gen` の page-level remset (自然 bounded)
  - `immix_gen` v2 の pressure-triggered minor (iter 38)
- **特性的な数値**: gen backend 全体 0.30-0.40 s、 non-gen は 0.45+ s。
  `immix_gen` 0.30 が最速。

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
- **特性的な数値**: ベスト ~0.55 s 帯 (`mark_compact_gen`
  tied)、 ワースト 2.41 s (`mark`、 後に 1.68 s に改善)。

#### `string_concat_dyn.ba.rb` — dynamic string concat (iter 37)

- **What**: `make_chunk(i)` で `i % 3` に応じた異なる literal を返す関数経由で、
  動的に 3 string を `a + b + c` で concat、 5_000_000 iter。 iter 37 で
  `node_str_lit + node_str_lit` を parse-time fold したため、 通常の
  `string_concat` は const-folded で 1-alloc/iter に縮んでしまった。 本 bench
  は関数経由で fold を回避し、 本来の "many small string allocs" pattern を
  保持する。
- **Alloc pattern**: 3 BaString alloc + 2 concat per iter = 5 alloc / iter。
  全体 ~750 MB alloc。
- **Lifetime**: 各 string は 1 iter 内で die、 nursery で完結。
- **テスト対象**: string allocator + concat の現実的なコスト。
- **特性的な数値**: `copy` 1.02 s 最速、 `immix_gen` 1.04 s 同位。 oracle=45000000。

#### `tokenize.ba.rb` — CSV-like 分割 (iter 48)

- **What**: 20 × "red,blue,green,yellow,orange,purple" を `,` で join した
  長文字列を、 各 iter で 120 tokens に分割し tokens.push でリスト化。
  17500 iter。 各 token の長さ合計を加算、 oracle = 10500000。
- **Alloc pattern**: 1 iter で約 240 個の class-0 alloc (BaString header +
  short bytes payload が各 120)、 加えて空 array → 12-128 capa への growth
  (PAYLOAD_VAL 系)。 BaString と bytes 両方が class 0 を共有するため
  freelist 再利用が頻発。
- **Lifetime**: 1 iter 内で全 tokens / array が live、 iter 完了で die。
  baruby_str_slice + baruby_ary_push の混合 hot path を exercise する初の
  bench (他 string bench は concat 中心、 substr_churn は long-lived single
  string)。
- **テスト対象**:
  - 同一 size class 内の混合 kind (OBJ_STRING / PAYLOAD_BYTE / PAYLOAD_VAL)
    の freelist 再利用が壊れていないか
  - aro_gc_alloc_byte hot path (まだ inline 候補)
  - array growth path (a->capa doubling)
- **特性的な数値**: `mark_compact_gen` 0.88 s 最速、 copy / copy_gen /
  mark_bump_gen / immix_gen は 0.89-0.91 s tied 近い。
- **iter 48 で mark_freelist の dormant memset bug を発覚**: BaString
  header 再利用時に bytes 領域が前の用途 (raw 文字列バイト) のままで、
  scan_outgoing が BaString.bytes を VALUE と解釈して SEGV した。 fix も
  iter 48 で commit (`b1050bd`)。 詳細 [done.md (48)](done.md)。

#### `substr_churn.ba.rb` — 長寿命 String を sliding window で読む

- **What**: 18 MB の 1 String (`repeat("abcdef", 3M)`) を生成、
  全 offset から 5-byte substring を切る。 ~3M substr alloc。
- **Alloc pattern**: 532 MB total (短命 substr) + 持続 18 MB (text)。
- **Lifetime**: 1 iter 短命 substr + permanent text。
- **テスト対象**: BaString slice の sp ref pattern、 long-lived + short-
  lived の混在 (gc_combined と類似だが string 軸)。
- **特性的な数値**: ベスト ~0.90 s 帯、 ワースト ~1.85 s
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
| string-heavy (concat / slice 多) | `mark_compact_gen` または `mark_bump_gen` | nursery + sp ref pattern の組合せ |
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
- **copy** (`copy` / `copy_gen`): Cheney は trace と
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
- `mark_gen_inc` を真の incremental に (stack write barrier + work budget)。
  現状は SATB infra のみ で実態は STW。
- `copy_gen_inc` (現 placeholder) に Cheney 用 incremental scan-loop を
  実装して真の incremental Cheney 化。

(iter 29 で完了: `mark_compact` / `copy` / `copy_gen` /
`mark_compact_gen` に adaptive 16 MiB threshold 追加で fair 化。 全 backend
の MIN を 16 MiB に統一。 詳細は [gc_runtime.md §6](gc_runtime.md) と
done.md iter 29。)
