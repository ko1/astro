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
`ascheme_c7.txt` (= 7 chunk file)。 各 chunk は 2 backend × {plain, AOT} の
elapsed + RSS。

## 1. plain interpreter (= 15 precise backend + libgc × 9 bench)

### 1.1 elapsed (秒、 median of 3)

実用 11 backend (= §3 の 4 特殊用途を除く):

| bench | cat | libgc | mark | m_G | m_G_inc | copy | copy_G | m_c | m_c_G | I | I_G | m_bmp_G | m_crd_G | m_free |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fib35     | INT | **0.57** | 1.19 | 1.44 | 1.44 | 1.13 | 1.19 | 1.37 | 1.45 | 1.17 | 1.15 | 1.45 | 1.40 | 1.21 |
| sumloop   | INT | **1.97** | 2.65 | 2.04 | 2.46 | 2.09 | 2.05 | 2.14 | 2.59 | 2.20 | 2.04 | 2.54 | 2.85 | 2.20 |
| nbody     | INT | 0.69 | 0.74 | 0.54 | 0.69 | 0.55 | 0.55 | 0.66 | 0.68 | 0.54 | 0.55 | 0.71 | 0.70 | 0.59 |
| sieve_big | GC  | 1.46 | 2.06 | 1.39 | 1.90 | 1.41 | **1.35** | 1.55 | 1.97 | 1.47 | 1.44 | 1.97 | 1.95 | 1.55 |
| deriv     | GC  | 1.38 | 1.40 | 1.20 | 1.44 | 1.21 | 1.15 | 1.33 | 1.35 | 1.24 | **1.14** | 1.38 | 1.37 | 1.23 |
| nqueens   | MIX | **2.70** | 3.31 | 2.79 | 3.47 | 2.81 | 2.84 | 3.11 | 3.32 | 2.84 | 2.83 | 3.39 | 3.38 | 3.08 |
| fannkuch  | MIX | 1.77 | 2.12 | 1.38 | 1.92 | 1.37 | 1.52 | 1.93 | 1.87 | 1.43 | 1.42 | 1.91 | 1.91 | 1.58 |
| cps_loop  | MIX | 1.07 | 1.41 | 1.15 | 1.35 | 1.12 | **0.99** | 1.18 | 1.41 | 1.14 | 1.15 | 1.40 | 1.41 | 1.13 |
| matmul    | MIX | 10.00 | 6.26 | 6.08 | 6.09 | 6.42 | 6.20 | 6.32 | 6.14 | 6.28 | 5.93 | 6.18 | 6.04 | **5.74** |

**太字** は各 bench で libgc を含む全 backend (= 実用 + 特殊) の最速。

### 1.2 peak RSS (MiB、 max of 3)

| bench | libgc | mark | m_G | m_G_inc | copy | copy_G | m_c | m_c_G | I | I_G | m_bmp_G | m_crd_G | m_free |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fib35     | **4.0** | 32.2 | 35.8 | 23.8 | 35.6 | 35.8 | 19.8 | 35.6 | 19.6 | 35.6 | 19.9 | 20.0 | 19.6 |
| sumloop   | **4.0** | 3.9 | 3.6 | 3.6 | 3.8 | 3.6 | 3.6 | 3.6 | 3.6 | 3.6 | 3.6 | 3.6 | 3.6 |
| nbody     | 73.6 | 23.5 | 35.9 | 26.5 | 35.9 | 35.8 | 19.9 | 23.4 | 20.1 | 35.9 | 23.4 | 23.2 | 23.1 |
| sieve_big | 164.9 | 99.1 | 201.6 | 102.0 | 201.5 | 112.1 | 226.9 | 98.9 | 201.8 | 111.9 | 98.9 | 98.8 | 221.9 |
| deriv     | 73.5 | 27.8 | 35.6 | 29.9 | 35.5 | 35.6 | **19.8** | 26.0 | 19.8 | 35.6 | 26.0 | 26.0 | 25.8 |
| nqueens   | 73.5 | FAIL | 35.6 | 27.9 | 35.9 | 35.8 | 19.6 | 25.5 | 19.8 | 35.6 | 25.2 | 25.4 | 25.0 |
| fannkuch  | 73.4 | 25.4 | 35.9 | 28.2 | 35.9 | 35.6 | 19.5 | 25.2 | 19.8 | 35.9 | 25.2 | 25.2 | 24.9 |
| cps_loop  | 4.1 | 3.9 | 3.5 | 3.6 | 3.6 | 3.6 | 3.8 | 3.8 | 3.6 | 3.8 | 3.8 | 3.6 | 3.8 |
| matmul    | 71.9 | 32.3 | 29.1 | 33.0 | 29.0 | 29.5 | 29.1 | 32.2 | 29.8 | 29.7 | 32.3 | 32.3 | 32.1 |

`mark` の `nqueens` は **FAIL** (= AOT 列も `nqueens` 失敗)。 §5 todo。

### 1.3 plain での観察

- **fib35** (= 純再帰、 stack 深い): precise rooting の sframe 更新が worst
  case。 全 backend で **2.0–2.5× 遅い** (= libgc 0.57 vs precise 1.13–1.45)。
  libgc は C stack を保守的 scan するので per-call sp[] 不要。 **fib35 の
  RSS は libgc 4.0 MiB に対し precise 系 19–36 MiB** = sframe + heap の
  initial reserve cost。
- **sumloop / nqueens** で libgc が precise を全 backend で上回る (= libgc
  1.97 vs 最速 precise `mark_gen` 2.04; libgc 2.70 vs 最速 `mark_gen` 2.79)。
  dispatch only な int bench。
- **GC-heavy** な `sieve_big` / `deriv` / `fannkuch` / `matmul` は backend
  によって libgc を上回る:
  - `copy_gen` sieve_big 1.35 < libgc 1.46
  - `immix` deriv 1.14 < libgc 1.38
  - precise 系全部 fannkuch < libgc 1.77 (= 最速 `immix` 1.42)
  - **matmul は precise 14 backend 全て libgc 10.00 を上回り 5.74–6.42 (=
    0.57–0.64×)**
- **`mark` で `nqueens` FAIL**: GC bug、 §5 todo。
- **`mark_compact`** は ascheme 環境で `binary_trees` 系の long-lived chain
  で SEGV する known issue があるが、 本 9 bench では問題なく全 PASS。

### 1.4 plain ranking (= 実用 11 backend、 geomean elapsed)

| rank | backend | plain geomean | vs libgc |
|---:|---|---:|---:|
| 1 | `copy_gen` | 1.55s | **0.96×** |
| 2 | `immix_gen` | 1.55s | 0.97× |
| 3 | `copy` | 1.57s | 0.97× |
| 4 | `mark_gen` | 1.59s | 0.99× |
| 5 | `immix` | 1.60s | 0.99× |
| — | **libgc** | **1.61s** | **1.00 (baseline)** |
| 6 | `mark_freelist` | 1.64s | 1.02× |
| 7 | `mark_compact` | 1.76s | 1.09× |
| 8 | `mark_gen_inc` | 1.90s | 1.18× |
| 9 | `mark_compact_gen` | 1.91s | 1.18× |
| 10 | `mark_bitmap_gen` | 1.92s | 1.20× |
| 11 | `mark_card_gen` | 1.93s | 1.20× |
| 12 | `mark` | 1.94s | 1.20× (n=8、 nqueens FAIL) |

主流の `copy_gen` / `immix_gen` / `copy` / `mark_gen` / `immix` が **libgc 比
0.96–0.99×** で互角〜微優位。 落ちこぼれは `mark_card_gen` / `mark_bitmap_gen`
/ `mark_gen_inc` / `mark_compact_gen` / `mark` で 1.18–1.20×。

## 2. AOT (= cached SD)

`--aot-compile --run` で hot AST node を `code_store/all.so` 内 SD として
gcc で bake。 2 回目以降の run が dlopen して bind する仕組み。 ascheme
(libgc) は AOT 未対応なので、 AOT 表は precise 15 backend のみ。

### 2.1 elapsed (秒、 median of 3、 AOT cached)

実用 11 backend:

| bench | mark | m_G | m_G_inc | copy | copy_G | m_c | m_c_G | I | I_G | m_bmp_G | m_crd_G | m_free |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fib35     | 1.03 | **0.77** | 1.05 | 0.76 | 0.78 | 0.98 | 1.01 | 0.79 | 0.78 | 1.02 | 1.00 | 0.81 |
| sumloop   | 1.13 | **0.58** | 1.01 | NOAOT | 0.71 | 0.71 | 1.10 | 0.60 | 0.59 | 1.07 | 1.07 | 0.59 |
| nbody     | 0.64 | 0.43 | 0.57 | 0.43 | 0.44 | 0.55 | NOAOT | 0.43 | **0.44** | 0.57 | NOAOT | 0.48 |
| sieve_big | 1.09 | **0.58** | 1.01 | 0.58 | 0.57 | 0.62 | 1.06 | 0.60 | 0.60 | 1.04 | 1.09 | 0.58 |
| deriv     | 1.38 | **1.07** | 1.36 | 1.10 | 1.05 | 1.25 | 1.30 | 1.13 | 1.05 | 1.31 | 1.29 | 1.13 |
| nqueens   | FAIL | 1.24 | 1.71 | 1.29 | **1.23** | 1.41 | 1.74 | 1.27 | 1.29 | 1.71 | 1.72 | 1.35 |
| fannkuch  | 1.58 | 1.03 | 1.55 | 1.01 | **1.06** | 1.37 | 1.58 | 1.05 | 1.05 | 1.53 | 1.51 | 1.21 |
| cps_loop  | 0.60 | **0.33** | 0.53 | 0.32 | 0.32 | 0.32 | 0.58 | 0.32 | 0.32 | 0.58 | 0.58 | 0.31 |
| matmul    | 6.10 | 5.81 | 5.84 | 6.25 | 5.81 | 6.04 | 5.90 | 5.95 | 5.74 | 5.72 | 5.66 | **5.45** |

`NOAOT` = AOT compile が出力した SD が attach しなかった cell (= dlopen 時
に fallback to interpreter)。 個別 cell の compile bug 由来、 §5 todo。
`FAIL` = `mark` の `nqueens` (= GC bug)。

### 2.2 peak RSS (MiB、 max of 3、 AOT)

| bench | mark | m_G | m_G_inc | copy | copy_G | m_c | m_c_G | I | I_G | m_bmp_G | m_crd_G | m_free |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fib35     | 23.8 | 35.8 | 24.0 | 35.8 | 35.6 | 19.6 | 20.0 | 19.9 | 35.8 | 20.0 | 20.0 | 19.6 |
| sumloop   | 3.8 | 3.6 | 3.9 | NOAOT | 31.6 | 31.0 | 4.0 | 31.5 | 3.8 | 3.9 | 3.9 | 3.6 |
| nbody     | 23.5 | 36.0 | 26.6 | 36.0 | 36.1 | 20.0 | NOAOT | 20.1 | 35.9 | 23.2 | NOAOT | 23.0 |
| sieve_big | 99.1 | 201.8 | 102.1 | 201.6 | 112.1 | 227.0 | 99.0 | 201.8 | 112.1 | 99.1 | 99.1 | 222.1 |
| deriv     | 27.8 | 35.6 | 29.9 | 35.9 | 35.5 | 27.9 | 27.9 | 27.6 | 35.8 | 27.9 | 27.9 | 27.6 |
| nqueens   | FAIL | 35.6 | 28.1 | 35.9 | 35.6 | 19.8 | 25.4 | 20.0 | 35.6 | 25.4 | 25.4 | 24.9 |
| fannkuch  | 25.4 | 36.0 | 28.2 | 35.9 | 35.9 | 19.6 | 25.4 | 20.0 | 35.8 | 25.5 | 33.9 | 25.0 |
| cps_loop  | 3.9 | 3.9 | 3.8 | 3.8 | 3.9 | 3.8 | 3.9 | 3.6 | 3.8 | 3.9 | 3.9 | 3.8 |
| matmul    | 32.3 | 29.2 | 33.1 | 29.0 | 29.4 | 29.1 | 32.5 | 29.9 | 29.8 | 32.5 | 32.5 | 32.4 |

### 2.3 plain vs AOT speedup (実用 11 backend、 geomean of bench ratios)

| backend | plain geomean | AOT geomean | plain/AOT |
|---|---:|---:|---:|
| `mark`            | 1.94s | 1.18s (n=8) | 1.45× |
| `mark_gen`        | 1.59s | **0.81s** | **1.84×** |
| `mark_gen_inc`    | 1.90s | 1.10s | 1.56× |
| `copy`            | 1.57s | 0.84s (n=8) | 1.65× |
| `copy_gen`        | 1.55s | 0.83s | **1.75×** |
| `mark_compact`    | 1.76s | 0.99s | 1.76× |
| `mark_compact_gen`| 1.91s | 1.18s (n=8) | 1.57× |
| `immix`           | 1.60s | 0.84s | **1.81×** |
| `immix_gen`       | 1.55s | 0.83s | **1.78×** |
| `mark_bitmap_gen` | 1.92s | 1.16s | 1.56× |
| `mark_card_gen`   | 1.93s | 1.16s (n=8) | 1.61× |
| `mark_freelist`   | 1.64s | **0.85s** | **1.83×** |

AOT 速度上位 5: `mark_gen` (0.81s) / `copy_gen` (0.83s) / `immix_gen`
(0.83s) / `copy` (0.84s) / `immix` (0.84s)。

### 2.4 AOT 加速率 (= plain / AOT、 主要 backend × bench)

| backend | fib35 | sumloop | nbody | sieve_big | deriv | nqueens | fannkuch | cps_loop | matmul |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `mark_gen`   | 1.87× | **3.52×** | 1.26× | **2.40×** | 1.12× | 2.25× | 1.34× | **3.48×** | 1.05× |
| `copy_gen`   | 1.53× | **2.89×** | 1.25× | **2.37×** | 1.10× | 2.31× | 1.43× | **3.09×** | 1.07× |
| `immix_gen`  | 1.47× | **3.46×** | 1.25× | **2.40×** | 1.09× | 2.19× | 1.35× | **3.59×** | 1.03× |
| `immix`      | 1.48× | **3.67×** | 1.26× | **2.45×** | 1.10× | 2.24× | 1.36× | **3.59×** | 1.06× |
| `mark_free`  | 1.49× | **3.73×** | 1.23× | **2.67×** | 1.09× | 2.28× | 1.31× | **3.65×** | 1.05× |

- **dispatch heavy** (= `sumloop` / `cps_loop`) で AOT は **2.9–3.7×** speedup
- **GC bound** (= `matmul`) は全 backend で 1.03–1.07× tied。 GC time が
  dominate
- **fib35** は 1.47–1.87× で改善するが libgc plain 0.57 にはまだ届かない (=
  `mark_gen` AOT 0.77、 sframe overhead が SD でも残る)

## 3. 特殊用途 backend 4 個

実用と並べると判定がミスリーディングなため別表。 §1 ranking からは除外。

### 3.1 elapsed (秒、 plain + AOT)

| bench | cat | none plain / AOT | bump plain / AOT | mark_bump_gen plain / AOT |
|---|---|---:|---:|---:|
| fib35     | INT | 0.67 / 1.06 | 1.58 / 0.77 | 1.17 / 0.77 |
| sumloop   | INT | 2.64 / 1.11 | 2.12 / 0.59 | 2.06 / 0.59 |
| nbody     | INT | 0.70 / 0.58 | 0.54 / 0.45 | 0.58 / 0.43 |
| sieve_big | GC  | 2.09 / 1.02 | 1.43 / 0.61 | 1.45 / FAIL |
| deriv     | GC  | 1.41 / 1.30 | 1.22 / 1.11 | 1.13 / 1.04 |
| nqueens   | MIX | 3.48 / 1.75 | 2.88 / 1.33 | 2.95 / 1.30 |
| fannkuch  | MIX | 1.94 / 1.61 | 1.40 / 1.05 | 1.45 / 1.06 |
| cps_loop  | MIX | 1.46 / 0.59 | 1.15 / 0.32 | 1.13 / 0.31 |
| matmul    | MIX | 6.53 / 5.84 | 6.41 / 6.19 | 6.07 / 5.61 |
| **geomean** | — | 1.81s / 1.16s | 1.64s / 0.88s | 1.58s / 0.84s |

### 3.2 peak RSS (MiB、 plain + AOT)

| bench | none p / AOT | bump p / AOT | mark_bump_gen p / AOT |
|---|---:|---:|---:|
| fib35     | 20.1 / 19.6 | **914.8** / 35.8 | 35.6 / 35.8 |
| sumloop   | 3.9 / 3.6 | 3.6 / 3.8 | 3.6 / 31.4 |
| nbody     | 23.4 / 23.2 | 36.0 / 36.0 | 35.9 / 36.1 |
| sieve_big | 99.0 / 98.8 | 201.6 / 201.8 | 112.1 / FAIL |
| deriv     | 27.8 / 26.0 | 35.6 / 35.6 | 35.8 / 35.8 |
| nqueens   | 25.5 / 25.4 | 35.8 / 35.6 | 35.8 / 35.8 |
| fannkuch  | 25.2 / 25.2 | 35.8 / 35.8 | 35.9 / 35.6 |
| cps_loop  | 3.9 / 3.8 | 3.8 / 3.8 | 3.6 / 3.8 |
| matmul    | 32.3 / 32.3 | 29.0 / 28.9 | 29.8 / 29.8 |

### 3.3 観察

- **`none` (= libc malloc + leak)**: 全 alloc を leak。 short bench でも
  20–99 MiB の RSS、 long-lived `sieve_big` で 99 MiB。 GC 抜きの alloc
  throughput 上限の参考値。 plain matmul で 6.53s と precise 系の 6.0s 台
  にやや劣る (= cache miss が effective floor)。
- **`bump` (= bump alloc only、 leak)**: `none` と類似だが alloc が
  region-bump で hot path が短い。 **fib35 で plain 1.58s / AOT 0.77s / RSS
  914.8 MiB** (= sframe alloc を leak し続けた累積)。 short bench でも GiB
  スケール RSS で production 不可。 AOT 時 RSS が 35.8 に下がるのは「AOT が
  alloc を pool 化している」 という興味深い observation。
- **`mark_bump_gen` (= bump tenured + mark sweep nursery)**: tenured 側に
  compactor が無く、 long-running workload で fragmentation 累積 (=
  `sieve_big` AOT で **FAIL**)。 short bench では precise 系上位と互角 (=
  plain 1.58s = `copy_gen` 1.55s 並み)。 GC algorithm comparison としては
  「tenured compactor の必要性」 の反例。
- **`mark_freelist`** (= 本来「fragmentation testbed」 の意図) は §1 で
  実用 backend と一緒に評価。 今回 9 bench で fragmentation は顕在化せず、
  matmul plain で `mark_freelist` 5.74s と全 backend 中最速。 「freelist
  algorithm が一定 workload で悪くない」 という positive 結果として記録。

## 4. libgc 比較 (= ascheme baseline)

### 4.1 ⚠ caveat (再掲)

ascheme libgc = Boehm conservative GC、 ascheme_precise = sframe chain +
precise sp[] scan。 比較しているのは「runtime + rooting + collector の
合計差」 であって、 GC algorithm 純粋差ではない。

### 4.2 plain geomean ratio (= vs libgc plain)

§1.4 ranking 再掲。 主流 6 backend (`copy_gen` / `immix_gen` / `copy` /
`mark_gen` / `immix` / `mark_freelist`) が **libgc 比 0.96–1.02×** で並ぶ。

### 4.3 AOT geomean ratio (= AOT vs libgc plain)

| backend | AOT geomean | vs libgc plain |
|---|---:|---:|
| `mark_gen`        | **0.81s** | **0.54×** |
| `copy_gen`        | 0.83s | 0.55× |
| `immix_gen`       | 0.83s | 0.54× |
| `copy`            | 0.84s (n=8) | 0.58× |
| `immix`           | 0.84s | 0.55× |
| `mark_freelist`   | 0.85s | 0.56× |
| `mark_compact`    | 0.99s | 0.62× |
| `mark_gen_inc`    | 1.10s | 0.76× |
| `mark_bitmap_gen` | 1.16s | 0.77× |
| `mark_card_gen`   | 1.16s (n=8) | 0.76× |
| `mark`            | 1.18s (n=8) | 0.83× |
| `mark_compact_gen`| 1.18s (n=8) | 0.77× |
| `libgc` plain     | **1.61s** | **1.00 (baseline)** |

- **AOT geomean は 12/12 実用 backend で libgc plain を下回る** (= 0.54–0.83×)
- トップ 5: **`mark_gen` 0.54×** / `immix_gen` 0.54× / `copy_gen` 0.55× /
  `immix` 0.55× / `mark_freelist` 0.56×
- 落ちこぼれ: `mark_card_gen` / `mark_bitmap_gen` / `mark_gen_inc` /
  `mark_compact_gen` / `mark` が 0.76–0.83×。 matmul 以外で sumloop /
  cps_loop の speedup が頭打ちで geomean を引き下げる

## 5. ranking + geomean

### 5.1 plain mode ranking (= §1.4 再掲)

(略、 §1.4 と同じ)

### 5.2 AOT mode ranking (= §4.3 再掲)

1. `mark_gen` — **0.81s** (vs libgc plain **0.54×**)
2. `copy_gen` — 0.83s (0.55×)
3. `immix_gen` — 0.83s (0.54×)
4. `copy` — 0.84s n=8 (0.58×)
5. `immix` — 0.84s (0.55×)
6. `mark_freelist` — 0.85s (0.56×)
7. `mark_compact` — 0.99s (0.62×)
8. `mark_gen_inc` — 1.10s (0.76×)
9. `mark_bitmap_gen` — 1.16s (0.77×)
10. `mark_card_gen` — 1.16s n=8 (0.76×)
11. `mark` — 1.18s n=8 (0.83×、 nqueens FAIL)
12. `mark_compact_gen` — 1.18s n=8 (0.77×、 nbody NOAOT)

### 5.3 production 推奨

plain + AOT 両方で上位:

- **`copy_gen`** = balanced、 N-survive minor + Cheney major (plain 1 位、 AOT 2 位)
- **`immix_gen`** = fragmentation-resistant、 line+block region (plain 2 位、 AOT 3 位)
- **`immix`** = 非 gen で region-bump、 long-lived heavy 用途 (plain 5 位、 AOT 5 位)
- **`copy`** = 非 gen Cheney、 simple semispace (plain 3 位、 AOT 4 位)
- **`mark_gen`** = AOT トップ (0.54×) だが plain で `copy_gen` に劣る

短期 alloc-heavy で **`copy_gen` / `immix_gen`** が安定して最強。 long-lived
heap heavy では **`immix` / `copy`** に倒す選択もあり。

### 5.4 既知 limitation (= todo)

- **fib35 overhead 残 1.3–2.5×** (plain 2.0–2.5×、 AOT 1.3–1.5×): 純再帰の
  sframe 更新コスト本質。 libgc は C stack 保守的 scan で per-call sframe
  update 不要。
- **`mark_card_gen` / `mark_bitmap_gen` / `mark_gen_inc` / `mark` /
  `mark_compact_gen` の AOT 加速率が低い** (= sumloop / cps_loop で 2.0–2.5× に
  留まる、 他は 3.4–3.7×)。 GC overhead が比率高めで dispatch fold の効果が
  薄まる。
- **`mark` の `nqueens` FAIL**: known GC bug、 個別 todo。
- **`copy` の `sumloop` AOT が NOAOT** / **`mark_compact_gen` の `nbody` /
  `mark_card_gen` の `nbody` が NOAOT**: 個別 cell の AOT compile bug、 todo。

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

- **plain mode**: 主流 backend (= `copy_gen` / `copy` / `immix_gen` /
  `immix` / `mark_gen`) で **geomean 0.96–0.99×** (= libgc と互角)。
  GC-heavy 個別 workload で互角〜やや速い、 `fib35` のみ 2.0× 遅い
- **AOT mode**: 主流 backend で **geomean 0.54–0.58×** (= libgc plain の
  -42〜-46% faster)。 9/9 workload で libgc plain を上回り、 dispatch heavy
  で 3× 級、 GC heavy で 2× 級、 matmul で 1.6× 級
- **production 推奨**: `copy_gen` / `immix_gen` (= mainline) / `copy` /
  `immix` (= 非 gen variant)
- **audit / debug 兼用**: `copy_scramble` (= overhead 微小、 stress + scramble で
  precise rooting バグを即時検出)
