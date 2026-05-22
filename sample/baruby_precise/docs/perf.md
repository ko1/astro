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

### 2.1 plain mode

(matrix.rb 出力をここに貼る — iter 72 計測中、 完了次第更新)

### 2.2 AOT mode

(`-c` で code_store に SD を bake してから run。 上記同様 matrix.rb
`--mode aot` で生成。 後述)

### 2.3 観察まとめ

- **gen 系 backend は短命 alloc が多い workload で勝つ** (fib_pair /
  list_alloc / nqueens など)。 minor で nursery scan only で済むため。
- **non-gen (mark / copy / mark_compact) は long-lived heap で
  competitive**。 binary_trees 等は世代分離コストが効かない。
- **immix_gen は generic な strong baseline**。 region-bump alloc +
  conservative evacuation で多くの workload で middle-best。
- **mark_compact_gen は string / hash bench 系で強い**。 SSO + bump
  alloc + compact での再利用率が効く。
- **iter 67-69 の `realloc_in_place` (realloc(3) / mremap) で sieve /
  hash_chain で -11〜-21%** (= LargeObj の doubling cost 削減)。

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

iter 61 + 72 の dispatcher / parse 側 architecture 簡素化が大きい。
iter 67-69 の `realloc_in_place` が GC 側の最後の大きな win。

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
