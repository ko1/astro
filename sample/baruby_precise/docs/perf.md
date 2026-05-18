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

## 2. 全 GC backend のベンチ実測 (plain mode, 13 bench × 12 構成, 3-run best)

11 種類の自前 backend + 姉妹 `sample/baruby` (Boehm libgc conservative) を
**横並び 12 列**で比較。 ベンチは 13 種、 各 3-run 中の best。 単位: 秒。
行ごとの最速に `**` 印。 `libgc` 列の `life` は baruby 側の独立 bug で除外
([§3 参照](#3-libgc-列の-life-不在について))。

| Bench         | none | mark | mark\_gen | mark\_gen\_inc | copy | copy\_gen | copy\_gen\_inc | mark\_compact | mark\_compact\_gen | bump | mark\_bump\_gen | libgc |
|---------------| ------: | ------: | ------: | ------: | ------: | ------: | ------: | ------: | ------: | ------: | ------: | ------: |
| binary_trees  | 0.64 | 0.92 | 1.21 | 1.27 | 0.53 | 0.80 | 0.79 | 0.60 | 0.81 | **0.49** | 0.90 | 0.88 |
| cons_list     | 1.24 | 0.81 | 0.95 | 1.01 | 1.08 | 0.84 | 0.86 | 1.16 | **0.74** | 0.98 | 0.85 | 0.99 |
| fannkuch      | 0.72 | 0.69 | 0.73 | 0.74 | 0.73 | 0.73 | 0.71 | 0.76 | 0.72 | 0.75 | **0.66** | 0.71 |
| fib_pair      | 1.61 | 0.87 | 1.07 | 1.10 | 1.39 | 0.93 | 0.91 | 1.56 | 0.86 | 1.24 | **0.84** | 1.12 |
| gc_combined   | 1.42 | 0.92 | 1.01 | 1.11 | 1.22 | 1.00 | 0.94 | 1.32 | **0.82** | 1.26 | 0.88 | 1.05 |
| hash_chain    | 1.22 | 2.19 | 2.27 | 2.18 | 1.45 | 1.22 | 1.24 | 1.64 | 1.24 | 1.29 | **1.19** | 1.42 |
| interp_calc   | 1.35 | **0.96** | 1.14 | 1.21 | 1.20 | 1.04 | 1.01 | 1.31 | 1.02 | 1.10 | **0.96** | 1.24 |
| life          | 1.31 | 1.31 | 1.38 | 1.36 | 1.39 | 1.37 | 1.30 | 1.41 | 1.33 | 1.26 | **1.25** | — |
| list_alloc    | 1.34 | 0.82 | 0.92 | 1.06 | 1.18 | 0.91 | 0.87 | 1.29 | **0.78** | 1.11 | 0.84 | 1.05 |
| list_sort     | 1.20 | 1.21 | 1.22 | 1.23 | 1.23 | 1.08 | 1.08 | 1.28 | 1.04 | 1.11 | **1.00** | 1.01 |
| nqueens       | 0.88 | 0.94 | 0.94 | 0.95 | 0.98 | 0.96 | 0.97 | 1.00 | 0.94 | 0.94 | 1.01 | **0.88** |
| string_concat | 1.60 | 0.68 | 0.82 | 0.86 | 0.97 | 0.55 | 0.55 | 1.22 | 0.56 | 0.92 | **0.51** | 0.88 |
| substr_churn  | 1.75 | 1.11 | 1.10 | 1.07 | 1.32 | **0.88** | 0.95 | 1.64 | 0.93 | 1.21 | 0.96 | 1.23 |

**勝者分布** (2026-05-18 (24) refresh、 fannkuch 追加 + libgc 統合):
- `mark_bump_gen` が **5 bench で最速** (fannkuch / fib_pair / life /
  list_sort / string_concat) — bump nursery + tenured bump の cheapest-alloc
  パスが mutator-bound bench でも勝つ
- `mark_compact_gen` が 3 (cons_list / gc_combined / list_alloc) — long-live
  tenured workload で compaction が効く
- `bump` が 1 (binary_trees) — pure alloc-only (no GC) の floor 性能
- `mark` が 1 (interp_calc tied with mark_bump_gen)
- `mark_bump_gen` + `bump` が 1 tied (hash_chain ※bump 1.29 vs mark_bump_gen 1.19、 mark_bump_gen 単独最速)
- `copy_gen` が 1 (substr_churn) — string churn が nursery で死ぬ
- **`libgc` が 1 (nqueens、 tied with `none`)** — mutator-bound bench で
  conservative scan の overhead がほぼ見えない

驚くべきことに **新 bench `fannkuch` で libgc も含めた全 12 構成が 0.66 〜
0.76 s に収まる** (差 15%)。 これは fannkuch が integer-heavy で alloc/CPU
比率が低い ("macro" でも mutator-bound) ことを示す。 GC 差を出したい場合は
n=10 (~8s) にすべきだが run-time が伸びすぎるので n=9 で固定。

`mark_compact_gen` の cons_list (0.74 s) は全 12 構成中の全体最速 → libgc
比 -25%、 `bump` の binary_trees (0.49 s) は libgc 比 -44%、 `mark_bump_gen` の
string_concat (0.51 s) は libgc 比 -48%。 13 bench のうち **12 bench で
baruby_precise の最速 backend が libgc を上回る** (nqueens のみ tie)。
GC-only な fair 比較なので「自前で algorithm を選べる利」 がそのまま数値。

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

### ベンチカタログ (全 13 種)

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

## 3. libgc 列の `life` 不在について

`sample/baruby` (libgc) は top-level の long while loop 後の最終式値が
壊れる独立バグを抱えていて `bench/life.ba.rb` で誤った結果を返す。 GC
側のバグではないが fair 比較にならないため §2 table の libgc 列は `life`
を空欄 (`—`) としている。 baruby_precise の `life` 行 (11 backend で測定)
は table に残してある。 残り 12 bench は baruby (libgc) も同一スクリプトを
走らせて取得した数値で、 GC algorithm 以外の差分は無い。 parser fix
(commit 34be8d2) と fannkuch (commit このイテレーション) は baruby に
port 済。

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
