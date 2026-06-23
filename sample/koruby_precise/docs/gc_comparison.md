# koruby_precise — GC backend 比較レポート

2026-06-23。koruby_precise は build-time で 8 種の precise GC backend を切り替えられる
(`make GC=<backend>`)。「GC アルゴリズムを変えると速くなるか」をベンチスイート全体 +
GC stress 用に追加した 2 本で計測した。

## TL;DR

- **大半のコードでは GC backend は総実行時間にほぼ影響しない。** インタプリタ
  (mutator) が支配的で GC は数 % しか占めないため。default の `copy` で十分。
- **割り当てが本当に多いコードを AOT で走らせると backend が効く。** retained+churn
  系で **copy_gen が copy 比 −10%**、tree churn で **immix_gen が −17%**。ただし
  ワークロード依存 (大オブジェクト churn では copy_gen は +17% 悪化)。
- **mark-sweep 系 (mark / mark_gen / mark_compact) は churn 系で一貫して最悪**
  (copy 比 +15〜31%)。コピー系は生存オブジェクトだけ触るが mark-sweep はヒープ全体を
  mark/sweep するため、若くして死ぬゴミが多いと不利。
- **optcarrot は GC 律速でない** (180frame で alloc 173MB / GC 3〜17回 = 実行時間の
  1〜4%)。だから optcarrot では backend を変えても速くならない。これは optcarrot 固有。
- **バグ発見: `mark_compact_gen` は bignum で core dump** (GMP bignum + mark-compact)。

## 計測方法

- ベンチは `sample/rubyharness/bench/`。全 51 本を `KORUBY_GC_STATS=1 --plain` で
  triage し、**GC を 3 回以上起こす 20 本**を対象に選定 (残り 31 本は alloc ≒ 0 /
  GC 0 回で backend 無関係)。
- 揺れるマシン負荷 (16core / load 24〜35) を相殺するため **round-robin best-of-3**
  (各 backend を同じ負荷分布でサンプル)。`gc_count`/`allocMB` は決定論的で load 非依存。
- 全 run の出力を **CRuby と一致検証** (`checksum_ok`)。
- `--plain` は mutator が遅いので GC 割合を過小評価する。実モードの **AOT
  (`--compiled-only`)** でも GC-designed 5 本を計測 (mutator が速く GC 割合が上がる)。
- 再現: `make gc-compare`、`tools/gc_bench_ab.sh`、`tools/gc_aot_mini.sh`、
  `tools/gc_triage.sh`、`tools/gc_report.rb`。

## 1. 各 bench の GC 負荷 (決定論的)

allocMB と GC 回数 (copy backend)。これが小さい bench は backend を変えても無意味。

| bench | allocMB | gc_count |
|---|--:|--:|
| strfmt | 1139 | 71 |
| gcchurn | 488 | 30 |
| str | 419 | 26 |
| strscan | 409 | 25 |
| ary | 400 | 25 |
| gen_gc | 370 | 20 |
| closures | 275 | 17 |
| object | 275 | 17 |
| array_access | 250 | 15 |
| strops | 215 | 13 |
| nesteddata | 195 | 12 |
| methodchain | 189 | 11 |
| sprintfb | 178 | 11 |
| mapreduce | 176 | 11 |
| bignum | 16 | 10 |
| binary_trees | 144 | 9 |
| gc_wb (新規) | 112 | 6 |
| gc_bigobj (新規) | 92 | 5 |
| sort | 69 | 4 |
| cmpsort | 50 | 3 |

(triage 全体: 残り 31 本 — fib / ackermann / nbody / method_call / nested_loop /
mathfn / bitops / send / ivar 等 — は alloc < 2MB・GC 0 回。backend 完全に無関係。)

## 2. 総合 geomean (GC-heavy 20 本, copy 基準, `--plain`)

| backend | geomean(elapsed/copy) | geomean(gc_seconds/copy) | 備考 |
|---|--:|--:|---|
| **copy** (default) | 1.000× | 1.000× | baseline |
| **copy_gen** | **0.996×** | **0.424×** | GC時間は半減だが総時間は同等 (GCが小割合) |
| immix | 1.027× | 2.207× | |
| immix_gen | 1.031× | 0.594× | |
| mark | 1.164× | 21.7× | churn に弱い |
| mark_gen | 1.312× | 29.8× | 最遅 |
| mark_compact | 1.152× | 20.1× | |
| mark_compact_gen | 1.064× | 9.7× | bignum で crash |

`--plain` では mutator が遅く GC が総時間の数 % なので、GC時間を半減する copy_gen でも
総 elapsed はほぼ動かない (0.996×)。一方 mark 系は GC 自体が 20〜30× 重く総時間も悪化。

## 3. AOT 計測 (GC-designed 5 本, `--compiled-only`, best-of-3)

mutator が速い AOT では GC 割合が上がり、backend 差が総時間に出る。

| bench | copy | copy_gen | immix_gen | mark_gen |
|---|--:|--:|--:|--:|
| gen_gc | 0.832s | **0.753s (0.90×)** | 0.789s (0.95×) | 1.141s (1.37×) |
| gcchurn | 0.529s | **0.479s (0.91×)** | 0.493s (0.93×) | 0.797s (1.51×) |
| binary_trees | 0.576s | 0.544s (0.94×) | **0.477s (0.83×)** | 0.776s (1.35×) |
| gc_wb | 0.215s | 0.214s (1.00×) | 0.240s (1.12×) | 0.287s (1.34×) |
| gc_bigobj | 0.541s | 0.632s (1.17×) | 0.593s (1.10×) | 0.705s (1.30×) |

GC時間 (gc_seconds) で見ると gen_gc は copy 0.161s → copy_gen 0.050s (0.31×)、
これが総時間 −10% に直結。一方 gc_bigobj は大オブジェクトが nursery を溢れさせ
copy_gen が逆に悪化 (+17%)。**世代別は「小さく若く死ぬゴミ + 長寿命の retained 集合」
というパターンで勝ち、大オブジェクト churn では負ける。**

## 4. 失敗

- **bignum / mark_compact_gen**: core dump (GMP bignum + mark-compact backend のバグ)。
  他 7 backend は bignum 正常。

## 結論

GC backend を default の `copy` から変える価値があるのは「AOT + 割り当て多 + 若死に
パターン」に限られ、そこでは copy_gen / immix_gen が 10〜17% 速い。汎用の default は
`copy` のままが最善 (churn でも最遅 backend 群より速く、generational の write-barrier
オーバーヘッドや大オブジェクト劣化もない)。最も重要なのは **どの backend でも mutator
高速化の方が eff（optcarrot で実証: GC は 1〜4%）** で、GC 選択は二次的なレバー。
