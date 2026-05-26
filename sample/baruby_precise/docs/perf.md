# baruby_precise 性能ノート

仕様は [spec.md](spec.md)、 実装は [runtime.md](runtime.md)、 残タスクは
[todo.md](todo.md)、 過去の iter 履歴は [done.md](done.md) を参照。

baruby_precise は **precise *moving* GC の testbed**。 15 GC backend を
build-time switch (`make GC=<name>`) で切り替えて挙動 / 性能を比較するのが
目的。 姉妹サンプル `sample/baruby` (conservative libgc) と同じテスト・
ベンチを共有して「precise rooting + 各種 GC algorithm のオーバーヘッド」を
測る。

## 0. Fairness contract

iter 35 で固定し、 N-survive (= 9 個全 gen backend に統一) で再確認した
比較契約:

- **Build**: `make GC=<backend> ASTRO_DEBUG=0` で全 backend に同じ flags
  (`-O3 -flto=auto -fno-plt -march=native -ggdb3`)。 dev は
  `make ASTRO_DEBUG=1` で opt-in。
- **Mode**: `--plain` を正本とする。 AOT (`--aot-compile --run`) と PG
  (`--pg-compile`) は別 doc / 別 section で扱う。
- **Repeats / policy**: 各 (backend × bench) を **median of N=3 以上**、
  RSS は **max of N=3** (= `/usr/bin/time -f "%e %M"` で elapsed + peak
  RSS を同時取得)。 best-of-N は使わない。
- **Charging model**: 全 gen backend で `sizeof(GCHeader) + ALIGN8(payload_size)`
  を alloc-bytes trigger に統一。 payload bytes と nursery occupancy bytes
  の混在を排除。
- **Trigger threshold**: 全 gen backend の minor を統一 16 MiB。 major
  adaptive threshold MIN も統一 16 MiB。 major は **old growth** で発火
  (`old_alloc_since_major > major_threshold`)。
- **N-survive**: 9 個全 gen backend (= `mark_gen`, `mark_gen_inc`,
  `copy_gen`, `mark_compact_gen`, `mark_bump_gen`, `immix_gen`,
  `mark_bitmap_gen`, `mark_card_gen`) を「N-survive で promote」 (=
  N minor 連続 survive で tenure) に統一。 旧 first-survival 系列は除去。
- **Backends excluded from matrix**: `copy_gen_inc` は実体が `copy_gen` の
  clone (inc_step / SATB なし) で「独立 algorithm」 を主張できないため
  完全撤去 (`e60fa150`)。 16 → **15 backend**。
- **GC timer**: `aro_gc_time_begin/_end` で全 backend の collect / minor /
  major を計測。 `mark_gen_inc` の `inc_step` も同経路。
- **Runner**: `bench-results/20260526/bench_baruby.sh` (= 本 perf.md の
  data source)。 backend ごと `make -B GC=<name>` で完全 rebuild、
  `strings | grep baruby_gc=` で stamp を検証、 1 (bench × backend) =
  3 試行で elapsed + RSS HWM を取得。

## 1. 計測環境

| 項目 | 値 |
|---|---|
| 日付 | 2026-05-26 |
| CPU | AMD Ryzen 9 5900HX (8C / 16T、 ~4.6 GHz boost) |
| Memory | 30 GiB |
| OS | Linux 6.8.0-117 (x86_64) |
| Compiler | gcc 13.3.0 |
| GC (precise) | 自前 15 backend、 `gc_<name>.c` |
| GC (conservative 比較) | Boehm libgc 8.2.6 (`sample/baruby` 由来) |
| Build flags | `-O3 -flto=auto -ggdb3 -march=native -fno-plt -DASTRO_DEBUG=0` |
| Default backend | `copy` (semispace Cheney) |
| Run policy | `bench_baruby.sh` — median of 3 elapsed / max of 3 RSS HWM、 plain mode |
| 測定方法 | `/usr/bin/time -f "%e %M"` で elapsed + peak RSS 同時取得 |

⚠ **「libgc との比較」 caveat**: いま測っているのは「collector のみの
差」 ではなく **「runtime + rooting + collector の合計差」**。
baruby_precise は precise rooting (`c->env..c->sp` の flat scan) と
moving GC の組合せ、 baruby は conservative scanning。 同じ言語 /
同じベンチ / 同じ build flags だが、 数値差を「GC algorithm の差」 と
読み切るのは過剰解釈。 collector-only 比較が欲しいなら同じ runtime に
backend を差し込む設計が要る (= 別 iter)。

bench は 10 種に絞った (= GC 評価向けに alloc pattern と寿命を spread):

| bench | alloc pattern | lifetime | 主に exercise する点 |
|---|---|---|---|
| `fib` | int 再帰 (alloc 無) | n/a | 再帰 + dispatch cost (baseline) |
| `fib_pair` | 2-要素 BaArray × 多数 | LIFO 短命 | nursery 完結率 |
| `fannkuch` | 順列 enumerate | 短命 | mutator-bound |
| `ackermann` | int 再帰 (alloc 無) | n/a | deep recursion dispatch |
| `binary_trees` | 2-要素 BaArray ×2M | 長寿命 (構築中 live) | mark/sweep walk、 Cheney copy |
| `gc_combined` | long permanent + short churn | 2 層 | gen benefit、 remset |
| `hash_chain` | bucket hash | 3 層 | WB heavy、 chain realloc |
| `list_alloc` | pure 4-要素 alloc | 1 iter | alloc throughput |
| `json_parse` | 再帰下降 parser | 短命 | recursion + alloc 密 |
| `chain_add` | int 連鎖 add | n/a | binop dispatch (baseline) |

## 2. 実用 GC 11 個の比較 (plain mode)

「実用」 = 通常 workload で発火 / sweep / promote が機能している backend。
特殊 4 個 (= `none`, `bump`, `mark_bump_gen`, `mark_freelist`) は §3 で扱う。

**表中の `libgc` 列**: 姉妹サンプル `sample/baruby` の binary (= 同じ baruby
言語実装を Boehm-Demers-Weiser conservative libgc にリンクしたもの) で
同じ bench を測った数値。 precise GC backend との「conservative GC との
速度比較」 baseline として並べる (= 詳しい比較は §4)。 caveat は §1 末尾
参照。

### 2.1 elapsed (秒、 median of 3)

| bench | libgc | mark | mark_gen | mark_gen_inc | copy | copy_gen | mark_compact | mark_compact_gen | immix | immix_gen | mark_bitmap_gen | mark_card_gen |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fib | 6.34 | 6.77 | 6.91 | **6.22** | 6.15 | 6.80 | 6.83 | 6.80 | 6.76 | 6.82 | 7.03 | 6.49 |
| fib_pair | 0.97 | 0.88 | 1.00 | 0.98 | 0.82 | **0.78** | 0.98 | 0.87 | 0.82 | 0.79 | 0.99 | 0.89 |
| fannkuch | 0.68 | 0.73 | 0.75 | 0.72 | 0.72 | 0.71 | 0.72 | 0.71 | **0.70** | 0.74 | 0.82 | 0.71 |
| ackermann | 6.60 | 7.24 | 7.10 | 6.50 | 7.14 | 7.06 | 7.03 | 7.09 | 7.09 | 7.74 | 7.18 | **6.39** |
| binary_trees | 0.83 | 0.81 | 0.71 | 1.20 | 0.64 | 1.11 | FAIL | 1.96 | **0.57** | 1.17 | 0.63 | 1.28 |
| gc_combined | 0.91 | 0.82 | 1.29 | 0.87 | 0.74 | **0.71** | 0.87 | 0.80 | 0.75 | 0.72 | 1.29 | 0.82 |
| hash_chain | 1.48 | 1.15 | 1.34 | **1.07** | 1.12 | 1.08 | 1.28 | 1.33 | 1.26 | 1.12 | 1.41 | 1.13 |
| list_alloc | 0.89 | 0.76 | 1.28 | 0.83 | 0.69 | 0.66 | 0.82 | 0.74 | 0.69 | **0.67** | 1.25 | 0.77 |
| json_parse | 1.14 | 0.95 | 1.73 | 1.06 | 0.89 | 0.90 | FAIL | 0.99 | 0.96 | **0.92** | 1.79 | 0.93 |
| chain_add | 1.09 | 1.16 | 1.26 | 1.22 | 1.28 | 1.23 | 1.20 | 1.38 | 1.13 | 1.26 | 1.25 | **1.08** |
| **geomean** | **1.42** | 1.35 | 1.62 | 1.42 | 1.27 | 1.32 | 1.57 (n=8) | 1.51 | 1.27 | 1.37 | 1.63 | 1.37 |

**太字** は各 bench の最速 backend (= `libgc` を含めず precise 15 内で
選んだ場合の最速)。

`mark_compact` の `binary_trees` / `json_parse` は SEGV (FAIL)。 これは
**non-gen** な mark_compact の長寿命 heap 大量での known issue (= compactor
が forwarding chain で 32 GiB を使い切る)。 gen 版 `mark_compact_gen` は
そのまま動く。

### 2.2 peak RSS (MiB、 max of 3 `/usr/bin/time -M`)

| bench | libgc | mark | mark_gen | mark_gen_inc | copy | copy_gen | mark_compact | mark_compact_gen | immix | immix_gen | mark_bitmap_gen | mark_card_gen |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fib | 3.6 | 2.5 | 2.5 | 2.4 | 2.5 | 2.4 | 2.4 | 2.4 | 2.4 | 2.4 | 2.4 | 2.4 |
| fib_pair | 4.2 | 23.6 | 29.1 | 29.2 | 34.4 | 34.1 | 18.2 | 34.2 | 18.5 | 34.4 | 23.9 | 23.9 |
| fannkuch | 4.9 | 30.9 | 33.0 | 33.0 | 34.2 | 34.4 | 18.4 | 34.2 | 18.6 | 34.4 | 31.0 | 31.0 |
| ackermann | 10.2 | 8.2 | 8.4 | 8.1 | 8.2 | 8.1 | 8.4 | 8.4 | 8.4 | 8.1 | 8.2 | 8.2 |
| binary_trees | 281.2 | 258.8 | 258.4 | 269.6 | 338.2 | 402.2 | FAIL | 274.2 | 195.2 | 227.8 | 258.3 | 267.4 |
| gc_combined | 5.6 | 26.9 | 765.6 | 31.1 | 34.9 | 34.7 | 18.9 | 34.9 | 18.8 | 35.2 | 765.6 | 27.3 |
| hash_chain | 17.6 | 20.3 | 13.8 | 22.9 | 13.3 | 13.3 | 15.6 | 15.6 | 13.4 | 13.4 | 13.8 | 20.4 |
| list_alloc | 4.2 | 26.4 | 765.3 | 30.5 | 34.4 | 34.4 | 18.2 | 34.2 | 18.4 | 34.4 | 765.3 | 26.6 |
| json_parse | 4.9 | 23.9 | 944.4 | 29.2 | 34.5 | 34.5 | FAIL | 34.5 | 18.5 | 34.4 | 944.2 | 24.1 |
| chain_add | 3.5 | 2.4 | 2.4 | 2.4 | 2.4 | 2.4 | 2.4 | 2.4 | 2.4 | 2.4 | 2.4 | 2.5 |
| **geomean** | **8.3** | 17.8 | 49.8 | 19.4 | 20.1 | 20.3 | 9.8 (n=8) | 19.9 | 13.9 | 19.2 | 48.2 | 17.9 |

### 2.3 観察

**time の trade-off**:

- **最速 plain backend は `copy` / `immix`** (geomean 1.27s = libgc 比 0.89×)。
  alloc-heavy bench (fib_pair / gc_combined / list_alloc / json_parse) で
  特に強い。 long-lived heavy な `binary_trees` でも `immix` 0.57s が
  全体最速 (= region-bump の locality)。
- **gen 系の `copy_gen` / `immix_gen`** は alloc-heavy で互角〜やや勝ち
  (= minor で完結)。 `binary_trees` で `copy_gen` が 1.11s と遅いのは
  promote 後の major copy cost。
- **`mark_gen` / `mark_bitmap_gen` は alloc-heavy で大幅遅延** (gc_combined
  1.29s / list_alloc 1.28s / json_parse 1.73-1.79s)。 RSS も 765-944 MiB
  に膨らんでおり、 「実質 GC が発火していない」 = no-collect 領域に落ちて
  いる可能性が高い (= alloc trigger / threshold tuning の問題)。 これは
  N-survive 化後に再発した regression として todo。
- **`mark_gen_inc` は alloc-heavy で正常** (gc_combined 0.87s / list_alloc
  0.83s)、 RSS も 29-31 MiB。 inc step の SATB infra が trigger を
  正しく駆動している側。
- **`mark_compact` (non-gen) は `binary_trees` / `json_parse` で FAIL**。
  forwarding chain が 32 GiB を使い切る。 gen 版 `mark_compact_gen` は
  そのまま動くが `binary_trees` で 1.96s と遅め。

**RSS の trade-off**:

- **最小 RSS は `mark_compact`** (geomean 9.8 MiB、 8 bench 平均)、 次に
  `immix` (13.9 MiB)。 圧縮 / region-bump の効果。
- libgc は alloc が無い bench (fib / chain_add) で precise 系より RSS が
  大きい (3.5-3.6 MiB vs 2.4 MiB) のは libgc 自体の常駐 mark stack 等。
  逆に alloc-heavy で `binary_trees` 281 MiB と precise 系の上位 (`immix`
  195 MiB) より大きい。
- **`copy` / `copy_gen` は `binary_trees` で 338 / 402 MiB**。 semispace の
  2× region cost が露呈。
- **`mark_gen` / `mark_bitmap_gen` の outlier 765-944 MiB** (gc_combined /
  list_alloc / json_parse) は前述の non-collect bug。 §5 todo。

**ranking (= geomean elapsed、 11 実用 backend)**:

1. `copy` — 1.27s (libgc 比 0.89×)
2. `immix` — 1.27s (0.89×)
3. `copy_gen` — 1.32s (0.93×)
4. `mark` — 1.35s (0.95×)
5. `immix_gen` — 1.37s (0.96×)
6. `mark_card_gen` — 1.37s (0.97×)
7. `mark_gen_inc` — 1.42s (1.00×)
8. `mark_compact_gen` — 1.51s (1.06×)
9. `mark_compact` — 1.57s (1.00×、 n=8、 2 bench FAIL)
10. `mark_gen` — 1.62s (1.14×)
11. `mark_bitmap_gen` — 1.63s (1.14×)

## 3. 特殊用途 backend 4 個

実用と並べると判定がミスリーディングになる backend。 §2 ranking からは
除外して、 ここで testbed としての挙動を観察する。

### 3.1 elapsed + RSS

| bench | none p / RSS | bump p / RSS | mark_bump_gen p / RSS | mark_freelist p / RSS |
|---|---:|---:|---:|---:|
| fib | 6.79 / 2.4 | 6.88 / 2.4 | 6.81 / 2.4 | 6.09 / 2.4 |
| fib_pair | 1.50 / 818.2 | 1.10 / 614.2 | 0.88 / 23.6 | 0.75 / 23.6 |
| fannkuch | 0.76 / 56.5 | 0.73 / 85.9 | 0.71 / 31.0 | 0.65 / 30.5 |
| ackermann | 7.27 / 8.1 | 7.10 / 8.2 | 7.21 / 8.2 | 6.33 / 8.2 |
| binary_trees | 0.65 / 258.4 | 0.63 / 258.4 | 0.79 / 258.8 | 0.72 / 258.2 |
| gc_combined | 1.29 / 765.6 | 1.28 / 765.6 | 0.82 / 26.9 | 0.69 / 26.7 |
| hash_chain | 1.28 / 13.8 | 1.47 / 13.8 | 1.15 / 20.4 | 1.19 / 19.4 |
| list_alloc | 1.27 / 765.3 | 1.28 / 765.3 | 0.78 / 26.4 | 0.68 / 26.4 |
| json_parse | 1.81 / 944.1 | 1.81 / 944.1 | 0.92 / 23.9 | 0.87 / 23.8 |
| chain_add | 1.20 / 2.4 | 1.26 / 2.4 | 1.14 / 2.4 | 1.04 / 2.4 |
| **geomean** | 1.67s / 72.8 | 1.63s / 73.8 | 1.34s / 17.7 | **1.21s** / 17.6 |

### 3.2 観察

- **`none` (= libc malloc + no free)**: 名前通り全 alloc を leak。
  alloc-heavy で 765-944 MiB を使い、 long bench (json_parse 1.81s / fib_pair
  1.50s) が `mark_freelist` の 2× 程度に遅い。 cache miss と TLB pressure
  が effective floor。 alloc throughput baseline として参考になる。
- **`bump` (= bump alloc only、 leak)**: `none` とほぼ同じ挙動 (RSS 614-944
  MiB)、 ただし alloc が region-bump で TLB miss が `none` より少ない分
  `binary_trees` 0.63s と全体 2 番目に速い。 long-lived heavy だけが速い
  pattern。
- **`mark_bump_gen` (= bump tenured + mark sweep nursery)**: RSS は 17.7 MiB
  と最小級、 elapsed は 1.34s で実用上位 (`mark_card_gen` 相当)。 ただし
  **tenured 側に compactor が無い** ため、 long-running workload では
  fragmentation が累積して 64 GiB virtual を使い切る design limit がある
  (= long-lived workload 非推奨)。 短時間 bench では問題が表面化しない。
- **`mark_freelist` (= mark+sweep + per-class freelist)**: 観察上は **全
  bench 正常完走、 elapsed geomean 1.21s で precise 系全体の最速**、 RSS
  17.6 MiB と最小級。 設計上 freelist は fragmentation accumulation の
  testbed として書いたが、 今回 bench の 10 種では fragmentation が顕在化
  していない。 元来 hash_chain (= medium alloc rate) で win する想定で
  追加した backend で、 実際 hash_chain は 1.19s で good。 中長期の
  workload (= matrix.rb の 30+ bench での long-running iter) でも追跡が
  必要。 「fragmentation 実証」 は今回 dataset では未確認。

特殊 4 個の挙動から:

- **alloc が無い bench (fib / ackermann / chain_add)** では elapsed が
  全 backend で 6-7s / 1.0-1.2s に収束 (= GC algorithm の差はほぼ無く、
  dispatch cost dominant)。 `mark_freelist` だけ `ackermann` 6.33s /
  `fib` 6.09s と 3-5% 速いのは bench-runner ノイズか header layout の
  差程度。
- **alloc-heavy bench (gc_combined / list_alloc / json_parse)** で
  `none` / `bump` は RSS 765-944 MiB に振り切る。 これらは「GC が無い
  ことの真コスト」 (= cache miss + page fault) の比較材料。 同じ大き
  さの RSS を出す `mark_gen` / `mark_bitmap_gen` が §2 で混ざるのは
  benchmark の trigger 不発の bug 由来 (§5 参照)。

## 4. libgc baseline との比較

### 4.1 ⚠ caveat (再掲)

libgc = `sample/baruby` の Boehm libgc 8.2.6 backend。 conservative scan、
non-moving、 C stack を bdwgc が直接 scan。 precise rooting overhead が無い
代わりに scan の精度が低い。 同じ言語 / 同じ bench を実行できるが、
**ここで測れているのは「runtime + rooting + collector の合計差」** であり、
GC algorithm のみの比較ではない (= conservative vs precise の overhead が
混入)。

### 4.2 plain mode: `libgc` plain vs `copy` plain (= 代表 head-to-head)

実用 backend の代表 = `copy` (Cheney semispace、 default GC、 §2.1 で
geomean 1.27s = 最速 tied)。 同じ plain mode で head-to-head:

| bench | libgc plain | copy plain | copy / libgc |
|---|---:|---:|---:|
| fib | 6.34 | 6.15 | **0.97×** |
| fib_pair | 0.97 | 0.82 | **0.85×** |
| fannkuch | 0.68 | 0.72 | 1.06× |
| ackermann | 6.60 | 7.14 | 1.08× |
| binary_trees | 0.83 | 0.64 | **0.77×** |
| gc_combined | 0.91 | 0.74 | **0.81×** |
| hash_chain | 1.48 | 1.12 | **0.76×** |
| list_alloc | 0.89 | 0.69 | **0.78×** |
| json_parse | 1.14 | 0.89 | **0.78×** |
| chain_add | 1.09 | 1.28 | 1.17× |
| **geomean** | **1.42s** | **1.27s** | **0.89×** |

- **`copy` plain が geomean 0.89× で libgc に勝つ** (= 10 bench 集計、
  -11% faster)。 baruby は alloc-heavy (= Array / String が頻出) のため
  GC bound bench で precise の効果が大きい
- **GC bound bench** (= `binary_trees` / `gc_combined` / `hash_chain` /
  `list_alloc` / `json_parse`) で `copy` が 0.76–0.81×。 conservative の
  per-call C stack scan より precise rooting + Cheney copy が速い
- **CPU bound** (= `fannkuch` / `ackermann` / `chain_add`) は precise の
  sp[] push/pop overhead で `copy` がわずかに負け (1.06–1.17×)
- ⚠ caveat (§4.1) を踏まえると「同 言語実装で同 bench、 ただし precise
  + Cheney は libgc + conservative より総合速い」 が結論

注: 今回 (2026-05-26 bench) は baruby_precise の plain mode のみ取得。
`--aot-compile` も実装されている (`make GC=<X>` で AOT bake 可能) が、
matrix.rb 集計時の chunk 制約で plain だけに絞った。 AOT 比較は ascheme
側 §4.3 を参照。

### 4.3 geomean ratio (= 11 実用 precise backend、 vs libgc)

| backend | elapsed geomean | vs libgc |
|---|---:|---:|
| `mark_freelist` (§3) | 1.21s | 0.85× |
| `copy` | 1.27s | 0.89× |
| `immix` | 1.27s | 0.89× |
| `copy_gen` | 1.32s | 0.93× |
| `mark` | 1.35s | 0.95× |
| `mark_bump_gen` (§3) | 1.34s | 0.94× |
| `immix_gen` | 1.37s | 0.96× |
| `mark_card_gen` | 1.37s | 0.97× |
| `mark_gen_inc` | 1.42s | 1.00× |
| `mark_compact` | 1.57s | 1.00× (n=8) |
| `mark_compact_gen` | 1.51s | 1.06× |
| `mark_gen` | 1.62s | 1.14× |
| `mark_bitmap_gen` | 1.63s | 1.14× |
| `none` (§3) | 1.67s | 1.17× |
| `bump` (§3) | 1.63s | 1.15× |
| `libgc` (baseline) | **1.42s** | **1.00** |

- **上位 5 (precise 実用)**: `copy` (0.89×) / `immix` (0.89×) /
  `copy_gen` (0.93×) / `mark` (0.95×) / `immix_gen` (0.96×) が libgc 比で
  互角〜10% 勝ち。
- **`mark_gen` / `mark_bitmap_gen` のみ 1.14× で大きく負け** (= §2 で
  指摘した non-collect bug によるもの)。 修正後 `mark_gen_inc` 並みの
  1.00× まで戻ることが想定される。
- libgc は alloc-heavy で precise 系より遅い (= hash_chain 1.48s vs `copy`
  1.12s) が、 baseline の int-only bench (fib / chain_add) では精度が
  低い分 fast (= 6.34s vs `copy` 6.15s でほぼ同等)。

## 5. todo

§2 で観測した未解決項目:

- **`mark_gen` / `mark_bitmap_gen` の alloc-heavy 不発**: gc_combined /
  list_alloc / json_parse で RSS 765-944 MiB、 elapsed 1.3-1.8s。
  N-survive 化以降の trigger / threshold tuning の問題。 alloc trigger を
  `mark_gen_inc` と同じ経路に揃えるか、 major adaptive を見直す。
- **`mark_compact` の `binary_trees` / `json_parse` SEGV**: non-gen の
  forwarding chain が 32 GiB virtual を使い切る known issue。 `gen` 版を
  推奨。
- **`copy` / `copy_gen` の `binary_trees` RSS 338 / 402 MiB**: semispace
  2× cost。 alloc-heavy 短期 workload では問題なし、 long-lived heap で
  顕在化。 `immix` (195 MiB) との対比に注意。
- **`mark_freelist` の fragmentation testbed としての検証**: 今回 10 bench
  では fragmentation が顕在化せず、 むしろ最速。 medium alloc rate の長期
  workload (= 30 min 以上の loop bench) で再確認が要る。 現状は
  「freelist algorithm が一定の workload では悪くない」 という positive
  結果として記録。
- **`bench-results/20260526/`** が data source。 matrix.rb (= bench/matrix.rb)
  も同じ 10 bench / 15 backend に揃える更新が要る (= 今は別 set を含む)。
- **AOT / PG mode の 15 backend × 10 bench** matrix は未取得。 別 iter で
  再実行。
