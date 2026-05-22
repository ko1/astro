# baruby_precise 性能ノート

仕様は [spec.md](spec.md)、 実装は [runtime.md](runtime.md)、 残タスクは
[todo.md](todo.md)、 過去の iter 履歴は [done.md](done.md) を参照。

baruby_precise は **precise *moving* GC (semi-space) の testbed**。 16
GC backend を build-time switch で切り替えて挙動 / 性能を比較するのが
目的。 姉妹サンプル `sample/baruby` (conservative libgc) と同じテスト・
ベンチを共有して「precise rooting + 移動 GC のオーバーヘッド」を測る。

## 0. Fairness contract

iter 35 で固定した比較契約 (= 過去の不公平な測定を再発させないための
ルール集):

- **Build**: `make GC=<backend> ASTRO_DEBUG=0` で全 backend に同じ
  flags (`-O3 -flto=auto -fno-plt -march=native`)。 `ASTRO_DEBUG=0` が
  release shape の default。 dev は `make ASTRO_DEBUG=1` で opt-in。
- **Mode**: `--plain` を正本とする。 AOT (`-c`) と PG (`-p`) は補助的に
  載せる程度。
- **Repeats / policy**: 各 (backend × bench) を **median of N=3 以上**。
  best-of-N はノイズで運の影響を受けるので使わない。
- **Charging model**: 全 gen backend で `sizeof(GCHeader) + ALIGN8(payload_size)`
  を alloc-bytes trigger に統一。 payload bytes と nursery occupancy
  bytes の混在を排除。
- **Trigger threshold**: 全 gen backend の minor を統一 16 MiB
  (`mark_gen` / `mark_gen_inc` の旧 4 MiB から修正)。 major adaptive
  threshold MIN も統一 16 MiB。 major は **old growth** で発火
  (`old_alloc_since_major > major_threshold`)。
- **Header sizing**: 全 backend で GCHeader 8 B or 16 B に packing。
  BaArray (24 B payload + 8 B header) が slab class 32 に収まるように。
- **Backends excluded from matrix**: `copy_gen_inc` は実体が
  `copy_gen` の clone (inc_step / SATB なし) で「独立 algorithm」 を
  主張できないため除外。 16 backend のうち matrix runner で **15
  backend** を比較。
- **GC timer**: `aro_gc_time_begin/_end` で全 backend の collect /
  minor / major を計測。 `mark_gen_inc` の `inc_step` も同経路。
- **Runner**: `bench/matrix.rb` が canonical。 backend ごと rebuild、
  `strings` で `baruby_gc=<name>` stamp を検証、 result を
  `oracle.json` に対して checksum、 CSV + JSON + Markdown 出力。

## 1. 計測環境

| 項目 | 値 |
|---|---|
| CPU | AMD Ryzen 9 5900HX |
| OS | Linux 6.8 (x86_64) |
| Compiler | gcc 13.3.0 |
| GC (precise) | 自前 16 backend、 `gc_<name>.c` |
| GC (conservative 比較) | Boehm libgc 8.2.6 (`sample/baruby` 由来) |
| Build flags | `-O3 -flto=auto -ggdb3 -march=native -fno-plt -DASTRO_DEBUG=0` |
| Default backend | `copy` (semispace Cheney) |
| Run policy | `ruby bench/matrix.rb` — median of 3, plain mode |

⚠ **「libgc との比較」 caveat**: いま測っているのは「collector のみの
差」 ではなく **「runtime + rooting + collector の合計差」**。
baruby_precise は precise rooting (`c->env..c->sp` の flat scan) と
moving GC の組合せ、 baruby は conservative scanning。 同じ言語 /
同じベンチ / 同じ build flags だが、 数値差を「GC algorithm の差」 と
読み切るのは過剰解釈。 collector-only 比較が欲しいなら同じ runtime に
backend を差し込む設計が要る (= 別 iter)。

## 2. 最新マトリクス

`ruby bench/matrix.rb --mode plain -n 3` (iter 72 後)。 15 baruby_precise
backend + libgc 比較列。 bench は 30 種 (= GC 評価 20 + naruby-style
int 15 を合算、 一部省略)。 各 cell の数値は秒 (median of 3)、 **太字** は
その bench の最速 backend。

[bench-results/matrix.md](../bench-results/matrix.md) に最新の生データ
(json / csv 含む)。 perf.md は要約のみ。

### 2.1 plain mode matrix (iter 72、 median of 3、 15 backend + libgc × 35 bench)

`ruby bench/matrix.rb --mode plain -n 3` の出力。 **太字** は各 bench の
最速 backend。 数値は秒。 backend 列の順序は naruby-style int bench
重視の左から (= ベース → mark 系 → copy 系 → compact 系 → bump 系 →
immix 系 → bitmap/card 系 → freelist → libgc 比較)。

| Bench | none | mark | mark_gen | mark_gen_inc | copy | copy_gen | mark_compact | mark_compact_gen | bump | mark_bump_gen | immix | immix_gen | mark_bitmap_gen | mark_card_gen | mark_freelist | libgc |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| ackermann | 6.05 | 5.99 | 6.06 | 6.03 | 6.27 | 6.06 | 6.03 | 6.25 | 6.19 | 6.36 | 6.04 | 6.31 | 6.27 | 5.93 | 6.42 | **5.77** |
| ast_eval | **0.29** | 0.30 | 0.33 | 0.32 | 0.32 | 0.31 | 0.32 | 0.32 | 0.29 | 0.31 | 0.31 | 0.32 | 0.31 | 0.31 | 0.30 | 0.30 |
| binary_trees | 0.69 | 0.65 | 0.71 | 0.76 | 0.71 | 0.71 | 0.70 | 0.64 | **0.40** | 0.80 | 0.47 | 0.71 | 0.82 | 0.80 | 0.75 | 0.70 |
| branch_dom | 1.77 | 1.82 | 1.76 | 1.82 | 1.81 | 1.76 | 1.77 | 1.81 | 1.80 | 1.81 | 1.78 | 1.86 | 1.76 | 1.78 | 1.80 | **1.75** |
| call | 6.96 | 7.08 | 7.56 | 7.20 | 6.75 | 7.71 | 7.79 | 7.11 | 6.80 | 7.75 | 6.85 | 7.48 | 6.80 | 7.24 | 6.85 | **6.28** |
| chain20 | 6.44 | 6.20 | 7.15 | 7.03 | 6.45 | 6.97 | 6.87 | 6.47 | 6.67 | 7.18 | 6.18 | 7.09 | 6.29 | 6.26 | 6.13 | **5.96** |
| chain40 | 6.62 | 6.77 | 7.60 | 7.29 | 6.90 | 7.26 | 7.17 | **6.36** | 6.52 | 7.12 | 6.79 | 7.27 | 6.74 | 6.78 | 6.78 | 7.20 |
| chain_add | 1.07 | 1.14 | 1.06 | 0.98 | **0.93** | 1.03 | 0.95 | 0.97 | 1.05 | 1.14 | 1.10 | 0.98 | 1.05 | 1.04 | 0.95 | 0.98 |
| collatz | 4.72 | 4.86 | 4.88 | 5.00 | 4.70 | 5.13 | 5.02 | **4.65** | 4.74 | 4.86 | 4.78 | 4.82 | 4.89 | 4.93 | 5.01 | 5.45 |
| compose | 1.28 | 1.27 | 1.44 | 1.23 | **1.23** | 1.30 | 1.27 | 1.33 | 1.26 | 1.29 | 1.33 | 1.25 | 1.26 | 1.30 | 1.23 | 1.29 |
| cons_list | 0.95 | 0.69 | 0.77 | 0.75 | **0.59** | 0.65 | 0.72 | 0.64 | 0.72 | 0.64 | 0.60 | 0.61 | 0.70 | 0.74 | 0.61 | 0.84 |
| deep_const | 3.64 | **3.63** | 4.31 | 4.14 | 3.71 | 4.38 | 4.18 | 3.68 | 4.23 | 4.18 | 3.65 | 4.17 | 4.36 | 3.77 | 3.75 | 5.18 |
| dll_walk | 0.80 | 0.61 | 0.74 | 0.72 | **0.59** | 0.66 | 0.68 | 0.65 | 0.72 | 0.64 | 0.63 | 0.66 | 0.69 | 0.70 | 0.63 | 0.78 |
| early_return | 5.25 | 5.70 | 5.58 | 5.70 | 5.77 | 6.02 | 5.75 | 5.41 | **5.21** | 5.27 | 5.35 | 5.92 | 5.49 | 5.55 | 5.60 | 6.19 |
| fannkuch | 0.61 | 0.62 | 0.67 | 0.66 | 0.63 | **0.61** | 0.63 | 0.63 | 0.63 | 0.62 | 0.61 | 0.64 | 0.67 | 0.67 | 0.61 | 0.66 |
| fib | 5.85 | 5.63 | 5.66 | 5.83 | 5.82 | 5.72 | 5.75 | 5.86 | 5.66 | 5.67 | 5.67 | 5.82 | 5.75 | 5.80 | 5.66 | **5.49** |
| fib_pair | 1.18 | 0.75 | 0.89 | 0.90 | **0.66** | 0.69 | 0.83 | 0.69 | 0.86 | 0.69 | 0.70 | 0.67 | 0.76 | 0.82 | 0.69 | 0.88 |
| gc_combined | 1.10 | 0.71 | 0.79 | 0.80 | 0.63 | 0.66 | 0.73 | 0.66 | 0.83 | 0.66 | 0.67 | **0.62** | 0.72 | 0.73 | 0.69 | 0.81 |
| gcd | 4.21 | 4.21 | 4.07 | 4.14 | **3.90** | 4.14 | 4.06 | 3.97 | 4.09 | 4.19 | 4.05 | 4.08 | 4.12 | 4.17 | 4.03 | 3.90 |
| graph_bfs | 0.87 | 0.88 | 0.99 | 1.00 | 0.87 | 0.87 | **0.84** | 0.93 | 0.91 | 0.89 | 0.92 | 0.99 | 0.99 | 0.96 | 0.89 | 0.84 |
| hash_chain | 0.90 | 0.91 | 0.93 | 0.93 | 0.89 | 0.91 | 0.94 | 0.91 | **0.89** | 0.98 | 0.89 | 0.93 | 0.93 | 0.91 | 0.94 | 1.12 |
| interp_calc | 1.04 | 0.77 | 0.85 | 0.87 | 0.73 | 0.79 | 0.87 | 0.77 | 0.88 | 0.78 | 0.73 | **0.73** | 0.82 | 0.80 | 0.80 | 0.82 |
| json_parse | 1.43 | 0.81 | 0.87 | 0.94 | 0.70 | 0.71 | 0.92 | 0.70 | 1.05 | **0.69** | 0.75 | 0.71 | 0.79 | 0.77 | 0.78 | 0.98 |
| life | 1.16 | 1.19 | 1.20 | 1.17 | 1.15 | 1.17 | 1.17 | 1.16 | 1.21 | 1.21 | **1.13** | 1.20 | 1.23 | 1.18 | 1.20 | — |
| list_alloc | 1.04 | 0.68 | 0.75 | 0.76 | 0.57 | 0.64 | 0.71 | 0.63 | 0.83 | 0.63 | 0.62 | **0.57** | 0.66 | 0.69 | 0.68 | 0.76 |
| list_sort | 1.01 | 1.03 | 1.09 | 1.07 | **0.90** | 0.94 | 0.93 | 0.94 | 1.02 | 0.91 | 0.92 | 0.92 | 1.10 | 1.11 | 1.01 | 0.99 |
| loop | 1.25 | 1.27 | 1.23 | 1.27 | 1.29 | 1.27 | 1.25 | 1.25 | 1.25 | **1.23** | 1.23 | 1.27 | 1.29 | 1.25 | 1.23 | 1.28 |
| nqueens | 0.77 | 0.81 | 0.85 | 0.82 | 0.79 | **0.77** | 0.80 | 0.80 | 0.79 | 0.79 | 0.81 | 0.79 | 0.81 | 0.84 | 0.82 | 0.79 |
| prime_count | 18.44 | 18.48 | 19.17 | 19.55 | 18.85 | 19.96 | 19.20 | 18.68 | 19.14 | 18.62 | 18.56 | 19.99 | **18.36** | 18.62 | 18.70 | 18.62 |
| remset_pressure | 0.41 | 0.31 | 0.31 | 0.33 | 0.31 | 0.27 | 0.35 | 0.28 | 0.33 | 0.26 | 0.27 | **0.26** | 0.29 | 0.29 | 0.29 | 0.36 |
| sieve | 1.08 | **1.03** | 1.17 | 1.17 | 1.05 | 1.21 | 1.11 | 1.31 | 1.17 | 1.19 | 1.07 | 1.21 | 1.17 | 1.17 | 1.08 | 1.09 |
| string_concat | 0.38 | 0.22 | 0.22 | 0.25 | 0.18 | 0.18 | 0.25 | 0.18 | 0.25 | 0.17 | 0.20 | **0.17** | 0.21 | 0.21 | 0.21 | 0.25 |
| string_concat_dyn | 1.51 | 0.92 | 0.96 | 1.04 | 0.82 | 0.82 | 1.03 | 0.81 | 1.06 | **0.79** | 0.82 | 0.82 | 0.84 | 0.85 | 0.88 | 1.08 |
| substr_churn | 1.07 | 0.76 | 0.80 | 0.82 | 0.71 | 0.69 | 0.79 | 0.67 | 0.85 | 0.65 | 0.71 | **0.65** | 0.70 | 0.71 | 0.74 | 0.93 |
| tak | 0.64 | 0.64 | 0.63 | 0.64 | 0.63 | 0.65 | 0.65 | 0.66 | 0.64 | 0.62 | 0.67 | 0.66 | 0.64 | 0.65 | 0.64 | **0.61** |
| tokenize | 1.31 | 0.75 | 0.84 | 0.90 | **0.64** | 0.66 | 0.94 | 0.66 | 0.95 | 0.65 | 0.72 | 0.65 | 0.74 | 0.74 | 0.74 | 0.91 |

生データ + CSV / JSON は [bench-results/iter72/](../bench-results/iter72/)。
`life` の libgc 列だけ `—` なのは naruby ベースの parser/main の
toplevel return value bug (GC 非関連、 別件)。

### 2.2 AOT mode matrix (iter 72、 median of 3、 15 backend + libgc × 35 bench)

`ruby bench/matrix.rb --mode aot -n 3` の出力。 AOT は `-c` モードで
parser が `code_store/all.so` に SD (specialized dispatcher) を gcc で
bake し、 2 回目以降の run が dlopen して bind する仕組み。

| Bench | none | mark | mark_gen | mark_gen_inc | copy | copy_gen | mark_compact | mark_compact_gen | bump | mark_bump_gen | immix | immix_gen | mark_bitmap_gen | mark_card_gen | mark_freelist | libgc |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| ackermann | 1.26 | 1.22 | 1.23 | 1.26 | 1.26 | 1.23 | **1.21** | 1.22 | 1.27 | 1.25 | 1.23 | 1.25 | 1.24 | 1.24 | 1.25 | 5.82 |
| ast_eval | 0.05 | 0.05 | 0.05 | 0.06 | 0.06 | 0.06 | 0.06 | 0.06 | **0.05** | 0.06 | 0.05 | 0.05 | 0.06 | 0.06 | 0.05 | 0.31 |
| binary_trees | 0.41 | 0.45 | 0.52 | 0.52 | 0.53 | 0.53 | 0.48 | 0.45 | **0.21** | 0.62 | 0.28 | 0.49 | 0.59 | 0.59 | 0.47 | 0.70 |
| branch_dom | **0.19** | 0.20 | 0.19 | 0.19 | 0.19 | 0.19 | 0.19 | 0.19 | 0.19 | 0.19 | 0.19 | 0.19 | 0.20 | 0.20 | 0.20 | 1.74 |
| call | 1.51 | 1.52 | **1.48** | 1.52 | 1.52 | 1.48 | 1.53 | 1.48 | 1.48 | 1.49 | 1.52 | 1.48 | 1.53 | 1.52 | 1.52 | 5.98 |
| chain20 | 1.60 | 1.61 | 1.60 | 1.67 | 1.65 | 1.64 | 1.61 | 1.65 | 1.65 | **1.60** | 1.61 | 1.61 | 1.61 | 1.62 | 1.64 | 5.73 |
| chain40 | 2.77 | 2.81 | **2.74** | 2.81 | 2.82 | 2.82 | 2.75 | 2.75 | 2.80 | 2.75 | 2.80 | 2.74 | 2.75 | 2.82 | 2.81 | 6.76 |
| chain_add | 0.28 | 0.29 | 0.29 | 0.28 | 0.29 | 0.29 | 0.28 | 0.28 | 0.28 | 0.28 | 0.29 | 0.29 | 0.29 | 0.29 | **0.28** | 1.00 |
| collatz | **0.31** | 0.31 | 0.31 | 0.31 | 0.31 | 0.31 | 0.31 | 0.32 | 0.32 | 0.31 | 0.32 | 0.31 | 0.31 | 0.31 | 0.32 | 4.51 |
| compose | 0.22 | **0.22** | 0.22 | 0.22 | 0.22 | 0.22 | 0.22 | 0.23 | 0.22 | 0.22 | 0.23 | 0.22 | 0.22 | 0.22 | 0.22 | 1.16 |
| cons_list | 0.53 | 0.24 | 0.29 | 0.35 | 0.18 | 0.19 | 0.30 | 0.18 | 0.32 | 0.18 | 0.18 | **0.17** | 0.29 | 0.27 | 0.22 | 0.71 |
| deep_const | 1.20 | **1.20** | 1.24 | 1.23 | 1.20 | 1.24 | 1.21 | 1.25 | 1.24 | 1.24 | 1.22 | 1.21 | 1.24 | 1.23 | 1.21 | 4.50 |
| dll_walk | 0.35 | 0.17 | 0.21 | 0.25 | 0.14 | 0.15 | 0.22 | 0.15 | 0.23 | 0.15 | **0.14** | 0.14 | 0.21 | 0.21 | 0.16 | 0.70 |
| early_return | 0.32 | 0.32 | 0.32 | 0.32 | 0.31 | 0.31 | 0.32 | 0.32 | 0.31 | 0.32 | 0.32 | 0.31 | **0.31** | 0.32 | 0.31 | 5.18 |
| fannkuch | 0.12 | 0.12 | 0.13 | 0.14 | 0.15 | 0.10 | 0.16 | **0.10** | 0.15 | 0.10 | 0.12 | 0.13 | 0.15 | 0.14 | 0.14 | 0.59 |
| fib | 1.18 | 1.15 | 1.11 | 1.11 | 1.15 | 1.12 | 1.14 | 1.15 | 1.17 | 1.13 | **1.10** | 1.12 | 1.11 | 1.11 | 1.12 | 5.28 |
| fib_pair | 0.79 | 0.33 | 0.38 | 0.47 | 0.25 | **0.24** | 0.42 | 0.24 | 0.47 | 0.24 | 0.26 | 0.25 | 0.35 | 0.35 | 0.31 | 0.89 |
| gc_combined | 0.60 | 0.27 | 0.30 | 0.36 | 0.22 | 0.18 | 0.35 | 0.18 | 0.40 | **0.17** | 0.21 | 0.20 | 0.27 | 0.28 | 0.28 | 0.77 |
| gcd | 0.28 | **0.28** | 0.29 | 0.28 | 0.28 | 0.28 | 0.29 | 0.29 | 0.28 | 0.28 | 0.28 | 0.29 | 0.28 | 0.28 | 0.29 | 3.79 |
| graph_bfs | **0.17** | 0.22 | 0.31 | 0.30 | 0.19 | 0.20 | 0.17 | 0.20 | 0.25 | 0.20 | 0.22 | 0.27 | 0.31 | 0.30 | 0.21 | 0.86 |
| hash_chain | 0.14 | 0.14 | 0.15 | 0.15 | 0.16 | 0.15 | 0.16 | 0.16 | **0.13** | 0.16 | 0.13 | 0.13 | 0.15 | 0.16 | 0.16 | 1.02 |
| interp_calc | 0.50 | 0.25 | 0.30 | 0.34 | 0.21 | 0.23 | 0.34 | 0.24 | 0.37 | 0.24 | 0.22 | **0.21** | 0.28 | 0.28 | 0.24 | 0.80 |
| json_parse | 1.03 | 0.44 | 0.51 | 0.58 | 0.34 | 0.33 | 0.57 | 0.33 | 0.68 | **0.33** | 0.38 | 0.33 | 0.41 | 0.41 | 0.40 | 0.99 |
| life | 0.14 | 0.15 | 0.16 | 0.16 | 0.14 | 0.13 | 0.14 | 0.13 | 0.14 | 0.13 | **0.13** | 0.13 | 0.17 | 0.18 | 0.15 | — |
| list_alloc | 0.61 | 0.27 | 0.29 | 0.35 | 0.18 | 0.17 | 0.32 | 0.18 | 0.44 | 0.17 | 0.18 | **0.16** | 0.28 | 0.26 | 0.25 | 0.75 |
| list_sort | 0.31 | 0.30 | 0.33 | 0.36 | **0.19** | 0.20 | 0.24 | 0.19 | 0.30 | 0.20 | 0.20 | 0.21 | 0.35 | 0.35 | 0.33 | 0.98 |
| loop | 0.10 | 0.10 | 0.09 | **0.09** | 0.10 | 0.09 | 0.10 | 0.10 | 0.09 | 0.09 | 0.09 | 0.09 | 0.10 | 0.09 | 0.10 | 1.24 |
| nqueens | 0.08 | 0.09 | 0.09 | 0.09 | 0.08 | 0.07 | 0.07 | 0.07 | 0.07 | 0.07 | 0.07 | **0.07** | 0.09 | 0.09 | 0.08 | 0.81 |
| prime_count | 0.51 | 0.51 | 0.51 | 0.51 | 0.51 | 0.53 | 0.53 | 0.51 | 0.53 | **0.51** | 0.53 | 0.51 | 0.52 | 0.53 | 0.53 | 18.40 |
| remset_pressure | 0.22 | 0.12 | 0.12 | 0.15 | 0.13 | 0.08 | 0.16 | 0.08 | 0.14 | 0.08 | 0.10 | **0.07** | 0.10 | 0.10 | 0.11 | 0.37 |
| sieve | **0.23** | 0.26 | 0.37 | 0.39 | 0.58 | 0.40 | 0.59 | 0.39 | 0.63 | 0.37 | 0.58 | 0.35 | 0.40 | 0.40 | 0.56 | 1.09 |
| string_concat | 0.26 | 0.11 | 0.12 | 0.14 | 0.09 | 0.07 | 0.14 | 0.07 | 0.14 | 0.07 | 0.08 | **0.06** | 0.10 | 0.11 | 0.10 | 0.26 |
| string_concat_dyn | 0.88 | 0.37 | 0.44 | 0.50 | 0.30 | 0.28 | 0.51 | 0.28 | 0.55 | 0.28 | 0.31 | **0.27** | 0.34 | 0.35 | 0.33 | 1.07 |
| substr_churn | 0.57 | 0.28 | 0.29 | 0.34 | 0.30 | **0.19** | 0.39 | 0.20 | 0.47 | 0.20 | 0.27 | 0.24 | 0.23 | 0.25 | 0.31 | 0.91 |
| tak | 0.18 | 0.17 | 0.18 | 0.18 | 0.17 | 0.17 | 0.17 | 0.17 | 0.17 | 0.17 | 0.17 | **0.17** | 0.17 | 0.17 | 0.17 | 0.61 |
| tokenize | 0.97 | 0.41 | 0.48 | 0.58 | 0.32 | 0.31 | 0.57 | 0.32 | 0.62 | 0.31 | 0.35 | **0.30** | 0.40 | 0.39 | 0.39 | 0.90 |

libgc は AOT 未対応 (= matrix runner が `-c` 引数を渡しても baruby
binary は plain 動作する) ので、 libgc 列は plain と同等の値。 列としては
保持して plain → AOT がどれだけ伸びるかを `--/<libgc>` で見やすくする
ために使う。

### 2.2.1 plain → AOT speedup (主要 bench、 copy backend)

| Bench | plain | AOT | speedup |
|---|---:|---:|---|
| prime_count | 18.85 | 0.51 | **37×** |
| collatz | 4.70 | 0.31 | **15×** |
| branch_dom | 1.81 | 0.19 | **9.5×** |
| early_return | 5.77 | 0.31 | **18.6×** |
| chain20 | 6.45 | 1.65 | 3.9× |
| chain40 | 6.90 | 2.82 | 2.4× |
| ackermann | 6.27 | 1.26 | 5.0× |
| fib | 5.82 | 1.15 | 5.1× |
| call | 6.75 | 1.52 | 4.4× |
| binary_trees | 0.71 | 0.53 | 1.3× |
| cons_list | 0.59 | 0.18 | **3.3×** |
| dll_walk | 0.59 | 0.14 | **4.2×** |
| list_alloc | 0.57 | 0.18 | **3.2×** |
| string_concat_dyn | 0.82 | 0.30 | **2.7×** |
| tokenize | 0.64 | 0.32 | 2.0× |
| sieve | 1.05 | 0.58 | 1.8× |

AOT win が大きいのは「dispatch コスト dominant」 な int bench
(prime_count / collatz / early_return / fib_pair etc.)。 SD bake で
dispatcher の indirect call が直接 jump に化けるため。 alloc-heavy
bench でも 2-4× 速くなる (= 約 alloc 部分以外が SD で消える)。

binary_trees / sieve など「GC が支配的」 な bench は AOT で
2 倍ぐらいまでしか伸びない (= GC コストは SD では消えない)。

### 2.3 観察まとめ (iter 72 plain matrix)

**naruby-style int bench (alloc 無し、 dispatch コスト dominant)** —
ackermann / branch_dom / call / chain20 / chain40 / chain_add / collatz
/ compose / deep_const / early_return / fib / fib_pair / gcd / loop /
prime_count / tak。 ほぼ全 backend が同水準 (= GC algorithm は
発火しないので dispatch コストのみ)。 libgc 列も近い。 `mark` /
`copy` / `mark_compact` / `bump` の最速グループが多い (= bump alloc
の単純さ)。

**alloc-heavy bench (= 1 行目 dispatch + alloc cost、 大半 short-lived)**:

- `cons_list` / `dll_walk` / `fib_pair` / `gc_combined` / `interp_calc`
  / `list_alloc`: **gen backend が勝つ** (immix_gen / copy_gen /
  mark_compact_gen)。 nursery で完結する short alloc が大半なので
  minor で済む形。 libgc は 30-60% 遅い。
- `tokenize` / `string_concat_dyn` / `substr_churn` / `json_parse`:
  **mark_bump_gen / mark_compact_gen / immix_gen が強い**。 string +
  bytes alloc が頻発するため compact / bump で再利用が効く。 libgc は
  20-40% 遅い。
- `binary_trees` (= long-lived heap 大半): **bump が最速** (= 0.40 で
  圧倒的、 GC が走らない 1-pass alloc)、 次点 immix (0.47)。 gen 系は
  promote コストで遅い (mark_bump_gen 0.80)。 libgc 0.70 も健闘。
- `sieve` (= 1 つの巨大 long-lived alloc + sweep stress): **mark が
  最速** (1.03)。 iter 67-69 の realloc_in_place が効いている。
  immix_gen は 1.21 で若干遅め。
- `remset_pressure` (= old→young WB 多発): **immix_gen 最速** (0.26)、
  続いて mark_bump_gen (0.26) / copy_gen (0.27)。 iter 38 で全 gen
  backend に remset cap + heap-walk fallback を入れた効果。
- `life`: 全 backend ほぼ tie (1.13-1.23)。 mutator-bound で alloc 軽め。

**no GC = `bump` の限界**: long-lived heavy (binary_trees) は速いが、
heap が膨らむ workload (list_alloc / cons_list 等) では未回収のせいで
cache miss が増えてむしろ遅い。 alloc-pattern によって win/lose が
極端に分かれる。

**libgc との対比** (= precise rooting + 移動 GC vs conservative scan):
- libgc が勝つ: 数値 dispatch only (= ackermann / branch_dom / call /
  chain20 / fib / tak)。 GC が発火しないので rooting overhead だけ差
  になり、 precise 側がやや負ける (= sp scan の constant cost)。
- baruby_precise が勝つ: alloc-heavy bench (= cons_list / fib_pair /
  json_parse / tokenize / substr_churn 等)。 gen GC + bump alloc が効く。
  特に generational backends は libgc 比 20-60% 速い。

詳細な per-backend 解説は §4。

## 3. マクロベンチカタログ

GC 評価向け 20 個 + naruby int 15 個。 アルファベット順、 主な軸を
1 行サマリ:

| bench | alloc pattern | lifetime | 主に exercise する点 |
|---|---|---|---|
| ackermann | int のみ (alloc 無) | n/a | 再帰 + dispatch cost |
| ast_eval | AST build + intermediate | 2 層 (long + short) | gen 効果、 deep mark cost |
| binary_trees | 2-要素 BaArray ×2M | 長寿命 (構築中 live) | mark/sweep walk、 Cheney copy |
| branch_dom | int 制御 | n/a | branch prediction |
| call | int + 関数呼び出し | n/a | call dispatch overhead |
| chain20 / chain40 | int 連鎖 | n/a | chain 解析 |
| chain_add | int 連鎖 add | n/a | binop dispatch |
| collatz | int 操作 | n/a | int dispatch |
| compose | function composition | n/a | call chain |
| cons_list | 5000 cells × 2000 iter | 1 iter で die | deep alloc chain |
| deep_const | const lookup | n/a | const ID dispatch |
| dll_walk | 4000 × 3-要素 BaArray | 1 iter 内 live | WB stress、 3-要素 alloc |
| early_return | 関数早期 return | n/a | call/return |
| fannkuch | 順列 enumerate | 短命 | mutator-bound |
| fib / fib_pair | 再帰 frame | LIFO 短命 | nursery 完結率 |
| gc_combined | long permanent + short churn | 2 層 | gen benefit、 remset |
| gcd | int 連鎖 | n/a | int dispatch |
| graph_bfs | working set | medium | BFS + visited array |
| hash_chain | bucket hash | 3 層 | WB heavy、 chain realloc |
| interp_calc | AST build + eval | 短命 burst | gen の burst→静止 |
| json_parse | 再帰下降 parser | 短命 | recursion + alloc 密 |
| life | Conway grid × 200 ticks | tick lifetime | mutator dominant |
| list_alloc | pure 4-要素 alloc | 1 iter | alloc throughput |
| list_sort | merge sort burst | recursion 短命 | 中規模 burst |
| loop | int loop | n/a | loop overhead |
| nqueens | backtrack array copy | LIFO 短命 | deep recursion |
| prime_count | int 連鎖 | n/a | int divmod |
| remset_pressure | sparse old→young writes | mixed | gen remset 実装 |
| sieve | 1 大配列 + sweep | 全 long-lived | 大 alloc + sweep stress |
| string_concat | small str concat hot loop | 1 iter | String alloc + bytes |
| string_concat_dyn | dynamic str concat (parse fold 回避) | 1 iter | 実 string alloc cost |
| substr_churn | long str + slice churn | 2 層 | BaString slice |
| tak | 3-arg 再帰 | n/a | call_3 path |
| tokenize | CSV split | 1 iter | substr + array growth |

oracle (各 bench の期待結果) は [bench/oracle.json](../bench/oracle.json)
で matrix runner が checksum 検証する。

## 4. backend 比較 + 選択ガイド

### 4.1 backend 列

| backend | algorithm | 適性 workload | 特徴 |
|---|---|---|---|
| `none` | libc malloc (no GC) | alloc cost baseline | 全 ptr leak、 OOM 時 abort |
| `mark` | mark+sweep (non-gen) | long-lived heavy | per-obj malloc、 簡素 |
| `mark_gen` | mark+sweep + young/old | short alloc 大半 | bump young + linked old |
| `mark_gen_inc` | mark_gen + SATB infra | latency 重視 (要 budget) | iter 38 で SATB のみ、 真の inc は budget が SIZE_MAX |
| `mark_bitmap_gen` | mark+sweep、 page-level mark bitmap | 8 B header workload | per-page mark + 同 page sweep |
| `mark_card_gen` | mark_gen + card-based remset | sparse WB workload | page-level remset で bounded |
| `mark_freelist` | mark+sweep + freelist | medium alloc rate | per-class freelist |
| `mark_compact` | mark + slide compact (non-gen) | long-lived + 圧縮要 | in-place、 freelist 不要 |
| `mark_compact_gen` | mark_compact + young | string-heavy | nursery + compact tenured |
| `mark_bump_gen` | mark+sweep + bump young | mid-burst alloc | tenured bump (compactor 無) |
| `copy` | Cheney semispace (non-gen) | long-lived heavy | 半空間 swap、 単純 |
| `copy_gen` | Cheney + young/old | mixed lifetime | 2× region |
| `copy_gen_inc` | (placeholder = copy_gen) | n/a (matrix 除外) | inc_step 未実装 |
| `bump` | bump alloc only (leak) | alloc throughput 上限 | floor 用、 実用不可 |
| `immix` | region+block mark (non-gen) | mid-large heap | region-bump |
| `immix_gen` | immix + young/old | broad-strong | line-based reclamation |

`copy_gen_inc` は matrix から除外 (= 「独立 algorithm」 を主張できない
ため、 honesty)。 残 15 backend が比較対象。

### 4.2 ワークロード→ backend 選択ガイド

| パターン | 推奨 | 理由 |
|---|---|---|
| 短命 alloc 多 (大半 nursery 完結) | `mark_compact_gen` / `copy_gen` / `immix_gen` | minor scan only |
| 長寿命 heap 大半 (binary_trees 等) | `copy` / `mark_compact` | gen の overhead 不要 |
| string-heavy | `mark_compact_gen` / `mark_bump_gen` | SSO + compact 再利用 |
| 仮想空間節約 | `mark_compact_gen` | tenured 1× (vs copy 2×) |
| latency 上限が要件 | `mark_gen_inc` (要 budget 設定) | inc で pause 分割 |
| pure alloc throughput 測定 | `bump` (leak) / `none` | GC 抜きの floor |

default は `copy`。 全 backend が 16 BACK × 8 T_*.ba.rb × 2 (plain +
stress) = **256/256 PASS**。

### 4.3 GC 時間 / pause 計測

`BARUBY_GC_STATS=1` で各 backend が mutator / GC 時間を分けて出す。
`gc_seconds` / `gc_pct` / `max_pause_ms` / `mark_seconds` /
`reclaim_seconds` の 5 値。 minor → major の re-entrant ケースは
depth guard で最外側だけ計上。

phase semantics:

| backend kind | mark | reclaim |
|---|---|---|
| mark&sweep (`mark` / `mark_gen` / `mark_gen_inc` / `mark_bitmap_gen`) | root scan + gray queue | sweep |
| mark&compact (`mark_compact` / `mark_compact_gen` / `mark_bump_gen`) | trace | forward + update_pointers + slide |
| copy (`copy` / `copy_gen`) | (Cheney は trace と relocate が交錯した単一 loop) = 0 | 全部 reclaim 計上 |

例 (binary_trees, plain, iter 17 計測):

| backend | max_pause_ms | meaning |
|---|---|---|
| `mark_gen` | 288.55 | 単一の major sweep が支配 |
| `mark_gen_inc` | 53.84 | inc で start / finish_sweep 分割 |
| `copy_gen` | 17.62 | 各 minor、 major なし |
| `mark_bump_gen` | 54.98 | major (promote + sweep) |

mark_gen vs mark_gen_inc で max_pause が **5.4× 差**。 latency 重視
workload では mark_gen_inc が選択肢に上がる。

## 5. 主な最適化履歴 (iter 範囲別)

詳細は [done.md](done.md)。 perf.md は要点のみ。

| iter | 内容 | 効果 |
|---|---|---|
| 36 | array literal 1-shot (`node_ary_lit_N`) | plain -9〜-12% (fib_pair / gc_combined / list_alloc) |
| 37 | str literal `+` の parse-time fold | string_concat plain -58%, AOT -79% |
| 38 | remset cap + heap-walk fallback | gen backend の adversarial fix |
| 41 | mark_freelist 追加 (16 番目 backend) | medium-rate hash_chain で 0.96 → 0.79 |
| 53 | SSO (BSTR_SSO_MAX=7) | tokenize -17%、 substr_churn -2% |
| 58 | @child operand 全面導入 | dispatcher convention 統一 |
| 61 | fp 引数削除 (3-arg dispatcher) | prime_count -89%、 関数呼出系 -50〜-68% |
| 65 | `aro_gc_fini` で全 backend clean shutdown | exit-time validity |
| 67-69 | LargeObj realloc_in_place (realloc(3) / mremap) | sieve / hash_chain -11〜-21% (10 backend) |
| 71 | call_N args @child 化 (per-body self-contained) | walker から callee_locals_cnt 依存除去 |
| 72 | walker 削除 + parse-time bake + noinline/cold | plain geomean -3.02% (vs iter 71) |
| 73 | baruby_ary_push fast-path inline + AOT bake で endbr64 (CET) 削除 | sieve copy AOT **-8.6%** (= 0.58→0.53s) |

iter 61 + 72 の dispatcher / parse 側 architecture 簡素化が大きい。
iter 67-69 の `realloc_in_place` が GC 側の最後の大きな win。 iter 73
は ABI-layer の micro-opt (= SD 内 i-cache 効率と関数 prologue 削減)。

## 6. 既知の問題

- **stress mode (`BARUBY_GC_STRESS=1`) の resource limit**:
  - `gc_copy`: 全 minor で from-space を `PROT_NONE + MADV_DONTNEED` で
    恒久 retire。 約 65k 回 GC で `/proc/sys/vm/max_map_count` を
    使い果たして `mmap: Cannot allocate memory` で abort。 長 bench で
    max_map_count を上げるか retire の circular buffer 化が要。
  - `gc_mark_bump_gen`: tenured 側 compactor 無し。 long-live old が
    溜まると tenured 64 GiB virtual を使い切る (design limit、
    `mark_compact_gen` を使えば回避)。
- **mark family の `hash_chain` slab locality**: BaArray と items[] が
  別 page で cache miss。 `mark_bitmap_gen` で 8 B header + class 32
  同梱は確認したが、 24 B header の既存系は構造改変が必要。
- **AOT で `aro_gc_realloc_in_place` を呼ぶ場合**: SD 越しに動作する
  ことを iter 68 で確認 (= 全 15 backend × 主要 bench 動作)。
- **graph_bfs で gen backend が遅い**: BFS の visited / queue が
  nursery threshold (16 MiB) より大きく、 minor 中に promote されて
  しまう。 mutator-bound 寄りで gen benefit が薄い。

## 7. 次の段階

- **Immix v2 opportunistic evacuation** (fragmentation 解消)
- **mark_bitmap_gen minor sweep 最適化**: 64-bit-wise old_bm scan、
  per-page "all old" flag で binary_trees regression 縮小
- **`mark_gen_inc` を真の incremental に**: stack write barrier +
  work budget 実装 (現在は SATB infra のみで実態は STW)
- **`copy_gen_inc` を真の incremental Cheney に**: scan-loop の
  incremental step 実装
- **`gc.c` / `gc.h` を `runtime/` に格上げ**: root mechanism (sp[] flat
  scan) + semi-space を framework backend として汎用化
- **PGO** (`-fprofile-use`) で LTO layout 最適化: iter 72 で LTO
  layout artifact が観測されたため、 PGO で hot dispatcher 配置を
  確定させると効くはず
