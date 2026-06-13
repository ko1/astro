# baruby_precise 性能ノート

仕様は [spec.md](spec.md)、 実装は [runtime.md](runtime.md)、 残タスクは
[todo.md](todo.md)、 過去の iter 履歴は [done.md](done.md) を参照。 GC
backend のアルゴリズム解説は root の
[`docs/gc_runtime.md`](../../../docs/gc_runtime.md) に集約済 (全 sample 共通)。

baruby_precise は **precise *moving* GC (semi-space) の testbed**。 **15 個
の precise GC backend** (= 11 実用 + 4 特殊用途) を build-time switch で
切り替えて挙動 / 性能を比較するのが目的。 姉妹サンプル `sample/baruby`
(conservative libgc) と同じテスト・ベンチを共有して「precise rooting +
移動 GC のオーバーヘッド」 を測る。

## 0. Fairness contract

iter 35 で固定した比較契約 (= 過去の不公平な測定を再発させないための
ルール集):

- **Build**: `make GC=<backend> ASTRO_DEBUG=0` で全 backend に同じ
  flags (`-O3 -flto=auto -fno-plt -march=native`)。 `ASTRO_DEBUG=0` が
  release shape の default。 dev は `make ASTRO_DEBUG=1` で opt-in
- **Mode**: `--plain` を正本とする。 AOT (`--aot-compile --run`) と PG
  (`--pg-compile`) は補助的に載せる程度
- **Repeats / policy**: 各 (backend × bench) を **median of N=3**。
  best-of-N はノイズで運の影響を受けるので使わない
- **計測ツール**: `/usr/bin/time -f "%e %M"` で wallclock 秒 + peak RSS
  (KiB) を同時取得。 elapsed の median と RSS の max を採用
- **Charging model**: 全 gen backend で `sizeof(GCHeader) + ALIGN8(payload_size)`
  を alloc-bytes trigger に統一
- **Trigger threshold**: 全 gen backend の minor を統一 16 MiB。 major
  adaptive threshold MIN も統一 16 MiB
- **Header sizing**: 全 backend で GCHeader 8 B or 16 B に packing
- **Backends excluded from matrix**: `copy_gen_inc` は commit `e60fa150` で
  削除済 (= `copy_gen` の clone で独立 algorithm 主張不可)。 `copy_scramble`
  は audit 用 backend、 production 比較からは除外。 残 **15 backend** が
  matrix の対象
- **GC timer**: `aro_gc_time_begin/_end` で全 backend の collect / minor /
  major を計測
- **Runner**: `bench/matrix.rb` が canonical。 backend ごと rebuild、
  `strings` で `aro_gc=<name>` stamp を検証、 result を `oracle.json` に
  対して checksum、 CSV + JSON + Markdown 出力

## 1. 計測環境

| 項目 | 値 |
|---|---|
| CPU | AMD Ryzen 9 5900HX |
| OS | Linux 6.8 (x86_64) |
| Compiler | gcc 13.3.0 |
| GC (precise) | 自前 15 backend (= 11 実用 + 4 特殊用途、 `runtime/precise_gc/gc_<name>.c`) |
| GC (conservative 比較) | Boehm libgc 8.2.6 (`sample/baruby` 由来) |
| Build flags | `-O3 -flto=auto -ggdb3 -march=native -fno-plt -DASTRO_DEBUG=0` |
| Default backend | `copy` (semispace Cheney) |
| Run policy | `ruby bench/matrix.rb` — median of 3 elapsed + max of 3 RSS、 plain mode |

backend カタログ (= **15 個**、 詳細は
[`../../../docs/gc_runtime.md`](../../../docs/gc_runtime.md)):

| backend | algorithm | doc § |
|---|---|---|
| `mark` | mark+sweep (non-gen) | §2.1 |
| `mark_gen` | mark+sweep + young/old | §2.2 |
| `mark_gen_inc` | mark_gen + SATB infra | §2.3 |
| `copy` | Cheney semispace (non-gen) | §2.4 |
| `copy_gen` | Cheney + young/old | §2.5 |
| `mark_compact` | mark + slide compact | §2.6 |
| `mark_compact_gen` | mark_compact + young | §2.7 |
| `immix` | region+block mark | §2.8 |
| `immix_gen` | immix + young/old | §2.9 |
| `mark_bitmap_gen` | mark+sweep + page bitmap | §2.10 |
| `mark_card_gen` | mark_gen + card remset | §2.11 |
| `none` † | libc malloc, no GC | §3.1 |
| `bump` † | bump alloc only (leak) | §3.2 |
| `mark_bump_gen` † | bump tenured testbed | §3.3 |
| `mark_freelist` † | freelist no-coalesce testbed | §3.4 |

† は **特殊用途 testbed** (= 実用 GC ではなく、 baseline / fragmentation 観察用)。
`copy_scramble` (§3.5) は audit 専用で matrix 除外。

⚠ **「libgc との比較」 caveat**: いま測っているのは「collector のみの
差」 ではなく **「runtime + rooting + collector の合計差」**。
baruby_precise は precise rooting (`c->env..c->sp` の flat scan) と
moving GC の組合せ、 baruby は conservative scanning。 同じ言語 /
同じベンチ / 同じ build flags だが、 数値差を「GC algorithm の差」 と
読み切るのは過剰解釈。

## 2. plain mode matrix (2026-05-26、 median of 3、 15 backend + libgc × 36 bench)

`ruby bench/matrix.rb --mode plain -n 3` の出力 (= 35 bench + libgc 列、
**太字** は各 bench の最速 backend)。 単位 秒。

### 2.1 elapsed (seconds, median of 3)

| Bench | libgc | none | mark | mark_gen | mark_gen_inc | copy | copy_gen | mark_compact | mark_compact_gen | bump | mark_bump_gen† | immix | immix_gen | mark_bitmap_gen | mark_card_gen | mark_freelist† |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| ackermann | 7.28 | 8.39 | 7.95 | 7.88 | 7.86 | 6.40 | 6.49 | 6.35 | 6.23 | 7.59 | 6.22 | 6.23 | 6.63 | 6.46 | 6.41 | **6.08** |
| ast_eval | 0.36 | 0.42 | 0.40 | 0.41 | 0.40 | 0.32 | 0.33 | 0.34 | 0.32 | 0.32 | 0.35 | 0.32 | 0.34 | 0.36 | 0.35 | **0.31** |
| binary_trees | 0.88 | 0.70 | 0.78 | 1.42 | 1.33 | 0.59 | 0.98 | FAIL | 1.70 | **0.39** | 0.95 | 0.48 | 1.06 | 1.20 | 1.16 | 0.70 |
| branch_dom | 2.05 | 2.26 | 2.35 | 2.42 | 2.25 | 1.93 | 1.89 | 1.93 | 1.89 | 1.89 | 1.88 | 1.88 | 1.93 | 1.91 | **1.83** | 1.83 |
| call | 7.48 | 9.93 | 9.11 | 9.25 | 9.19 | 8.13 | 8.35 | 6.94 | 7.55 | 7.29 | 7.88 | 7.81 | 7.70 | 7.66 | **6.86** | 7.19 |
| chain20 | 7.57 | 9.18 | 8.26 | 8.46 | 8.30 | 7.28 | 7.76 | 6.83 | 7.74 | 6.75 | 6.67 | 7.39 | 7.06 | 7.36 | **6.26** | 6.62 |
| chain40 | 8.52 | 9.21 | 9.10 | 9.80 | 7.36 | 7.84 | 7.73 | 7.43 | 7.34 | 6.99 | 7.49 | 7.75 | 7.13 | 7.69 | **6.84** | 7.11 |
| chain_add | 1.24 | 1.46 | 1.48 | 1.34 | 1.13 | 1.10 | 1.23 | **0.99** | 1.12 | 1.11 | 1.08 | 1.10 | 1.16 | 1.05 | 1.05 | 1.10 |
| collatz | 6.36 | 6.66 | 6.54 | 6.41 | 5.62 | 5.41 | 5.38 | **5.07** | 5.24 | 5.10 | 5.32 | 5.23 | 5.31 | 5.21 | 5.17 | 5.07 |
| compose | 1.49 | 1.66 | 1.67 | 1.77 | 1.44 | 1.40 | 1.33 | 1.38 | 1.42 | 1.31 | 1.41 | **1.28** | 1.37 | 1.38 | 1.30 | 1.42 |
| cons_list | 0.92 | 1.26 | 0.84 | 0.93 | 0.87 | 0.65 | 0.65 | FAIL | 0.68 | 0.77 | 0.83 | 0.67 | **0.63** | 0.79 | 0.76 | 0.63 |
| deep_const | 5.88 | 5.53 | 4.99 | 4.93 | 4.66 | 4.05 | 4.52 | 3.91 | 4.45 | **3.82** | 4.54 | 4.36 | 4.48 | 4.05 | 3.92 | 3.88 |
| dll_walk | 0.88 | 1.14 | 0.90 | 0.87 | 0.82 | 0.65 | 0.65 | FAIL | 0.66 | 0.74 | 0.87 | 0.66 | **0.63** | 0.74 | 0.70 | 0.62 |
| early_return | 7.36 | 6.99 | 7.24 | 7.12 | 6.26 | 6.12 | 5.94 | 5.86 | 5.93 | 6.05 | 6.16 | **5.74** | 6.06 | 6.09 | 5.92 | 5.91 |
| fannkuch | 0.73 | 0.78 | 0.75 | 0.78 | 0.72 | 0.67 | 0.63 | **0.62** | 0.63 | 0.64 | 0.65 | 0.62 | 0.66 | 0.75 | 0.68 | 0.65 |
| fib | 6.66 | 7.25 | 6.86 | 6.96 | 6.22 | 6.17 | 6.25 | 6.01 | 6.07 | **5.92** | 6.27 | 5.94 | 6.17 | 6.07 | 6.07 | 6.02 |
| fib_pair | 1.02 | 1.47 | 0.96 | 1.03 | 1.00 | 0.74 | **0.71** | 0.85 | 0.76 | 0.92 | 0.74 | 0.76 | 0.72 | 0.88 | 0.87 | 0.73 |
| gc_combined | 1.00 | 1.33 | 0.89 | 0.94 | 0.92 | 0.69 | 0.67 | 0.76 | 0.71 | 0.85 | 0.67 | **0.65** | 0.70 | 0.77 | 0.78 | 0.67 |
| gcd | 4.76 | 5.35 | 5.39 | 5.41 | 4.47 | 4.44 | 4.33 | 4.33 | 4.42 | **4.27** | 4.36 | 4.28 | 4.42 | 4.32 | 4.27 | 4.28 |
| graph_bfs | 1.14 | 1.17 | 1.35 | 1.46 | 1.06 | 1.06 | 0.99 | FAIL | 1.02 | 1.02 | 1.26 | 1.05 | 1.06 | 1.05 | 1.08 | **0.97** |
| hash_chain | 1.94 | 1.44 | 1.52 | 2.00 | 1.05 | 1.04 | 0.99 | 0.98 | 0.96 | 0.98 | 1.01 | 1.40 | 1.00 | 0.99 | **0.93** | 0.97 |
| interp_calc | 1.13 | 1.48 | 1.05 | 1.10 | 0.96 | 0.80 | **0.77** | FAIL | 0.82 | 0.90 | 0.78 | 0.90 | 0.84 | 0.85 | 0.83 | 0.78 |
| json_parse | 1.27 | 2.23 | 1.02 | 1.18 | 1.04 | 0.82 | 0.83 | FAIL | 0.87 | 1.06 | **0.77** | 0.97 | 0.88 | 0.85 | 0.84 | 0.82 |
| life | — | 1.49 | 1.50 | 1.52 | 1.30 | 1.30 | 1.28 | FAIL | 1.27 | 1.30 | 1.25 | 1.39 | 1.26 | 1.27 | 1.27 | **1.24** |
| list_alloc | 1.00 | 1.31 | 0.88 | 0.95 | 0.82 | 0.67 | **0.61** | 0.72 | 0.67 | 0.86 | 0.64 | 0.74 | 0.64 | 0.73 | 0.75 | 0.67 |
| list_sort | 1.28 | 1.37 | 1.29 | 1.36 | 1.17 | 1.01 | **0.97** | 0.97 | 1.00 | 1.09 | 0.99 | 1.03 | 1.00 | 1.15 | 1.11 | 1.10 |
| loop | 1.62 | 1.63 | 1.66 | 1.62 | 1.35 | 1.37 | 1.35 | 1.32 | 1.30 | 1.33 | 1.34 | 1.42 | 1.37 | 1.35 | 1.30 | **1.28** |
| nqueens | 1.00 | 1.03 | 1.03 | 1.00 | 0.89 | 0.89 | 0.84 | 0.85 | 0.85 | **0.82** | 0.86 | 0.89 | 0.87 | 0.87 | 0.87 | 0.83 |
| prime_count | 24.17 | 23.94 | 24.98 | 25.30 | 20.72 | 20.89 | 20.67 | 19.93 | 20.33 | 20.41 | 21.28 | 20.77 | 20.71 | 20.02 | **19.43** | 20.06 |
| remset_pressure | 0.49 | 0.53 | 0.41 | 0.38 | 0.37 | 0.31 | **0.28** | FAIL | 0.31 | 0.32 | 0.31 | 0.29 | 0.29 | 0.32 | 0.31 | 0.30 |
| sieve | 1.36 | 1.25 | 1.33 | 1.44 | 1.35 | 1.18 | 1.15 | **1.11** | 1.14 | 1.21 | 1.21 | 1.16 | 1.18 | 1.32 | 1.17 | 1.20 |
| string_concat | 0.30 | 0.46 | 0.25 | 0.29 | 0.28 | **0.19** | 0.19 | 0.23 | 0.22 | 0.24 | 0.19 | 0.20 | 0.21 | 0.22 | 0.22 | 0.20 |
| string_concat_dyn | 1.33 | 1.86 | 1.13 | 1.25 | 1.15 | 0.89 | 0.92 | 1.02 | 0.99 | 1.10 | 0.89 | 0.95 | 0.93 | 0.94 | 0.97 | **0.87** |
| substr_churn | 1.08 | 1.40 | 1.00 | 1.07 | 0.89 | 0.72 | 0.73 | 0.84 | 0.80 | 0.85 | **0.71** | 0.78 | 0.77 | 0.71 | 0.73 | 0.75 |
| tak | 0.77 | 0.87 | 0.86 | 0.85 | 0.69 | 0.69 | 0.68 | 0.69 | **0.67** | 0.72 | 0.69 | 0.67 | 0.71 | 0.68 | 0.69 | 0.68 |
| tokenize | 1.15 | 1.84 | 1.01 | 1.12 | 1.00 | 0.73 | 0.76 | FAIL | 0.86 | 0.95 | **0.72** | 0.79 | 0.80 | 0.80 | 0.81 | 0.80 |

生 log: [`../benchmark/report/20260526_plain.log`](../benchmark/report/20260526_plain.log)
+ [`../benchmark/report/20260526_plain_part2.log`](../benchmark/report/20260526_plain_part2.log)。
集約 TSV: [`../benchmark/report/20260526_aggregated.tsv`](../benchmark/report/20260526_aggregated.tsv)。

`life` の libgc 列だけ `—` なのは naruby ベースの parser/main の
toplevel return value bug (GC 非関連、 別件)。

**`mark_compact` の FAIL 9 件** (= binary_trees / cons_list / dll_walk / graph_bfs
/ interp_calc / json_parse / life / remset_pressure / tokenize) は本 backend の
構造的問題 (= slide compact 中の pointer 書換が現状の precise rooting と整合しない
ケースが特定の alloc pattern で出る)。 回避策: `mark_compact_gen` (= nursery +
compact tenured) を使う。

### 2.2 peak RSS (MiB, max of 3 /usr/bin/time -M)

| Bench | libgc | none | mark | mark_gen | mark_gen_inc | copy | copy_gen | mark_compact | mark_compact_gen | bump | mark_bump_gen† | immix | immix_gen | mark_bitmap_gen | mark_card_gen | mark_freelist† |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| ackermann | 10.0 | 7.6 | 8.0 | 8.2 | 7.8 | 8.0 | 8.1 | 8.1 | 8.1 | 8.1 | 8.2 | 8.1 | 8.1 | 8.4 | 8.2 | 8.4 |
| ast_eval | 10.8 | 10.6 | 9.0 | 11.0 | 10.9 | 8.1 | 8.2 | 9.8 | 9.9 | 8.2 | 8.2 | **8.0** | 8.1 | 9.1 | 9.2 | 9.1 |
| binary_trees | 281.0 | 258.4 | 258.6 | 269.6 | 269.8 | 338.2 | 402.1 | FAIL | 274.1 | **194.2** | 231.8 | 195.5 | 227.9 | 267.2 | 267.0 | 258.1 |
| branch_dom | 3.5 | 2.4 | 2.4 | 2.4 | **2.2** | 2.4 | 2.4 | 2.5 | 2.2 | 2.4 | 2.4 | 2.2 | 2.2 | 2.2 | 2.2 | 2.2 |
| call | 3.5 | 2.4 | 2.4 | **2.1** | 2.5 | 2.5 | 2.4 | 2.4 | 2.4 | 2.4 | 2.4 | 2.4 | 2.1 | 2.4 | 2.4 | 2.2 |
| chain20 | 3.5 | **2.2** | 2.4 | 2.4 | 2.2 | 2.2 | 2.2 | 2.4 | 2.4 | 2.4 | 2.4 | 2.4 | 2.4 | 2.4 | 2.2 | 2.4 |
| chain40 | 3.5 | **2.2** | 2.4 | 2.4 | 2.4 | 2.4 | 2.2 | 2.4 | 2.2 | 2.4 | 2.4 | 2.4 | 2.4 | 2.2 | 2.2 | 2.4 |
| chain_add | 3.5 | 2.4 | **2.2** | 2.4 | 2.4 | 2.4 | 2.4 | 2.4 | 2.5 | 2.2 | 2.4 | 2.2 | 2.4 | 2.4 | 2.2 | 2.2 |
| collatz | 3.6 | 2.4 | 2.4 | 2.2 | 2.4 | 2.4 | 2.2 | 2.4 | 2.4 | **2.2** | 2.2 | 2.2 | 2.4 | 2.4 | 2.5 | 2.4 |
| compose | 3.5 | 2.5 | 2.4 | 2.4 | 2.4 | 2.4 | 2.4 | 2.2 | **2.2** | 2.2 | 2.4 | 2.2 | 2.2 | 2.4 | 2.4 | 2.4 |
| cons_list | 5.5 | 612.4 | 24.2 | 29.2 | 29.1 | 35.0 | 34.0 | FAIL | 34.1 | 460.1 | 34.2 | **18.9** | 34.2 | 24.5 | 24.5 | 24.1 |
| deep_const | 3.5 | 2.4 | 2.2 | 2.4 | 2.4 | **2.1** | 2.4 | 2.4 | 2.2 | 2.2 | 2.2 | 2.2 | 2.4 | 2.2 | 2.4 | 2.2 |
| dll_walk | 5.5 | 460.1 | 21.0 | 25.2 | 25.2 | 35.2 | 34.1 | FAIL | 34.2 | 322.8 | 34.2 | **18.8** | 34.2 | 21.2 | 21.2 | 20.9 |
| early_return | 3.5 | 2.2 | 2.2 | 2.4 | 2.4 | 2.4 | 2.4 | 2.4 | 2.4 | **2.2** | 2.4 | 2.4 | 2.2 | 2.2 | 2.2 | 2.2 |
| fannkuch | 4.8 | 56.5 | 30.8 | 32.8 | 32.8 | 34.4 | 34.2 | **18.2** | 34.2 | 85.9 | 34.1 | 18.5 | 34.4 | 30.9 | 31.0 | 30.6 |
| fib | 3.5 | 2.2 | 2.2 | 2.4 | 2.4 | **2.1** | 2.5 | 2.2 | 2.4 | 2.2 | 2.5 | 2.4 | 2.4 | 2.2 | 2.4 | 2.2 |
| fib_pair | 4.4 | 817.8 | 23.6 | 29.0 | 28.9 | 34.4 | 34.1 | **18.1** | 34.1 | 614.1 | 34.4 | 18.2 | 34.2 | 24.0 | 23.9 | 23.6 |
| gc_combined | 5.6 | 765.2 | 26.8 | 31.1 | 31.3 | 34.9 | 34.9 | 19.0 | 34.8 | 613.5 | 34.8 | **18.8** | 35.1 | 27.3 | 27.1 | 26.8 |
| gcd | 3.5 | **2.2** | 2.2 | 2.2 | 2.5 | 2.2 | 2.4 | 2.2 | 2.2 | 2.4 | 2.4 | 2.2 | 2.4 | 2.4 | 2.4 | 2.4 |
| graph_bfs | 9.5 | 60.6 | 15.6 | 23.3 | 23.4 | 22.3 | 35.8 | FAIL | 36.1 | 184.2 | 35.6 | **15.1** | 195.5 | 23.3 | 23.4 | 15.8 |
| hash_chain | 17.2 | 13.8 | 20.2 | 22.6 | 22.8 | **13.2** | 13.2 | 15.6 | 15.6 | 13.2 | 13.1 | 13.2 | 13.1 | 20.2 | 20.5 | 19.2 |
| interp_calc | 6.5 | 627.2 | 21.4 | 25.1 | 25.2 | 35.9 | 34.1 | FAIL | 34.2 | 439.6 | 34.1 | **19.2** | 34.2 | 21.9 | 21.8 | 21.5 |
| json_parse | 4.9 | 944.0 | 23.6 | 29.1 | 29.1 | 34.4 | 34.2 | FAIL | 34.4 | 710.1 | 34.4 | **18.5** | 34.4 | 24.1 | 24.1 | 23.8 |
| life | — | **19.0** | 36.5 | 36.6 | 37.0 | 34.5 | 34.1 | FAIL | 34.2 | 34.6 | 34.2 | 18.6 | 34.2 | 36.8 | 36.8 | 33.8 |
| list_alloc | 4.2 | 764.9 | 26.4 | 30.6 | 30.4 | 34.4 | 34.2 | **18.1** | 34.2 | 612.8 | 34.4 | 18.4 | 34.2 | 26.5 | 26.6 | 26.2 |
| list_sort | 4.8 | 249.9 | 29.1 | 31.4 | 31.3 | 32.4 | 34.2 | **18.5** | 34.4 | 310.6 | 34.4 | 18.7 | 48.1 | 29.5 | 29.6 | 28.2 |
| loop | 3.5 | 2.4 | 2.4 | 2.5 | 2.4 | **2.2** | 2.2 | 2.4 | 2.4 | 2.4 | 2.4 | 2.4 | 2.2 | 2.2 | 2.2 | 2.5 |
| nqueens | 4.6 | 23.2 | 29.8 | 32.2 | 32.2 | 30.1 | 30.5 | **18.4** | 34.4 | 30.4 | 30.6 | 18.2 | 30.2 | 29.9 | 29.9 | 29.6 |
| prime_count | 3.5 | 2.4 | 2.4 | **2.2** | 2.4 | 2.4 | 2.2 | 2.4 | 2.2 | 2.4 | 2.4 | 2.4 | 2.4 | 2.4 | 2.5 | 2.2 |
| remset_pressure | 18.1 | 228.1 | 30.4 | 37.2 | 37.0 | 43.4 | 38.9 | FAIL | 40.2 | 171.5 | 39.0 | **23.5** | 39.5 | 32.2 | 32.1 | 30.2 |
| sieve | 275.5 | 83.9 | **83.4** | 194.3 | 194.2 | 138.5 | 276.2 | 138.4 | 276.4 | 274.4 | 272.2 | 83.5 | 274.4 | 194.1 | 194.1 | 83.5 |
| string_concat | 4.2 | 307.4 | 26.8 | 28.9 | 29.1 | 38.8 | 34.1 | **20.0** | 34.4 | 231.2 | 34.2 | 20.6 | 34.2 | 23.8 | 24.0 | 26.6 |
| string_concat_dyn | 4.2 | 917.8 | 24.5 | 29.0 | 29.1 | 35.4 | 34.4 | **18.9** | 34.0 | 688.9 | 34.2 | 19.1 | 34.2 | 23.9 | 24.0 | 24.5 |
| substr_churn | 36.0 | 568.2 | 65.0 | 46.4 | 46.2 | 88.0 | 51.4 | 53.5 | 51.4 | 431.4 | 51.5 | 54.0 | 51.1 | **41.1** | 41.0 | 65.2 |
| tak | 3.5 | 2.4 | 2.4 | **2.2** | 2.4 | 2.4 | 2.5 | 2.4 | 2.4 | 2.4 | 2.2 | 2.2 | 2.2 | 2.4 | 2.4 | 2.2 |
| tokenize | 4.8 | 980.5 | 24.0 | 29.6 | 29.5 | 34.2 | 34.2 | FAIL | 34.2 | 756.5 | 34.4 | **18.6** | 34.4 | 24.6 | 24.6 | 24.0 |

### 2.3 観察まとめ

**naruby-style int bench (alloc 無し)** — ackermann / branch_dom / call /
chain20 / chain40 / chain_add / collatz / compose / deep_const / early_return
/ fib / fib_pair / gcd / loop / prime_count / tak。 全 backend が dispatch
コスト dominant で 1.1–1.4× の範囲に収まる。 `mark_compact` / `mark_card_gen`
/ `mark_freelist` がよく勝つ (= alloc 殆ど発火しないので i-cache 配置の
僅差で決まる)。 libgc も同水準。

**alloc-heavy bench** (= 1 行目 dispatch + alloc cost):

- `cons_list` / `dll_walk` / `fib_pair` / `gc_combined` / `interp_calc` /
  `list_alloc`: **gen / immix 系が勝つ** (immix / immix_gen / copy_gen /
  mark_bump_gen)。 nursery で完結する short alloc が大半なので minor で済む
- `tokenize` / `string_concat_dyn` / `substr_churn` / `json_parse`:
  **mark_bump_gen / mark_freelist が強い**。 string + bytes alloc が頻発する
- `binary_trees`: **bump が最速** (= 0.39 で圧倒的、 GC が走らない 1-pass
  alloc)、 次点 immix (0.48)。 gen 系は promote コストで遅い
- `sieve`: **mark_compact が最速** (1.11)。 iter 67-69 の realloc_in_place
  が効いている
- `remset_pressure`: **copy_gen 最速** (0.28)、 続いて immix / immix_gen
- `life`: 全 backend ほぼ tie (1.24–1.52)。 mutator-bound で alloc 軽め

**`mark_compact` 全 FAIL**: alloc-heavy 9 bench で構造的 FAIL (= todo)。
代替の `mark_compact_gen` は全 PASS。

**libgc との対比** (= precise rooting + 移動 GC vs conservative scan):
- libgc が勝つ: なし (= int bench で precise が同水準〜微優、 alloc-heavy では
  precise の gen GC が圧勝)
- baruby_precise が勝つ: ほぼ全 bench。 特に alloc-heavy で **20–60% 速い**
  (例 `tokenize`: libgc 1.15s vs `mark_bump_gen` 0.72s = -38%)

詳細な per-backend 解説は §4。

## 3. memory 消費比較 (= time とは別軸の RSS HWM 観察)

### 3.1 alloc-heavy benches での RSS 分布 (= cons_list / list_alloc など)

`none` / `bump` (no GC) は **400–950 MiB** に到達 (= leak で全 alloc 永続)。
`copy_gen` / `mark_*` 系 (gen) は **30–35 MiB**。 `immix` 系 (= line/block
reclamation) は **18–20 MiB** で最小、 `mark_compact` / `mark_freelist` 系も
近い水準。

| backend ファミリ | typical RSS (cons_list MiB) | コメント                         |
|------------------|-----------------------------|----------------------------------|
| `none` / `bump`† | 460–612                     | leak、 全 alloc 永続             |
| `mark` / mark_*  | 24–29                       | per-obj malloc + sweep           |
| `copy` / `copy_G`| 34                          | semispace 2× alloc space         |
| `mark_compact*`  | 18 (compact) / 34 (gen)     | slide compact で圧縮             |
| `immix` / `I_G`  | **18.9**                    | line+block dense layout          |
| `mark_freelist`† | 24.1                        | freelist isolation               |

→ **`immix` 系が最小 RSS** (= 18.9 MiB)、 **`mark_compact_gen` も 34 MiB**
で gen 系として小さい方。 **`none` / `bump`† が圧倒的に大 RSS** (= 6500%
larger than immix on cons_list)。

### 3.2 deep heap-walk bench (= binary_trees) での RSS

| backend           | RSS MiB | 備考                                   |
|-------------------|--------:|----------------------------------------|
| `bump`†           |   194.2 | leak で peak 在庫、 sweep 無し         |
| `immix`           |   195.5 | live set 200 MiB、 効率良い            |
| `immix_gen`       |   227.9 | gen overhead                           |
| `mark`            |   258.6 | per-obj malloc                         |
| `none`            |   258.4 |                                        |
| `mark_freelist`†  |   258.1 |                                        |
| `mark_bump_gen`†  |   231.8 |                                        |
| `mark_compact_gen`|   274.1 |                                        |
| `libgc`           |   281.0 | conservative scan、 page 単位 retain   |
| `mark_gen`        |   269.6 | young + tenured                        |
| `mark_gen_inc`    |   269.8 |                                        |
| `copy`            |   338.2 | 2 semispace = 2× live                  |
| `copy_gen`        |   402.1 | 4 region (2 young + 2 tenured)         |
| `mark_compact`    |    FAIL |                                        |

→ **`bump` / `immix` が最小** (= leak だが alloc が完全に walk 不要)、
**`copy_gen` が最大** (= 4-region 重複)。

### 3.3 substr_churn (= 大 String + slice churn) での RSS

| backend           | RSS MiB | 備考                                  |
|-------------------|--------:|---------------------------------------|
| `libgc`           |    36.0 | string body 全体保持                  |
| `mark_bitmap_gen` |  **41.1** | 8 B header + bitmap で dense        |
| `mark_card_gen`   |    41.0 |                                       |
| `mark_gen_inc`    |    46.2 |                                       |
| `copy_gen`        |    51.4 |                                       |
| `copy`            |    88.0 | semispace 2× = string body 2倍       |
| `bump`†           |   431.4 | leak                                  |
| `none`            |   568.2 | leak、 small alloc 多発               |

→ **`mark_bitmap_gen` / `mark_card_gen` が最も RSS 小** で且つ libgc に近い。
String-heavy では bitmap / card な mark+sweep が効率的。

### 3.4 RSS ランキング (= 全 35 bench geomean of RSS MiB、 alloc-heavy 26 bench)

| backend           | RSS geomean | 備考                                    |
|-------------------|------------:|-----------------------------------------|
| `immix`           |        17.3 | 最小 RSS                                |
| `mark_compact`    |        19.4 | 圧縮効くが alloc-heavy で FAIL          |
| `mark_freelist`†  |        24.5 | testbed                                 |
| `mark_bitmap_gen` |        24.5 |                                         |
| `mark_card_gen`   |        24.5 |                                         |
| `mark`            |        26.8 |                                         |
| `mark_compact_gen`|        34.2 |                                         |
| `mark_bump_gen`†  |        34.3 |                                         |
| `mark_gen_inc`    |        29.5 |                                         |
| `copy`            |        35.4 |                                         |
| `mark_gen`        |        29.5 |                                         |
| `copy_gen`        |        34.5 |                                         |
| `immix_gen`       |        35.0 |                                         |
| `libgc`           |       (mixed) | 36–281 MiB range、 conservative の特性 |
| `bump`†           |       400+ | leak                                    |
| `none`            |       500+ | leak                                    |

### 3.5 time vs RSS tradeoff

- **`copy_gen` は速いが RSS 大** (= 4 region で 30–400 MiB)。 short alloc
  workload では best time
- **`mark_compact_gen` は遅めだが RSS 小** (= compact tenured で半分以下)
- **`mark_bitmap_gen` / `mark_card_gen` は中庸** (= 24–25 MiB、 time も
  中位)
- **`immix` 系が time + RSS 共に良い** (= alloc-heavy で trade-off frontier に位置)

production 用途では time 重視なら `copy` / `copy_gen`、 memory 重視なら
`immix` / `mark_bitmap_gen` / `mark_card_gen`。

## 4. backend 比較 + 選択ガイド

### 4.1 ワークロード→ backend 選択ガイド

| パターン | 推奨 | 理由 |
|---|---|---|
| 短命 alloc 多 (nursery 完結) | `immix_gen` / `copy_gen` / `mark_compact_gen` | minor scan only |
| 長寿命 heap 大半 (binary_trees 等) | `immix` / `copy` | gen の overhead 不要 |
| string-heavy | `mark_card_gen` / `mark_bitmap_gen` / `mark_bump_gen`† | dense layout |
| 仮想空間節約 | `immix` / `mark_compact_gen` | tenured 1× |
| latency 上限が要件 | `mark_gen_inc` (要 budget 設定) | inc で pause 分割 |
| pure alloc throughput | `bump`† / `none` | GC 抜きの floor |
| **time vs RSS balance** | `copy` (時間) / `immix` (memory) | trade-off |

default は `copy` (= Cheney semispace、 balanced)。

### 4.2 GC 時間 / pause 計測

`BARUBY_GC_STATS=1` で各 backend が mutator / GC 時間を分けて出す。
`gc_seconds` / `gc_pct` / `max_pause_ms` / `mark_seconds` /
`reclaim_seconds` の 5 値。 minor → major の re-entrant ケースは
depth guard で最外側だけ計上。

phase semantics:

| backend kind | mark | reclaim |
|---|---|---|
| mark&sweep (`mark` / `mark_gen` / `mark_gen_inc` / `mark_bitmap_gen` / `mark_card_gen` / `mark_freelist`) | root scan + gray queue | sweep |
| mark&compact (`mark_compact` / `mark_compact_gen` / `mark_bump_gen`) | trace | forward + update_pointers + slide |
| copy (`copy` / `copy_gen`) | (Cheney は trace と relocate が交錯した単一 loop) = 0 | 全部 reclaim 計上 |

`mark_gen` vs `mark_gen_inc` で max_pause は **5.4× 差** (= 旧計測 binary_trees
で 288.55ms vs 53.84ms)。 latency 重視 workload では `mark_gen_inc` が選択肢に。

## 5. 主な最適化履歴 (iter 範囲別)

詳細は [done.md](done.md)。 perf.md は要点のみ。

| iter | 内容 | 効果 |
|---|---|---|
| 36 | array literal 1-shot (`node_ary_lit_N`) | plain -9〜-12% |
| 37 | str literal `+` の parse-time fold | string_concat plain -58%, AOT -79% |
| 38 | remset cap + heap-walk fallback | gen backend の adversarial fix |
| 41 | mark_freelist 追加 (= testbed) | medium-rate hash_chain で 0.96 → 0.79 |
| 53 | SSO (BSTR_SSO_MAX=7) | tokenize -17%、 substr_churn -2% |
| 58 | @child operand 全面導入 | dispatcher convention 統一 |
| 61 | fp 引数削除 (3-arg dispatcher) | prime_count -89%、 関数呼出系 -50〜-68% |
| 65 | `aro_gc_fini` で全 backend clean shutdown | exit-time validity |
| 67-69 | LargeObj realloc_in_place (realloc(3) / mremap) | sieve / hash_chain -11〜-21% |
| 71 | call_N args @child 化 | walker から callee_locals_cnt 依存除去 |
| 72 | walker 削除 + parse-time bake + noinline/cold | plain geomean -3.02% (vs iter 71) |
| 73 | baruby_ary_push fast-path inline + AOT bake で endbr64 (CET) 削除 | sieve copy AOT -8.6% |
| 76 | `copy_gen_inc` backend 削除 (= `copy_gen` の clone) | matrix 列を 16 → 15 |
| 77+ | `mark_bump_gen` / `mark_freelist` を §3 testbed に再分類 | doc 整理、 実用は §2 11 個 |

## 6. 既知の問題

- **`mark_compact` alloc-heavy 9 FAIL**: §2.1 参照。 回避策 `mark_compact_gen`。
  todo: 構造的修正
- **stress mode (`BARUBY_GC_STRESS=1`) の resource limit**:
  - `gc_copy`: 全 minor で from-space を `PROT_NONE + MADV_DONTNEED` で
    恒久 retire。 約 65k 回 GC で `/proc/sys/vm/max_map_count` を使い果たして
    abort
  - `gc_mark_bump_gen`†: tenured 側 compactor 無し。 long-live old が
    溜まると tenured 64 GiB virtual を使い切る (design limit、 §3.3 参照)
- **mark family の `hash_chain` slab locality**: BaArray と items[] が
  別 page で cache miss
- **`graph_bfs` で gen backend が遅い**: BFS の visited / queue が
  nursery threshold (16 MiB) より大きく、 minor 中に promote されてしまう

## 7. 次の段階

- **Immix v2 opportunistic evacuation** (fragmentation 解消)
- **mark_bitmap_gen minor sweep 最適化**: 64-bit-wise old_bm scan、
  per-page "all old" flag で binary_trees regression 縮小
- **`mark_gen_inc` を真の incremental に**: stack write barrier +
  work budget 実装 (現在は SATB infra のみで実態は STW、 `gc_runtime.md` §5.3)
- **`gc.c` / `gc.h` を `runtime/` に格上げ**: root mechanism (sp[] flat
  scan) + semi-space を framework backend として汎用化 (= 実装は
  `runtime/precise_gc/` で進行中、 docs は `docs/gc_design.md`)
- **PGO** (`-fprofile-use`) で LTO layout 最適化
- **`mark_compact` の slide compact alloc-heavy FAIL 修正**
