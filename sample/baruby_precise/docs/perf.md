# baruby_precise 性能ノート

仕様は [spec.md](spec.md)、実装は [runtime.md](runtime.md)、
未対応・残タスクは [todo.md](todo.md) を参照。

baruby_precise は **precise *moving* GC (semi-space) の testbed** で、
姉妹サンプル `sample/baruby` (conservative libgc) と同じテスト・ベンチ
スクリプトで動かして「precise rooting + 移動 GC のオーバーヘッドは
どれくらいか」を測ることを目的にしている。 設計の経緯は
[`docs/gc_design.md`](../../../docs/gc_design.md) を参照。

## 1. 計測環境

| 項目 | 値 |
|---|---|
| CPU | AMD Ryzen 9 5900HX |
| OS | Linux 6.8 (x86_64) |
| Compiler | gcc 13.3.0 |
| GC (precise) | 自前 semi-space (`gc.c`、 ~310 行)、 region 512 MiB |
| GC (conservative 比較対象) | Boehm libgc 8.2.6 (`sample/baruby` 由来) |
| Build flags | `-O3 -flto=auto -ggdb3 -march=native -fno-plt -DASTRO_DEBUG=1` |
| GC backend  | `make GC=<name>` で選択。 default = `copy` (semispace Cheney) |

**比較対象**: `sample/baruby/` (libgc 経由の conservative scanning) を
baseline にする。 ベンチスクリプト (`bench/*.ba.rb`) は両者で共通 — baruby
を copy したのでファイル単位で同一。 binary 名のみ異なる
(`./baruby` vs `./baruby_precise`)。 plain mode = AST インタプリタ
(code_store なし)。 AOT mode は moving GC 移行後に未再検証。

## 2. 全 GC backend のベンチ実測 (plain mode, 3-run 中央値, 11 bench × 11 backend)

11 種類の backend × 11 ベンチ (`mark_bump_gen` 追加で 11 backend、 bench は
`hash_chain` `nqueens` `life` を含む)、 各 3-run 中央値。 単位: 秒。
行ごとの最速に `**` 印。

| Bench         | none | mark | mark\_gen | mark\_gen\_inc | copy | copy\_gen | copy\_gen\_inc | mark\_compact | mark\_compact\_gen | bump | mark\_bump\_gen |
|---------------|------:|------:|------:|------:|------:|------:|------:|------:|------:|------:|------:|
| binary_trees  | 0.62 | 0.88 | 1.11 | 1.16 | **0.52** | 0.75 | 0.79 | 0.58 | 0.79 | **0.52** | 0.92 |
| cons_list     | 1.24 | 0.87 | 0.96 | 1.04 | 1.06 | **0.77** | 0.83 | 1.14 | 0.83 | 1.03 | 0.92 |
| fib_pair      | 1.58 | 0.93 | 1.06 | 1.12 | 1.26 | **0.88** | 0.95 | 1.47 | 0.90 | 1.27 | 0.99 |
| gc_combined   | 1.40 | 1.01 | 1.00 | 1.19 | 1.19 | **0.94** | 0.99 | 1.28 | 1.01 | 1.22 | 1.00 |
| hash_chain    | 1.29 | 2.48 | 1.72 | 1.73 | 1.20 | 1.12 | 1.24 | 1.26 | 1.18 | **1.11** | 1.28 |
| interp_calc   | 1.35 | 1.02 | 1.17 | 1.23 | 1.22 | 1.03 | 1.01 | 1.24 | **1.00** | 1.15 | 1.14 |
| life          | 1.32 | 1.46 | 1.37 | **1.31** | 1.34 | 1.34 | 1.34 | 1.33 | 1.37 | 1.36 | 1.41 |
| list_alloc    | 1.33 | 0.98 | 0.97 | 1.10 | 1.15 | 0.91 | **0.87** | 1.22 | 0.94 | 1.18 | 0.97 |
| list_sort     | 1.17 | 1.23 | 1.21 | 1.25 | 1.21 | 1.12 | 1.06 | 1.19 | 1.10 | 1.21 | **1.15** |
| nqueens       | 0.98 | 1.00 | 0.98 | 0.98 | 1.00 | 0.98 | 0.98 | 0.95 | **0.90** | 0.98 | 1.04 |
| string_concat | 1.65 | 0.68 | 0.78 | 0.83 | 0.93 | 0.54 | **0.53** | 1.15 | **0.53** | 0.91 | 0.60 |
| substr_churn  | 1.70 | 1.15 | 1.11 | 1.11 | 1.28 | 0.93 | 0.90 | 1.48 | **0.90** | 1.13 | 0.99 |

**勝者分布** (2026-05-17 refresh): `copy_gen` が 4 bench で最速
(cons_list / fib_pair / gc_combined / string_concat tied)、
`copy_gen_inc` が 3 (list_alloc / string_concat / substr_churn tied)、
`mark_compact_gen` が 3 (interp_calc / nqueens / string_concat tied、
substr_churn tied)、 `bump` が 2 (binary_trees tied / hash_chain)、
`copy` が 1 (binary_trees tied)、 `mark_compact` が tied、 `mark_bump_gen`
が 1 (list_sort)、 `mark_gen_inc` が 1 (life)。

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

### ベンチカタログ (全 12 種)

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

## 3. baruby (libgc conservative) との比較 (plain, 3-run 中央値)

姉妹サンプル `sample/baruby` (Boehm libgc 経由の conservative scanning)
と同じ AST 評価器・同じ bench を共有。 GC 戦略以外の差分 (parser fix iter
(12) や bench 6 種) は baruby へ port 済 (commit 34be8d2)。 fair な
GC-only 比較。

`life.ba.rb` は baruby に top-level long while loop 後 value 取得の独立
バグがあり 11 bench でのみ比較。 baruby_precise 側は最速 backend を採用。

| Bench | baruby (libgc) | baruby\_precise 最速 (backend) | precise vs libgc |
|---|---:|---:|---|
| binary_trees  | 0.86 | **0.52** (`copy` / `bump`) | **-40%** |
| cons_list     | 0.91 | **0.77** (`copy_gen`) | -15% |
| fib_pair      | 1.14 | **0.88** (`copy_gen`) | -23% |
| gc_combined   | 1.09 | **0.94** (`copy_gen`) | -14% |
| hash_chain    | 1.44 | **1.11** (`bump`) | -23% |
| interp_calc   | 1.13 | **1.00** (`mark_compact_gen`) | -12% |
| list_alloc    | 0.98 | **0.87** (`copy_gen_inc`) | -11% |
| list_sort     | 1.15 | **1.05** (`mark_bump_gen`) | -9% |
| nqueens       | 0.97 | **0.90** (`mark_compact_gen`) | -7% |
| string_concat | 0.98 | **0.53** (`copy_gen_inc` / `mark_compact_gen`) | **-46%** |
| substr_churn  | 1.36 | **0.90** (`mark_compact_gen`) | -34% |

geomean: precise 最速は libgc 比 **約 -22%** (~ 0.78×)。

**観察**:

- **全 11 bench で precise の最速 backend が libgc を上回る**。 GC 戦略の
  バリエーション + precise rooting/WB の組合せが workload 適合性を
  上げている (libgc は一律 mark+sweep + conservative scan)。
- 最大差は **string_concat の -46%** と **binary_trees の -40%**。
  string_concat は libgc が短命 BaString を full-heap mark+sweep する
  のに対し、 precise の generational backend は nursery 完結で勝つ。
  binary_trees は libgc が conservative scan の stack / data segment 全
  走査を毎 GC やるのに対し、 precise の Cheney / bump 単純モデルが勝つ。
- 最小差は **nqueens の -7%、 list_sort の -9%**。 どちらも mutator 支配
  (GC 比率が低い) なので GC 戦略の差が見えにくい。
- 全 11 backend 比較は §2 table を参照。

**過去の比較表 (5-run 中央値、 baruby_precise default = `copy` 限定)**:

| Bench | libgc | precise (copy) | precise vs libgc |
|---|---:|---:|---|
| `binary_trees` | 0.907 | **0.576** | -37% |
| `list_alloc` (560 MB alloc) | 1.085 | 1.175 | +8% |
| `string_concat` (745 MB alloc) | 0.968 | **0.961** | parity |
| `fib_pair` | 1.127 | 1.285 | +14% |
| `substr_churn` | 1.361 | **1.354** | parity |
| `gc_combined` | 1.079 | 1.244 | +15% |

旧表では precise (`copy` 単体) は libgc と互角〜+15% 程度のばらつき
だった。 (5)〜(16) の追加 backend と realloc 修正・slab mark allocator 等を
含めた最新では、 **適切な backend を選べば libgc を全 bench で大幅に
上回る** という結果になった。

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

- **toplevel sp が 64 で hardcode** (`main.c::create_context`)。 大きな
  toplevel フレームを持つプログラムでは scratch 領域不足
- **REGION_BYTES = 512 MiB が固定**。 live set がこれを超えると OOM
- **AOT mode は moving GC 移行後に未検証** — SD bake された経路で
  precise rooting が成立しているかは要再 audit (`-c` 動作含む)

## 7. 次の段階で試したいこと

- AOT mode の再検証 (`make CCACHE_DISABLE=1` で `-c` 経路を回す)
- toplevel locals_cnt を parser から取って main.c で正しい sp を設定
- `astrogen.rb` 拡張で `@locals` を機械化 (手書きの error-prone を減らす)
- list_alloc / fib_pair / gc_combined に残る +8〜15% overhead を perf
  record で内訳分析 (sp[] spill / callee frame zero-init / copy cost
  のどれが効くか)
- region size adaptive 化 (live set に応じて grow)
- 世代別 GC backend (`gc_combined` ベンチで効くはず) を同 interface に乗せる
