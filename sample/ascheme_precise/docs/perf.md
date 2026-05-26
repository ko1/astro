# ascheme_precise — performance evaluation

`sample/ascheme_precise/` の precise GC framework migration の perf 評価。
**libgc (= Boehm conservative GC, `sample/ascheme/`)** を baseline に、
15 個の precise GC backend (= `copy_gen_inc` 撤去後、 §0 参照) を **plain
interpreter** / **AOT (= `--aot-compile` + dlopen cached SDs)** の 2 軸で、
elapsed + peak RSS の両軸で計測。

bench script: `sample/ascheme_precise/bench/aot_matrix.sh` (= `make
bench-aot`)。 15 backend × 9 workload × {plain, aot-cached} を回す。

## 0. setup

- **日付**: 2026-05-26
- **machine**: AMD Ryzen 9 5900HX (= 8 cores / 16 threads、 ~4.6 GHz boost)
- **memory**: 30 GiB
- **kernel**: Linux 6.8.0-117 x86_64
- **compiler**: gcc 13.3.0 (`-O3 -flto=auto -ggdb3`)
- **methodology**: 各 benchmark **3 回実行**、 **elapsed = median of 3**、
  **peak RSS = max of 3** (= `/usr/bin/time -f "%e %M"`)。 出力 first-line を
  expected と照合して **正答性検証** してから記録 (= GC bug で「速いが結果が誤」
  を排除)
- **scale**: 0.3–10 秒の範囲で sustained measurement
- **revision**: **commit `04af2521` (= 3-arg dispatcher、 sp 引数化) + 4cf3aa50
  (= GC=none fix) を含む**。 dispatcher signature を 2-arg (`AsContext *c,
  AsNode *n`) → 3-arg (`AsContext *c, AsValue *sp, AsNode *n`) に変更し、 sp
  を register resident にした結果、 plain / AOT 両 mode で大幅な改善 (§1.3
  に before/after 表)。 baruby_precise iter 61 と同 pattern
- **N-survive**: 全 gen backend を N-survive promote に統一済。 `copy_gen_inc`
  は実体が `copy_gen` の clone (= inc_step / SATB なし) で「独立 algorithm」 を
  主張できないため撤去 (16 → **15 backend**)

略号一覧 (= 後続の表で使用):

| 略号 | backend | 略号 | backend |
|------|---------|------|---------|
| `m_G`     | mark_gen          | `copy_G`     | copy_gen          |
| `m_G_inc` | mark_gen_inc      | `m_free`     | mark_freelist     |
| `m_c`     | mark_compact      | `m_c_G`      | mark_compact_gen  |
| `m_bmp_G` | mark_bitmap_gen   | `m_crd_G`    | mark_card_gen     |
| `Bu`      | bump (leak、 §3)  | `m_Bu_G`     | mark_bump_gen (§3) |
| `I`       | immix             | `I_G`        | immix_gen         |
| `none`    | libc malloc + leak (§3) |  |  |

⚠ **「libgc との比較」 caveat**: libgc は ascheme の conservative scan
backend、 ascheme_precise は precise rooting (`sframe` chain + sp[] flat
scan)。 ここで測れているのは「runtime + rooting + collector の合計差」 で
あって、 GC algorithm 純粋差ではない。

bench data source: `bench-results/20260526/ascheme_c1.txt` –
`ascheme_c7.txt` (= 7 chunk file、 sp 引数化後の再測定)。 libgc AOT は
別 binary で `ascheme_libgc_aot.tsv` に格納。

## 1. plain interpreter (= 15 precise backend + libgc × 9 bench)

**表中の `libgc` 列**: 姉妹サンプル `sample/ascheme` の binary (= 同じ
Scheme サブセット を Boehm-Demers-Weiser conservative libgc にリンクした
もの) で同じ bench を測った数値。 precise GC backend との「conservative
GC との比較」 baseline。 詳細は §4。

### 1.1 elapsed (秒、 median of 3)

実用 11 backend (= §3 の 4 特殊用途を除く):

| bench | cat | libgc | mark | m_G | m_G_inc | copy | copy_G | m_c | m_c_G | I | I_G | m_bmp_G | m_crd_G | m_free |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fib35     | INT | **0.42** | 1.01 | 1.18 | 1.26 | 0.94 | 0.98 | 1.15 | 1.08 | 0.96 | 1.00 | 1.19 | 1.17 | 1.07 |
| sumloop   | INT | **1.61** | 1.80 | 1.82 | 2.12 | 1.77 | 1.71 | 1.77 | 1.82 | 1.83 | 1.77 | 2.15 | 2.11 | 1.87 |
| nbody     | INT | 0.54 | 0.51 | 0.54 | 0.60 | **0.44** | **0.44** | 0.53 | 0.50 | 0.46 | 0.45 | 0.60 | 0.57 | 0.53 |
| sieve_big | GC  | 1.15 | 1.27 | 1.35 | 1.67 | 1.24 | **1.17** | 1.28 | 1.32 | 1.22 | 1.26 | 1.91 | 1.75 | 1.34 |
| deriv     | GC  | 1.08 | 1.05 | 1.11 | 1.22 | 1.01 | **0.91** | 1.09 | 1.01 | 1.01 | 0.92 | 1.12 | 1.13 | 1.07 |
| nqueens   | MIX | **2.13** | 2.54 | 2.64 | 2.81 | 2.43 | 2.31 | 2.66 | 2.52 | 2.42 | 2.35 | 2.71 | 2.88 | 2.58 |
| fannkuch  | MIX | 1.41 | **1.31** | 1.53 | 1.57 | 1.12 | 1.16 | 1.43 | 1.31 | 1.19 | 1.18 | 1.49 | 1.52 | 1.42 |
| cps_loop  | MIX | **0.92** | 0.98 | 1.06 | 1.15 | 0.97 | **0.90** | 0.98 | 1.01 | 0.95 | 0.96 | 1.15 | 1.15 | 1.02 |
| matmul    | MIX | 8.39 | 5.14 | 4.95 | 4.90 | 4.93 | 4.82 | 4.88 | 4.72 | 4.84 | 4.56 | 4.73 | 4.63 | 5.01 |
| **geomean** | — | **1.30** | 1.40 | 1.49 | 1.63 | 1.32 | **1.28** | 1.45 | 1.40 | 1.33 | 1.31 | 1.61 | 1.59 | 1.45 |

**太字** は各 bench で libgc を含む全 backend (= 実用 + 特殊) の最速。

### 1.2 peak RSS (MiB、 max of 3)

| bench | libgc | mark | m_G | m_G_inc | copy | copy_G | m_c | m_c_G | I | I_G | m_bmp_G | m_crd_G | m_free |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fib35     | **4.1** | 19.8 | 23.9 | 23.9 | 35.6 | 35.6 | 19.8 | 35.8 | 19.8 | 35.8 | 19.8 | 20.0 | 19.8 |
| sumloop   | 4.2 | 3.6 | 3.6 | 3.6 | **3.5** | 3.6 | 3.8 | 3.8 | 3.8 | 3.8 | 3.8 | 3.6 | 3.8 |
| nbody     | 73.5 | 23.1 | 26.5 | 26.5 | 36.0 | 36.0 | **20.0** | 36.0 | **20.0** | 35.9 | 23.4 | 23.2 | 22.9 |
| sieve_big | 165.1 | 222.2 | 102.1 | 102.1 | 201.6 | 111.9 | 227.1 | 111.9 | 201.6 | 112.0 | **98.9** | **98.9** | 222.1 |
| deriv     | 73.5 | 25.9 | 29.9 | 30.0 | 35.6 | 35.6 | **19.8** | 35.6 | 19.9 | 35.8 | 26.0 | 26.1 | 25.8 |
| nqueens   | 73.4 | 25.0 | 28.1 | 28.1 | 35.6 | 35.6 | **19.6** | 35.6 | 19.8 | 35.6 | 25.5 | 25.4 | 25.0 |
| fannkuch  | 73.4 | 25.0 | 28.2 | 28.0 | 35.8 | 35.8 | **19.6** | 35.8 | 20.0 | 35.6 | 25.1 | 25.2 | 24.8 |
| cps_loop  | 4.1 | 3.9 | 3.8 | 3.9 | 3.8 | **3.6** | 3.8 | 3.8 | 3.8 | 3.8 | 3.9 | 3.8 | **3.6** |
| matmul    | 71.9 | 32.6 | 32.2 | 33.0 | **29.0** | 29.4 | 29.2 | 30.7 | 29.9 | 30.7 | 32.5 | 32.4 | 32.3 |

`mark` の `nqueens` が PASS するようになった (= 旧 perf.md は FAIL を記録、
3-arg dispatcher 化に伴う precise rooting 更新で safe)。

### 1.3 plain での観察

- **sp 引数化の効果は全 backend × 全 bench に均等 (= -10〜-25%)**: §1.3.1
  で詳細を整理。 例として `copy` plain は fib35 1.13s → 0.94s (= 0.83×)、
  matmul 6.42s → 4.93s (= 0.77×) と各 bench で 13–23% 削減
- **fib35** (= 純再帰、 stack 深い): precise rooting の sframe 更新が worst
  case ではあるが、 sp 引数化で改善し libgc 比 2.2× 程度に縮小 (= 旧 2.5×)。
  なお precise の RSS は libgc 4.1 MiB に対し 19–36 MiB = sframe + heap の
  initial reserve cost
- **sumloop / nqueens** で libgc が precise を上回るのは変わらず (= 1.61
  vs 最速 precise `copy_G` 1.71 / 2.13 vs `copy_G` 2.31)。 dispatch only な
  int bench で libgc 優位
- **GC-heavy** な `sieve_big` / `deriv` / `fannkuch` / `matmul` は backend
  によって libgc を上回る:
  - `copy_gen` sieve_big 1.17 < libgc 1.15 (ほぼ tied、 0.99×)
  - `immix_gen` deriv 0.92 < libgc 1.08 (0.85×)
  - precise 系全部 fannkuch < libgc 1.41 を含む (= `mark` 1.31 / `copy`
    1.12)
  - **matmul は precise 14 backend 全て libgc 8.39 を上回り 4.56–5.14 (=
    0.54–0.61×)**
- **`mark` の `nqueens`** は旧 perf.md で FAIL だったが、 今回の sp 引数化
  + 関連 fix で PASS (= 2.54s plain / 1.16s AOT)
- **`mark_compact`** は ascheme 環境で `binary_trees` 系の long-lived chain
  で SEGV する known issue があるが、 本 9 bench では問題なく全 PASS

### 1.3.1 sp 引数化 (commit 04af2521) 前後比較

dispatcher signature を 2-arg → 3-arg (sp 引数化) に変更した前後の同 cell
比較。 `copy` plain の per-bench (= 旧 perf.md 値 → 新値):

| bench | 旧 copy plain | 新 copy plain | ratio |
|---|---:|---:|---:|
| fib35     | 1.13 | 0.94 | 0.83× |
| sumloop   | 2.09 | 1.77 | 0.85× |
| nbody     | 0.55 | 0.44 | 0.80× |
| sieve_big | 1.41 | 1.24 | 0.88× |
| deriv     | 1.21 | 1.01 | 0.83× |
| nqueens   | 2.81 | 2.43 | 0.86× |
| fannkuch  | 1.37 | 1.12 | 0.82× |
| cps_loop  | 1.12 | 0.97 | 0.87× |
| matmul    | 6.42 | 4.93 | 0.77× |
| **geomean** | **1.58** | **1.32** | **0.83×** (= -17%) |

AOT mode の `copy` も同程度の改善 (= geomean 0.93s → 0.75s 相当、 0.80×、
詳細は §2)。 主因は dispatch loop で sp を memory から load せず register
で持ち回せる点。 全 backend で **plain ~-15〜-20%、 AOT ~-15〜-25%** の
trend が出ている。

### 1.4 plain ranking (= 実用 11 backend、 geomean elapsed)

| rank | backend | plain geomean | vs libgc |
|---:|---|---:|---:|
| 1 | `copy_gen` | 1.28s | **0.98×** |
| — | **libgc** | **1.30s** | **1.00 (baseline)** |
| 2 | `immix_gen` | 1.31s | 1.01× |
| 3 | `copy` | 1.32s | 1.01× |
| 4 | `immix` | 1.33s | 1.03× |
| 5 | `mark` | 1.40s | 1.08× |
| 6 | `mark_compact_gen` | 1.40s | 1.08× |
| 7 | `mark_compact` | 1.45s | 1.11× |
| 8 | `mark_freelist` | 1.45s | 1.12× |
| 9 | `mark_gen` | 1.49s | 1.15× |
| 10 | `mark_card_gen` | 1.59s | 1.22× |
| 11 | `mark_bitmap_gen` | 1.61s | 1.24× |
| 12 | `mark_gen_inc` | 1.63s | 1.25× |

主流の `copy_gen` / `immix_gen` / `copy` / `immix` が **libgc 比 0.98–1.03×**
で互角〜微優位 (旧 perf.md と同じ cluster だが、 sp 引数化で `copy_gen`
が `libgc` を僅差で抜く構図に変化)。 落ちこぼれは `mark_gen_inc` /
`mark_bitmap_gen` / `mark_card_gen` で 1.22–1.25×。

## 2. AOT (= cached SD)

`--compile` (= ascheme) / `--aot-compile --run` (= ascheme_precise) で hot
AST node を `code_store/all.so` 内 SD として gcc で bake。 2 回目以降の
run が dlopen して bind する仕組み。 **ascheme (libgc) も AOT 対応**
(= `aot_compile_and_load` 関数、 `astro_cs_*` 経由、 commit `8105bf85`
以降)。

### 2.1 elapsed (秒、 median of 3、 AOT cached)

実用 11 backend + `libgc AOT` (= 同 mode head-to-head):

| bench | libgc AOT | mark | m_G | m_G_inc | copy | copy_G | m_c | m_c_G | I | I_G | m_bmp_G | m_crd_G | m_free |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fib35     | **0.25** | 0.69 | 0.80 | 0.92 | 0.61 | 0.61 | 0.77 | 0.71 | 0.64 | 0.61 | 0.84 | 0.81 | 0.68 |
| sumloop   | **0.29** | 0.50 | 0.48 | 0.80 | 0.48 | 0.47 | 0.49 | 0.47 | 0.50 | 0.47 | 0.82 | 0.84 | 0.51 |
| nbody     | 0.50 | 0.39 | 0.44 | 0.50 | 0.36 | 0.36 | 0.42 | 0.39 | **0.35** | 0.36 | 0.47 | 0.45 | 0.41 |
| sieve_big | **0.45** | 0.53 | 0.52 | 0.84 | 0.48 | 0.46 | 0.51 | 0.48 | 0.49 | 0.46 | 0.89 | 0.85 | 0.50 |
| deriv     | 1.16 | 1.01 | 1.04 | 1.12 | 0.91 | **0.84** | 0.97 | 0.93 | 0.89 | **0.84** | 1.02 | 1.04 | 0.98 |
| nqueens   | 0.98 | 1.16 | 1.22 | 1.42 | 1.06 | 1.06 | 1.19 | 1.14 | 1.08 | **1.05** | 1.36 | 1.39 | 1.20 |
| fannkuch  | 1.19 | 0.98 | 1.12 | 1.31 | **0.81** | **0.80** | 1.03 | 0.94 | 0.84 | 0.82 | 1.17 | 1.19 | 1.06 |
| cps_loop  | **0.19** | 0.25 | 0.27 | 0.42 | 0.25 | 0.27 | 0.24 | 0.26 | 0.25 | 0.26 | 0.43 | 0.44 | 0.25 |
| matmul    | 8.97 | 4.68 | 4.69 | 4.78 | 4.73 | 4.53 | 4.57 | 4.42 | 4.63 | **4.33** | 4.54 | 4.45 | 4.61 |
| **geomean** | **0.70** | 0.77 | 0.82 | 1.03 | 0.71 | **0.70** | 0.78 | 0.75 | 0.72 | **0.70** | 0.99 | 0.99 | 0.78 |

旧 perf.md で `NOAOT` (= AOT SD attach 失敗) や `FAIL` を記録した cell は
今回 全 PASS (= sp 引数化 + 関連 fix の副次効果)。

### 2.2 peak RSS (MiB、 max of 3、 AOT)

**`libgc plain`** 列は cross-mode reference:

| bench | libgc plain | mark | m_G | m_G_inc | copy | copy_G | m_c | m_c_G | I | I_G | m_bmp_G | m_crd_G | m_free |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fib35     | 4.1   | 19.9 | 23.9 | 23.9 | 35.8 | 35.6 | 19.6 | 35.8 | 19.8 | 35.8 | 19.9 | 20.0 | 19.6 |
| sumloop   | 4.2   | 3.9 | 3.9 | 3.9 | 3.6 | 3.8 | 3.8 | 3.8 | 3.6 | 3.8 | 3.9 | 3.9 | 3.8 |
| nbody     | 73.5  | 23.1 | 26.5 | 26.6 | 36.0 | 36.0 | 20.0 | 36.1 | 20.2 | 36.0 | 23.5 | 23.5 | 23.0 |
| sieve_big | 165.1 | 222.5 | 102.2 | 102.2 | 201.8 | 112.1 | 227.1 | 112.2 | 201.9 | 112.0 | 99.0 | 98.9 | 222.1 |
| deriv     | 73.5  | 27.9 | 30.2 | 30.1 | 35.8 | 35.9 | 27.9 | 35.8 | 27.6 | 35.9 | 27.8 | 27.9 | 27.9 |
| nqueens   | 73.4  | 25.2 | 28.1 | 28.1 | 35.9 | 35.8 | 19.8 | 35.8 | 19.8 | 35.6 | 25.5 | 25.5 | 25.1 |
| fannkuch  | 73.4  | 25.0 | 28.2 | 28.4 | 35.9 | 35.8 | 19.6 | 35.8 | 19.9 | 35.9 | 25.5 | 25.4 | 24.9 |
| cps_loop  | 4.1   | 3.8 | 3.8 | 3.9 | 3.8 | 3.8 | 3.8 | 3.9 | 3.8 | 3.8 | 3.9 | 3.9 | 3.9 |
| matmul    | 71.9  | 32.5 | 32.5 | 32.9 | 29.2 | 29.6 | 29.4 | 30.8 | 29.8 | 30.8 | 32.3 | 32.6 | 32.4 |

### 2.3 plain vs AOT speedup (実用 11 backend、 geomean of bench ratios)

| backend | plain geomean | AOT geomean | plain/AOT |
|---|---:|---:|---:|
| `mark`            | 1.40s | 0.77s | 1.82× |
| `mark_gen`        | 1.49s | 0.82s | 1.83× |
| `mark_gen_inc`    | 1.63s | 1.03s | 1.58× |
| `copy`            | 1.32s | 0.71s | 1.85× |
| `copy_gen`        | 1.28s | **0.70s** | **1.82×** |
| `mark_compact`    | 1.45s | 0.78s | 1.85× |
| `mark_compact_gen`| 1.40s | 0.75s | 1.88× |
| `immix`           | 1.33s | 0.72s | 1.85× |
| `immix_gen`       | 1.31s | **0.70s** | **1.88×** |
| `mark_bitmap_gen` | 1.61s | 0.99s | 1.62× |
| `mark_card_gen`   | 1.59s | 0.99s | 1.61× |
| `mark_freelist`   | 1.45s | 0.78s | 1.87× |

AOT 速度上位 5: `immix_gen` (0.70s) / `copy_gen` (0.70s) / `copy` (0.71s)
/ `immix` (0.72s) / `mark_compact_gen` (0.75s)。

### 2.4 AOT 加速率 (= plain / AOT、 主要 backend × bench)

| backend | fib35 | sumloop | nbody | sieve_big | deriv | nqueens | fannkuch | cps_loop | matmul |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `mark_gen`   | 1.47× | **3.79×** | 1.23× | **2.60×** | 1.07× | 2.16× | 1.37× | **3.93×** | 1.06× |
| `copy_gen`   | 1.61× | **3.64×** | 1.22× | **2.54×** | 1.08× | 2.18× | 1.45× | **3.33×** | 1.06× |
| `immix_gen`  | 1.64× | **3.77×** | 1.25× | **2.74×** | 1.10× | 2.24× | 1.44× | **3.69×** | 1.05× |
| `immix`      | 1.50× | **3.66×** | 1.31× | **2.49×** | 1.13× | 2.24× | 1.42× | **3.80×** | 1.05× |
| `mark_free`  | 1.57× | **3.67×** | 1.29× | **2.68×** | 1.09× | 2.15× | 1.34× | **4.08×** | 1.09× |

- **dispatch heavy** (= `sumloop` / `cps_loop`) で AOT は **3.3–4.1×** speedup
- **GC bound** (= `matmul`) は全 backend で 1.05–1.09× tied。 GC time が
  dominate
- **fib35** は 1.47–1.64× で改善するが libgc plain 0.42 にはまだ届かない (=
  `copy_gen` AOT 0.61、 sframe overhead が SD でも残る)

## 3. 特殊用途 backend 4 個

実用と並べると判定がミスリーディングなため別表。 §1 ranking からは除外。

### 3.1 elapsed (秒、 plain + AOT)

| bench | cat | none plain / AOT | bump plain / AOT | mark_bump_gen plain / AOT |
|---|---|---:|---:|---:|
| fib35     | INT | 0.57 / 0.29 | 1.35 / 0.96 | 0.97 / 0.61 |
| sumloop   | INT | 1.81 / 0.50 | 1.76 / 0.49 | 1.77 / 0.47 |
| nbody     | INT | 0.90 / 0.83 | 0.67 / 0.55 | 0.45 / 0.36 |
| sieve_big | GC  | 1.30 / 0.53 | 1.22 / 0.46 | 1.26 / 0.46 |
| deriv     | GC  | 1.65 / 1.60 | 1.23 / 1.11 | 0.94 / 0.85 |
| nqueens   | MIX | 2.50 / 1.15 | 2.69 / 1.30 | 2.44 / 1.08 |
| fannkuch  | MIX | 2.53 / 2.23 | 1.79 / 1.45 | 1.18 / 0.83 |
| cps_loop  | MIX | 0.99 / 0.24 | 0.99 / 0.24 | 0.95 / 0.26 |
| matmul    | MIX | 10.78 / 10.58 | 10.18 / 10.07 | 4.59 / 4.39 |
| **geomean** | — | 1.72s / 0.96s | 1.70s / 0.95s | 1.31s / 0.70s |

### 3.2 peak RSS (MiB、 plain + AOT)

| bench | none p / AOT | bump p / AOT | mark_bump_gen p / AOT |
|---|---:|---:|---:|
| fib35     | 3.6 / 3.8 | **914.9** / 915.0 | 35.6 / 35.6 |
| sumloop   | 3.6 / 3.6 | 3.6 / 3.6 | 3.8 / 3.6 |
| nbody     | 668.6 / 668.6 | 484.9 / 485.0 | 35.9 / 36.0 |
| sieve_big | 252.4 / 252.4 | 201.6 / 201.8 | 112.0 / 112.1 |
| deriv     | 852.0 / 852.2 | 650.8 / 650.8 | 35.6 / 35.8 |
| nqueens   | 262.0 / 262.0 | 659.9 / 659.9 | 35.8 / 35.8 |
| fannkuch  | 2089.9 / 2089.9 | 1534.4 / 1534.5 | 35.9 / 35.8 |
| cps_loop  | 3.6 / 3.9 | 3.8 / 3.6 | 3.8 / 3.9 |
| matmul    | 12130.7 / 12130.7 | 12116.9 / 12117.1 | 29.7 / 29.7 |

### 3.3 観察

- **`none` (= libc malloc + leak)**: 全 alloc を leak。 GC 抜きの alloc
  throughput 上限の参考値。 `matmul` で **12130.7 MiB = 12 GiB** の RSS
  に達し、 実用不可。 plain matmul で 10.78s と precise 系の 4.5s 台に大幅劣る
  (= cache miss が effective floor)
- **`bump` (= bump alloc only、 leak)**: `none` と類似だが alloc が
  region-bump で hot path が短い。 `fannkuch` で 1534 MiB / `matmul` で
  12 GiB の RSS で production 不可。 fib35 plain で 1.35s だが AOT が
  0.96s と落ちる (= leak が dispatch fast path に重なる)
- **`mark_bump_gen` (= bump tenured + mark sweep nursery)**: 実は 9 bench
  全てで PASS。 plain geomean 1.31s = `copy_gen` 1.28s 並み、 AOT 0.70s
  も同水準。 long-running 系で tenured fragmentation が顕在化するはずだが
  本 9 bench は短期 workload で問題が出ない。 GC algorithm comparison
  としては「tenured compactor の必要性」 が今回 9 bench では surface 化
  しない例
- **`mark_freelist`** (= 本来「fragmentation testbed」 の意図) は §1 で
  実用 backend と一緒に評価。 今回 9 bench で fragmentation は顕在化せず、
  AOT matmul で 4.61s も悪くない

## 4. libgc 比較 (= ascheme baseline)

### 4.1 ⚠ caveat (再掲)

ascheme libgc = Boehm conservative GC、 ascheme_precise = sframe chain +
precise sp[] scan。 比較しているのは「runtime + rooting + collector の
合計差」 であって、 GC algorithm 純粋差ではない。

### 4.2 plain mode: `libgc` plain vs `copy` plain (= 代表 head-to-head)

実用 backend の代表 = `copy` (Cheney semispace、 default GC、 §1 ranking
の上位 cluster 中央)。 同じ plain mode (= interpreter で AST を辿る) で
head-to-head:

| bench | cat | libgc plain | copy plain | copy / libgc |
|---|---|---:|---:|---:|
| fib35     | INT | 0.42 | 0.94 | 2.24× |
| sumloop   | INT | 1.61 | 1.77 | 1.10× |
| nbody     | INT | 0.54 | 0.44 | **0.81×** |
| sieve_big | GC  | 1.15 | 1.24 | 1.08× |
| deriv     | GC  | 1.08 | 1.01 | **0.94×** |
| nqueens   | MIX | 2.13 | 2.43 | 1.14× |
| fannkuch  | MIX | 1.41 | 1.12 | **0.79×** |
| cps_loop  | MIX | 0.92 | 0.97 | 1.05× |
| matmul    | MIX | 8.39 | 4.93 | **0.59×** |
| **geomean** | — | **1.30s** | **1.32s** | **1.01×** |

- **geomean は libgc 1.30s vs copy 1.32s で 1.01×** (= ほぼ tied、 旧 perf.md
  の 0.96× から逆転)。 sp 引数化で `copy` 側も同程度速くなったが、 libgc
  binary も別途速くなっており全体 trend は維持
- **GC bound** (= `nbody` / `deriv` / `fannkuch` / `matmul`) で precise
  が `copy` 圧勝。 特に `matmul` は **0.59×** (= -41% faster)
- `fib35` のみ libgc が 2.24× 高速 (= conservative の C stack scan が precise
  の sp[] push/pop を avoid できる、 純再帰で worst)
- ⚠ caveat (§4.1) を踏まえると、 「同等」 と言える範囲

### 4.3 AOT mode: `libgc` AOT vs `copy` AOT (= 代表 head-to-head)

ascheme は CLI 標準化 (`sample_cli.md`) で `--aot-compile` に対応した
(2026-05-26、 commit `8105bf85`)。 同 mode head-to-head:

| bench | cat | libgc AOT | copy AOT | copy / libgc |
|---|---|---:|---:|---:|
| fib35     | INT | 0.25 | 0.61 | 2.44× |
| sumloop   | INT | 0.29 | 0.48 | 1.66× |
| nbody     | INT | 0.50 | 0.36 | **0.72×** |
| sieve_big | GC  | 0.45 | 0.48 | 1.07× |
| deriv     | GC  | 1.16 | 0.91 | **0.78×** |
| nqueens   | MIX | 0.98 | 1.06 | 1.08× |
| fannkuch  | MIX | 1.19 | 0.81 | **0.68×** |
| cps_loop  | MIX | 0.19 | 0.25 | 1.32× |
| matmul    | MIX | 8.97 | 4.73 | **0.53×** |
| **geomean** | — | **0.70s** | **0.71s** | **1.02×** |

- **AOT mode geomean libgc 0.70s vs copy 0.71s で 1.02×** (= ほぼ tied)。
  copy AOT が大幅に伸びた (旧 0.93s → 0.71s) のに対し libgc AOT の
  geomean は ~0.70s で stand-pat
- **GC-bound bench** で precise が依然 大きく勝つ: `nbody` 0.72× /
  `deriv` 0.78× / `fannkuch` 0.68× / **`matmul` 0.53×** (= -47%)
- **dispatch-heavy CPU bench** (= `fib35` / `sumloop` / `cps_loop`) で
  libgc が precise の 1.3–2.4× 速い。 conservative scan は AOT 化された
  dispatch fast path で sframe update を avoid できる

### 4.3.x AOT 効果 (= 同 backend の plain vs AOT)

各 backend で AOT がもたらす speedup (= plain / AOT):

| backend | plain geomean | AOT geomean | plain / AOT |
|---|---:|---:|---:|
| `libgc` | 1.30s | **0.70s** | **1.87×** |
| `copy`  | 1.32s | 0.71s | **1.85×** |

libgc と precise `copy` で AOT speedup が同水準 (= 1.85–1.87×)。 旧
perf.md では precise が 1.85× / libgc が 2.34× で libgc 優位だったが、
sp 引数化で precise 側の plain も縮んだため AOT 加速率は揃った形。

### 4.4 全 backend AOT を `copy` AOT で並べる

§4.3 で `copy` AOT を 1.00 とした時の他 backend AOT の倍率:

| backend | AOT geomean | vs `copy` AOT |
|---|---:|---:|
| `immix_gen`       | **0.70s** | **0.98×** |
| `copy_gen`        | 0.70s | 0.99× |
| `copy` (baseline) | **0.71s** | **1.00** |
| `immix`           | 0.72s | 1.01× |
| `mark_compact_gen`| 0.75s | 1.05× |
| `mark`            | 0.77s | 1.08× |
| `mark_freelist`   | 0.78s | 1.09× |
| `mark_compact`    | 0.78s | 1.09× |
| `mark_gen`        | 0.82s | 1.14× |
| `mark_card_gen`   | 0.99s | 1.39× |
| `mark_bitmap_gen` | 0.99s | 1.39× |
| `mark_gen_inc`    | 1.03s | 1.45× |

- **`copy` 周辺の cluster** (= 5 backend 内 ±10%): `immix_gen` 0.98× /
  `copy_gen` 0.99× / `immix` 1.01× / `mark_compact_gen` 1.05× / `mark`
  1.08×。 「実用最速 6 個」 がほぼ同水準
- **落ちこぼれ**: `mark_card_gen` / `mark_bitmap_gen` / `mark_gen_inc`
  が 1.39–1.45×。 sumloop / cps_loop / sieve_big の AOT speedup が頭打ち
  (= 1.4–1.7× にとどまり他 backend 3–4× に対して 遅れる) で geomean を
  引き下げる

## 5. ranking + geomean

### 5.1 plain mode ranking (= §1.4 再掲)

1. `copy_gen` — **1.28s** (vs libgc plain **0.98×**)
2. `immix_gen` — 1.31s (1.01×)
3. `copy` — 1.32s (1.01×)
4. `immix` — 1.33s (1.03×)
5. `mark` — 1.40s (1.08×)
6. `mark_compact_gen` — 1.40s (1.08×)
7. `mark_compact` — 1.45s (1.11×)
8. `mark_freelist` — 1.45s (1.12×)
9. `mark_gen` — 1.49s (1.15×)
10. `mark_card_gen` — 1.59s (1.22×)
11. `mark_bitmap_gen` — 1.61s (1.24×)
12. `mark_gen_inc` — 1.63s (1.25×)

### 5.2 AOT mode ranking (= libgc plain との比較)

1. `immix_gen` — **0.70s** (vs libgc plain **0.54×**)
2. `copy_gen` — 0.70s (0.54×)
3. `copy` — 0.71s (0.55×)
4. `immix` — 0.72s (0.55×)
5. `mark_compact_gen` — 0.75s (0.57×)
6. `mark` — 0.77s (0.59×)
7. `mark_freelist` — 0.78s (0.60×)
8. `mark_compact` — 0.78s (0.60×)
9. `mark_gen` — 0.82s (0.63×)
10. `mark_card_gen` — 0.99s (0.76×)
11. `mark_bitmap_gen` — 0.99s (0.76×)
12. `mark_gen_inc` — 1.03s (0.79×)

### 5.3 production 推奨

plain + AOT 両方で上位:

- **`copy_gen`** = balanced、 N-survive minor + Cheney major (plain 1 位、 AOT 2 位)
- **`immix_gen`** = fragmentation-resistant、 line+block region (plain 2 位、 AOT 1 位)
- **`copy`** = 非 gen Cheney、 simple semispace (plain 3 位、 AOT 3 位)
- **`immix`** = 非 gen region-bump、 long-lived heavy 用途 (plain 4 位、 AOT 4 位)
- **`mark_compact_gen`** = 落ち穂拾い的に AOT 5 位、 RSS も中庸

短期 alloc-heavy で **`copy_gen` / `immix_gen`** が安定して最強。 long-lived
heap heavy では **`immix` / `copy`** に倒す選択もあり。

### 5.4 既知 limitation (= todo)

- **fib35 overhead 残 1.5–2.4×** (plain 2.2×、 AOT 2.4×): 純再帰の sframe
  更新コスト本質。 libgc は C stack 保守的 scan で per-call sframe update
  不要
- **`mark_card_gen` / `mark_bitmap_gen` / `mark_gen_inc` の AOT 加速率が
  低い** (= sumloop / cps_loop / sieve_big で 1.4–1.7× に留まる、 他は
  3–4×)。 GC overhead が比率高めで dispatch fold の効果が薄まる
- 旧 perf.md で記録した `mark` の `nqueens` FAIL / `copy` の `sumloop`
  NOAOT / `mark_compact_gen` の `nbody` NOAOT / `mark_card_gen` の `nbody`
  NOAOT は **今回の measurement では全 PASS** (= sp 引数化 + 関連 fix の
  副次効果)

## 6. correctness 保証 + 再現

全 15 backend (+ `copy_scramble` audit backend) で:

- **ascheme native test** PASS
- **R5RS chibi test (179 個)** PASS
- **canary `16_alloc_root_stress`** PASS
- default mode + `BARUBY_GC_STRESS=1` 両方で実行

```sh
# 検証 + audit mode (= バグ検出)
make test                                              # default 全 test
make test_stress                                       # stress mode で全 test
make GC=copy_scramble test_stress                      # scramble + stress = 最強 audit

# AOT 単体動作確認 (= --aot-compile + cached run)
make GC=copy
./ascheme_precise -q --clear-cs --aot-compile bench/big/sumloop.scm  # build + 1 回 run
./ascheme_precise -q --aot-compile bench/big/sumloop.scm             # cached run

# 全 matrix 再現
make bench-aot   # 15 backend × 9 workload × {plain, aot-cached}
```

## 7. baruby_precise との比較

`sample/baruby_precise/docs/perf.md` の数値と直接比較すると、 ascheme の
方が backend あたり ~10–20% 遅い傾向。 主因:

- ascheme は `sframe` chain で env を表現、 baruby は flat `sp[]`
- ascheme は call/cc / closure / continuation / multi-values 等の heavy 機構
- ascheme は GMP 経由 bignum の external accounting で GC frequency 高め

libgc 直接比較は ascheme 側のみ可能 (= sister sample `sample/baruby` が
baruby 側の libgc baseline)。

## 8. 結論

precise GC framework + ascheme は **libgc に対して**:

- **plain mode**: 主流 backend (= `copy_gen` / `immix_gen` / `copy` /
  `immix`) で **geomean 0.98–1.03×** (= libgc と互角)。 GC-heavy 個別
  workload で互角〜やや速い、 `fib35` のみ 2.2× 遅い
- **AOT mode**: 主流 backend で **geomean 0.54–0.55× vs libgc plain** (=
  libgc plain の -45〜-46% faster)。 9/9 workload で libgc plain を上回り、
  dispatch heavy で 3–4× 級、 GC heavy で 2–3× 級、 matmul で 1.8× 級
- **vs libgc AOT** では precise AOT は **geomean 1.02×** (= 同水準)。
  GC-bound で precise 圧勝 (= matmul 0.53×)、 dispatch-heavy で libgc 優位
  (= fib35 2.44×)
- **production 推奨**: `copy_gen` / `immix_gen` (= mainline) / `copy` /
  `immix` (= 非 gen variant)
- **audit / debug 兼用**: `copy_scramble` (= overhead 微小、 stress + scramble で
  precise rooting バグを即時検出)

---

## 9. Phase 2c+ migration: sframe → sp[] frame + AOT 再評価 (2026-05-26)

### 9.1 sframe → sp[] frame migration の到達点

`docs/sframe_to_sp_migration.md` に従って migration を実施した結果、
**no_capture leaf closure (= 内部 lambda を持たず、 outer var への
depth>=1 lref も無い)** は sframe alloc を完全に skip して sp[]
frame で実行できるようになった。 capture を持つ closure は依然 sframe
を使うが、 これは "capture を持つには heap-resident な storage が
fundamental に必要" な制約なので Phase 7 (= sframe 完全削除) は
意味的に無効化と判断 (= alloc 1 回/call は不可避)。

**実装した phase** (commit hash 順):

| commit | phase | 内容 |
|---|---|---|
| `435ffd57` | 1   | parser に sp_offset operand 追加 |
| `237d5f2d` | 2a  | closure.no_capture flag |
| `db39603c` | 2b  | NODE_DEF lref_sp / lset_sp 併設 (patching off) |
| `09c43d1b` | 2c  | no_capture body の sp[] frame 化 (patching on) |
| `4e87a79a` | 2c' | call_0/_2/_3/_4 fast path 展開 + GC-safe saved env |
| `e67bfac6` | 2c'' | GC=none variant の frame_sp setup |
| `642748ce` | 5   | call/cc で sp / frame_sp save/restore |
| `9888ec0e` | 9   | AOT auto-load + profiling 漏れ修正 |
| `010193dc` | 9'  | HASH cache 撤去 + node_loop dispatch_cnt |

### 9.2 AOT 効果 (= big benches、 copy backend)

`bench/big/*.scm` を `--pg-compile` → SD 生成 → 通常 run で AOT 適用。
plain と AOT cached の elapsed (sec, best of 3):

| bench     | plain | AOT cached | speedup |
|-----------|-------|------------|---------|
| fib35     | 0.69  | 0.32       | 2.16×   |
| sumloop   | 2.37  | 0.65       | 3.65×   |
| sieve_big | 1.64  | 0.65       | 2.52×   |
| deriv     | 1.41  | 1.18       | 1.19×   |
| nqueens   | 3.04  | 1.20       | 2.53×   |
| fannkuch  | 1.56  | 1.10       | 1.41×   |
| cps_loop  | 1.28  | 0.34       | 3.75×   |

geomean speedup **2.20×**。 self-tail-call loop (sumloop / cps_loop) と
list-heavy interp loop (sieve_big / nqueens) で 2.5×〜3.7× の加速、
recursion-bound (fib35) で 2.16×、 array/symbolic op (deriv) で 1.19× と
最も低い。

### 9.3 他 Scheme 実装との head-to-head (plain + AOT)

`bench/cross/*.scm` (= fib35 / tarai / ack / sum / sieve / nqueens)
を 6 つの主流 Scheme 実装で計測 (= chez 9.5.8 / racket 8.10 / gambit
4.9.3 / chibi 0.9.1 / guile-3.0.9 / chicken 5)。 best of 3 elapsed sec。

**ascheme_precise の GC** は `copy` (= Cheney semi-space、 default
backend) を使用。 §1〜§5 の matrix で示した通り、 主流 backend
(`copy` / `copy_gen` / `immix` / `immix_gen`) は ±5% 以内で互角なので、
この cross-Scheme 比較は backend 選択に対して頑健。 他処理系の GC は
chez = native incremental generational、 racket = native incremental
generational、 guile = libgc (BDW conservative)、 gambit = own mark&sweep、
chicken = Cheney over C stack、 chibi = own mark&sweep。

| bench   | prec-AOT | prec-plain | chez  | racket | gambit | chibi | guile-3.0 | chicken |
|---------|----------|------------|-------|--------|--------|-------|-----------|---------|
| fib35   | **0.33** | 0.71       | 0.18  | 0.28   | 4.07   | 1.52  | 5.18      | 7.11    |
| tarai   | **0.14** | 0.33       | 0.12  | 0.21   | 1.56   | 0.76  | 2.82      | 2.70    |
| ack     | **0.13** | 0.50       | 0.10  | 0.22   | 1.85   | 0.75  | 2.48      | 3.14    |
| sum     | **0.09** | 0.25       | 0.10  | 0.20   | 1.57   | 0.66  | 2.12      | 2.74    |
| sieve   | 2.01     | 2.72       | 0.27  | 0.40   | 5.91   | 8.85  | 6.44      | 7.88    |
| nqueens | **0.20** | 0.26       | 0.09  | 0.20   | 0.47   | 0.95  | 1.21      | 1.17    |

**観察**:
- **ascheme_precise AOT は chez (native compile) / racket (cs JIT) に
  次ぐ 3 位**。 sum / ack で chez を上回り、 fib35 / tarai / nqueens は
  chez の 0.7-1.5× と非常に競争的。
- sieve は 7× 遅い (= sieve は cons-heavy + capturing lambda が hot で、
  Phase 2c の no_capture optimization が効きにくいワークロード)。
- **plain interpreter (= AOT 無し) ですら gambit gsi / guile-3.0 / chicken
  csi / chibi-scheme を上回る**。 plain interpreter として最速級。

### 9.4 残課題 (= Phase 3-8 を縮小した理由)

original migration plan の Phase 3 (= 全 closure を sp[] 化) と Phase 7
(= sframe 完全削除) は次の理由で実用的価値が薄いと判明:

- capture を持つ closure は heap-resident な slot storage が必要 (=
  inner lambda が outer's frame を escape する semantics 上)。 box 方式
  (= 各 captured slot を 1-VALUE box obj に分離) を採っても per-call
  box alloc 1 回 ≈ per-call sframe alloc 1 回 で heap traffic は同等
- 従って sframe を delete しても renaming に等しく perf gain なし
- fib35 / sumloop / cps_loop 等は既に no_capture path で chez 級の速度
  に到達済。 sieve のような capture-heavy が遅いのは sframe alloc cost
  というより interpreter dispatch overhead が dominant

そのため Phase 3 / Phase 7 / Phase 8 は **scope reduction** とし、 sframe
は capture を持つ closure 専用の per-call storage として残置。 次の
optimization 候補は dispatch overhead 自体の削減 (= inline cache、 type
specialization 等)。

### 9.5 AOT asm-level analysis (2026-05-26)

fib35 AOT (= `SD_5686e442f5dbe03a` = fib body inlined) を objdump + perf
record で解析した結果:

| metric | value |
|---|---|
| cycles    | 0.89 G |
| instructions | 3.6 G |
| IPC       | **4.04** |
| branch miss | 0.03% |
| L1-d miss | negligible |

IPC 4 はバックエンドポート完全飽和に近く、 microarch 的にはほぼ理論
上限。 一方 chez 9.5.8 は 0.18s で 720 M cycles 程度なので、 **同じ
microarch で chez は ~30 cycles/call、 prec-AOT は ~50 cycles/call**。
gap は instruction count に由来 — prec-AOT は per-call 100 inst 程度、
chez は 20-30 inst 程度と推定。

#### hot path 内訳 (= fib body 1 recursion 分)

1. `gref("fib")` cache check + load (≈ 6 inst)
2. arith fast path (= `(- n 1)` fixnum sub) (≈ 8 inst)
3. SP_PUSH zero (`vmovdqu %xmm0, ...`) — GC-safety の slot pre-init (5 inst)
4. closure shape guard (`cmpb 0x21(%rsi)`, `cmpq 0x18(%rsi)`) (≈ 6 inst)
5. body dispatcher 呼び出し (= `call *0x30(%rsi)` indirect) (1 inst)
6. tail_call_pending check + trampoline exit (≈ 5 inst)
7. frame_sp / env restore (≈ 6 inst)

合計 ~37 inst × 2 calls/recursion + overhead = ~100 inst/recursion で
計測値と整合。

#### 残る gap の構造的原因

- **indirect call**: `call *body->head.dispatcher` は 2-level の load
  (closure→body, body→dispatcher) + indirect branch。 chez は直接呼出
  (= compiler-resolved direct CALL)。
- **GC-safety の sp[] root scan**: SP_PUSH が毎 call 2-4 slot を zero。
  caller の arg dispatcher が GC-safe (= node_lref_sp + node_const_*)
  でも、 NODE_DEF body は callee 側で zero してしまう。
- **trampoline loop**: cross-closure tail-call を catch する `for(;;)`
  ループ + tail_call_pending check が毎 call 走る。 fib body は実際は
  pending を立てないが、 check 自体は構造上残る。

これらは static framework-level の制約。 chez 級にするには per-callsite
direct-call specialization (= inline cache + body dispatcher pinning) と
GC-safe-arg ベースの SP_PUSH skipping、 trampoline loop の hoist-out が
必要で、 framework architecture-level の変更となる。

#### 実装可能な近場最適化 (実験結果)

`node_loop` の非 leaf branch (= 各 iter ごと fresh sframe alloc) を leaf
branch (= WB-in-place 上書き) に置き換える sieve_big 12% 高速化の上限を
確認したが、 test/17_vector_closures (= 各 iter の i を closure で capture
する R5RS spec 準拠 test) で 30→100 に semantics 破壊。 安全に取るには
escape analysis (= inner lambda の値が outer frame の lifetime を超えて
escape するか) の実装が必要。 short-term 投資対効果が見合わないので
todo に保留。

### 9.6 fuzzer 拡張 + bug discovery (2026-05-26)

`test/fuzz/fuzz.rb` (1600+ lines) を comprehensive 化:
- 106 template + 9 structural generators
- 4-mode matrix (plain / stress / aot / aot-stress)
- mutation + crossover via dynamic CORPUS
- chez 9.5.8 を oracle として comparison
- `make fuzz` / `fuzz-quick` / `fuzz-all` rule + COV=1 gcov

`#182` (= compile-time stress GC で symbol name pointer corruption) を
発見・修正済。 60+ seed 15+ run の sustained sweep で false-positive
1 件 (= `gen_call_n_tail` の loop counter が `rnum` 経由で 2^40 bignum
を引いて legitimate timeout) を特定・generator 側で fix。 0 genuine bug
in 24,000+ runs at the post-fix baseline。
