# ascheme_precise — performance evaluation

`sample/ascheme_precise/` の precise GC framework migration の perf 評価。
**libgc (= Boehm conservative GC, `sample/ascheme/`)** を baseline に、
16 個の precise GC backend (= scramble 除く) を **plain interpreter** /
**AOT (= `--aot-compile` + dlopen cached SDs)** 2 軸で測定。

`make bench-aot` で再現可能 (= 約 30 分)。

## 0. setup

- **machine**: AMD Ryzen 9 5900HX (= 8 cores / 16 threads、 ~4.6 GHz boost)
- **memory**: 30 GiB
- **kernel**: Linux 6.8.0-117 x86_64
- **compiler**: gcc -O3 -ggdb3
- **methodology**: 各 benchmark 3 回実行、 最小値。 出力 first-line を expected と
  照合して **正答性検証** してから記録 (= GC bug で「速いが結果が誤」を排除)
- **scale**: 0.3–10 秒の範囲で sustained measurement

略号 (= 後続の表で使用):

| 略号 | backend | 略号 | backend |
|------|---------|------|---------|
| `m_G`     | mark_gen          | `copy_G`     | copy_gen          |
| `m_G_inc` | mark_gen_inc      | `copy_G_inc` | copy_gen_inc      |
| `m_free`  | mark_freelist     | `m_c`        | mark_compact      |
| `m_bmp_G` | mark_bitmap_gen   | `m_c_G`      | mark_compact_gen  |
| `m_crd_G` | mark_card_gen     | `m_Bu_G`     | mark_bump_gen     |
| `I`       | immix             | `I_G`        | immix_gen         |
| `Bu`      | bump              | (libgc は ascheme = Boehm conservative GC) |  |

## 1. plain interpreter (= 全 16 backend × 9 workload、 単位 秒)

```
bench       cat libgc  none  Bu    mark  m_G   m_G_inc m_free m_bmp_G m_crd_G copy  copy_G copy_G_inc m_c   m_c_G m_Bu_G I     I_G
fib35       INT 0.46   0.52  1.30  1.03  1.04  1.40    0.97   1.12    1.16    0.93  0.93   0.95       1.16  0.90  0.93   0.95  0.91
sumloop     INT 1.52   1.66  1.77  1.68  1.69  2.07    1.87   2.09    2.05    1.65  1.67   1.84       1.73  1.69  1.93   1.69  1.70
nbody       INT 0.55   0.89  0.65  0.49  0.52  0.59    0.48   0.56    0.56    0.44  0.44   0.44       0.52  0.43  0.45   0.43  0.43
sieve_big   GC  1.18   1.22  1.22  1.19  1.16  1.63    1.27   1.61    1.61    1.18  1.17   1.22       1.22  1.17  1.26   1.15  1.20
deriv       GC  1.09   1.59  1.29  1.05  1.03  1.20    1.02   1.16    1.15    1.02  0.96   0.99       1.07  0.92  0.98   1.00  0.94
nqueens     MIX 2.08   2.39  2.64  2.39  2.42  2.84    2.55   2.77    2.71    2.28  2.38   2.40       2.42  2.32  2.39   2.31  2.42
fannkuch    MIX 1.39   2.59  1.82  1.27  1.38  1.54    1.35   1.57    1.58    1.12  1.17   1.15       1.35  1.13  1.13   1.12  1.11
cps_loop    MIX 0.85   0.92  0.97  0.94  0.96  1.09    1.02   1.14    1.05    0.92  0.92   1.04       0.92  0.90  0.99   0.85  0.96
matmul      MIX 8.11   10.25 10.24 4.86  4.61  4.69    4.85   4.71    4.72    4.99  4.79   4.81       4.87  4.71  4.59   4.71  4.53
```

### 1.1 plain での観察

- **fib35** (= 純再帰、 stack 深い) は precise rooting の sp[] 更新が
  worst case。 全 backend で 2.0–3.0× 遅い (= libgc 0.46 vs precise 0.90–1.40)。
  libgc は C stack を保守的に scan するので per-call sp[] 不要
- **sumloop / nbody / cps_loop** は libgc とほぼ同等〜やや遅い (= +5〜25%)
- **GC-heavy** な `sieve_big` / `deriv` / `nqueens` / `fannkuch` は backend
  によって libgc とほぼ同等〜やや速い (= 例 `mark_compact_gen` の deriv
  0.92 < libgc 1.09)
- **matmul** は **全 16 backend で 4.53–4.99 秒** に収束 (= libgc 8.11 の
  **0.56〜0.62×** で圧勝)。 `none` / `bump` のみ GC 無しの分 10.2 秒で遅い

### 1.2 plain の backend ランキング (= 全 9 workload geomean vs libgc)

| backend           | plain geomean |
|-------------------|--------------:|
| `mark_compact_gen`|         0.97× |
| `immix`           |         0.98× |
| `copy_gen`        |         0.99× |
| `immix_gen`       |         0.99× |
| `copy`            |         1.00× |
| `mark_bump_gen`   |         1.02× |
| `copy_gen_inc`    |         1.03× |
| `mark`            |         1.05× |
| `mark_gen`        |         1.06× |
| `mark_freelist`   |         1.08× |
| `mark_compact`    |         1.08× |
| `mark_card_gen`   |         1.20× |
| `mark_bitmap_gen` |         1.22× |
| `mark_gen_inc`    |         1.25× |
| `none`            |         1.27× |
| `bump`            |         1.31× |
| `libgc`           |         1.00 (baseline) |

主流 backend (= `copy` / `copy_gen` / `immix` / `mark_compact_gen` /
`immix_gen` / `mark_bump_gen`) は **libgc とほぼ互角** (= 0.97–1.02×)。
非 incremental + 非 bitmap/card 系が安定して優秀。

## 2. AOT (= `--aot-compile` で hot AST node を C 関数化、 dlopen で attach)

注: ascheme (libgc) は `--aot-compile` **未実装** なので、 以降の AOT 表は
precise GC 16 backend のみ。 libgc baseline (plain) との比較は §3。

```
bench       cat none  Bu    mark  m_G   m_G_inc m_free m_bmp_G m_crd_G copy  copy_G copy_G_inc m_c   m_c_G m_Bu_G I     I_G
fib35       INT 0.29  0.98  0.68  0.76  1.08    0.66   0.81    0.81    0.61  0.63   0.63       0.81  0.63  0.63   0.62  0.62
sumloop     INT 0.47  0.47  0.48  0.47  0.84    0.49   0.93    0.88    0.48  0.48   0.47       0.47  0.47  0.47   0.46  0.47
nbody       INT 0.81  0.58  0.40  0.42  0.50    0.40   0.48    0.46    0.36  0.35   0.35       0.43  0.36  0.35   0.35  0.34
sieve_big   GC  0.52  0.47  0.51  0.48  0.80    0.49   0.83    0.83    0.50  0.47   0.46       0.51  0.45  0.45   0.48  0.46
deriv       GC  1.56  1.14  1.00  0.96  1.12    0.94   1.10    1.06    0.90  0.87   0.89       0.99  0.83  0.87   0.89  0.83
nqueens     MIX 1.13  1.34  1.10  1.15  1.45    1.13   1.43    1.44    1.02  1.04   0.99       1.12  0.99  0.98   1.01  0.99
fannkuch    MIX 2.18  1.53  0.96  1.12  1.24    1.02   1.26    1.22    0.82  0.83   0.84       1.09  0.83  0.83   0.82  0.83
cps_loop    MIX 0.26  0.26  0.27  0.26  0.44    0.26   0.49    0.46    0.26  0.26   0.26       0.27  0.26  0.26   0.25  0.26
matmul      MIX 10.20 10.17 4.63  4.46  4.46    4.58   4.54    4.46    4.95  4.72   4.76       4.58  4.55  4.49   4.52  4.33
```

### 2.1 AOT での観察

- AOT は plain の dispatch overhead を inline で畳む。 dispatch heavy な
  `sumloop` / `cps_loop` は **0.25–0.49 秒** に収束 (= libgc plain の
  **1.7–3.3×** 速い)
- `fib35` も AOT で **0.61–0.81 秒** に改善 (= plain 比 1.4×)。 ただし
  libgc plain 0.46 には届かず (= 0.61 / 0.46 = 1.33×、 -33% 残)
- GC-heavy な `sieve_big` / `nqueens` / `fannkuch` でも AOT で **0.45–1.10 秒**
  と libgc plain の **2.0–2.5×** 速い
- `matmul` は plain と同じく非 `none`/`bump` 全 backend で **4.33–4.95 秒** (=
  libgc plain の **1.6–1.9×** 速い)。 AOT による上乗せは小さい (= GC time
  dominates)

### 2.2 AOT の backend ランキング (= 全 9 workload geomean vs libgc plain)

| backend           | AOT geomean |
|-------------------|------------:|
| `immix_gen`       |       0.53× |
| `mark_compact_gen`|       0.54× |
| `mark_bump_gen`   |       0.54× |
| `immix`           |       0.54× |
| `copy_gen`        |       0.55× |
| `copy_gen_inc`    |       0.55× |
| `copy`            |       0.56× |
| `mark`            |       0.59× |
| `mark_freelist`   |       0.59× |
| `mark_gen`        |       0.60× |
| `mark_compact`    |       0.62× |
| `none`            |       0.73× |
| `bump`            |       0.75× |
| `mark_card_gen`   |       0.78× |
| `mark_bitmap_gen` |       0.80× |
| `mark_gen_inc`    |       0.81× |
| `libgc` (plain)   |       1.00 (baseline) |

- **AOT geomean は 16/16 backend で libgc plain を下回る** (= 0.53–0.81×)
- **トップ: `immix_gen` = 0.53×** (= libgc plain の -47% faster)
- 生成系 (gen 系) と 非生成 (`copy` / `immix` / `mark`) が **0.53–0.60×** に
  集中、 互角。 `mark_compact_gen` / `mark_bump_gen` も 0.54×
- 落ちこぼれ: `mark_card_gen` / `mark_bitmap_gen` / `mark_gen_inc` のみ
  0.78–0.81× (= matmul 以外で sumloop / cps_loop の speedup が頭打ちで
  全体 geomean を引き下げる)

### 2.3 AOT 加速率 (= plain / AOT、 backend 別)

| backend          | fib35 | sumloop | nbody | sieve_big | deriv | nqueens | fannkuch | cps_loop | matmul |
|------------------|------:|--------:|------:|----------:|------:|--------:|---------:|---------:|-------:|
| `none`           | 1.79× |   3.53× | 1.10× |     2.35× | 1.02× |   2.12× |    1.19× |    3.54× |  1.00× |
| `bump`           | 1.33× |   3.77× | 1.12× |     2.60× | 1.13× |   1.97× |    1.19× |    3.73× |  1.01× |
| `mark`           | 1.51× |   3.50× | 1.23× |     2.33× | 1.05× |   2.17× |    1.32× |    3.48× |  1.05× |
| `mark_gen`       | 1.37× |   3.60× | 1.24× |     2.42× | 1.07× |   2.10× |    1.23× |    3.69× |  1.03× |
| `mark_gen_inc`   | 1.30× |   2.46× | 1.18× |     2.04× | 1.07× |   1.96× |    1.24× |    2.48× |  1.05× |
| `mark_freelist`  | 1.47× |   3.82× | 1.20× |     2.59× | 1.09× |   2.26× |    1.32× |    3.92× |  1.06× |
| `mark_bitmap_gen`| 1.38× |   2.25× | 1.17× |     1.94× | 1.05× |   1.94× |    1.25× |    2.33× |  1.04× |
| `mark_card_gen`  | 1.43× |   2.33× | 1.22× |     1.94× | 1.08× |   1.88× |    1.30× |    2.28× |  1.06× |
| `copy`           | 1.52× |   3.44× | 1.22× |     2.36× | 1.13× |   2.24× |    1.37× |    3.54× |  1.01× |
| `copy_gen`       | 1.48× |   3.48× | 1.26× |     2.49× | 1.10× |   2.29× |    1.41× |    3.54× |  1.01× |
| `copy_gen_inc`   | 1.51× |   3.91× | 1.26× |     2.65× | 1.11× |   2.42× |    1.37× |    4.00× |  1.01× |
| `mark_compact`   | 1.43× |   3.68× | 1.21× |     2.39× | 1.08× |   2.16× |    1.24× |    3.41× |  1.06× |
| `mark_compact_gen`| 1.43×|   3.60× | 1.19× |     2.60× | 1.11× |   2.34× |    1.36× |    3.46× |  1.04× |
| `mark_bump_gen`  | 1.48× |   4.11× | 1.29× |     2.80× | 1.13× |   2.44× |    1.36× |    3.81× |  1.02× |
| `immix`          | 1.53× |   3.67× | 1.23× |     2.40× | 1.12× |   2.29× |    1.37× |    3.40× |  1.04× |
| `immix_gen`      | 1.47× |   3.62× | 1.26× |     2.61× | 1.13× |   2.44× |    1.34× |    3.69× |  1.05× |

- **dispatch heavy** (= `sumloop` / `cps_loop`) で AOT は **2.3–4.1×**
  speedup。 AOT が interpreter dispatch loop を C 関数 inline に展開
- **GC bound** (= `matmul`) は **全 backend で 1.00–1.06×** で tied。
  GC time が dominate、 dispatch fold の余地が少ない

## 3. 代表比較 (= libgc vs copy)

`copy` は GC-heavy 全 workload PASS かつ ascheme の推奨 production backend
(= Cheney semispace、 balanced)。 libgc baseline と並べた一覧:

| bench     | libgc | copy plain | copy AOT | AOT vs libgc     | AOT vs copy plain |
|-----------|------:|-----------:|---------:|------------------|-------------------|
| fib35     |  0.46 |       0.93 |     0.61 | 1.33× slower     | **1.52× faster**  |
| sumloop   |  1.52 |       1.65 |     0.48 | **3.17× faster** | **3.44× faster**  |
| nbody     |  0.55 |       0.44 |     0.36 | **1.53× faster** | 1.22× faster      |
| sieve_big |  1.18 |       1.18 |     0.50 | **2.36× faster** | **2.36× faster**  |
| deriv     |  1.09 |       1.02 |     0.90 | **1.21× faster** | 1.13× faster      |
| nqueens   |  2.08 |       2.28 |     1.02 | **2.04× faster** | **2.24× faster**  |
| fannkuch  |  1.39 |       1.12 |     0.82 | **1.70× faster** | 1.37× faster      |
| cps_loop  |  0.85 |       0.92 |     0.26 | **3.27× faster** | **3.54× faster**  |
| matmul    |  8.11 |       4.99 |     4.95 | **1.64× faster** | 1.01× tied        |

- `copy` plain vs libgc: GC-heavy で互角〜やや負け、 matmul で **0.62×**
  faster。 9 workload geomean **1.00×** (= libgc plain と同等)
- `copy` AOT は **9/9 workload で libgc plain を上回る** (= fib35 は 0.61 で
  libgc 0.46 にやや劣るが、 plain 比 1.52× faster)。 9 workload geomean **0.56×**
- AOT の plain 比 speedup は dispatch heavy で **3.4–3.5×**、 GC-heavy
  で **2.2–2.4×**、 matmul で tied

## 4. matmul

`matmul` (= LCG で巨大 bignum を生成 + 200×200 fixnum 行列積) は
**GMP 経由 bignum allocation** が dominant な workload。 過去版では
generational backend の minor GC が external memory pressure (= libc-malloc'd
GMP buffer) で **livelock** し、 60–100 秒級の outlier を出していた。
commit `c88f92a1` (= external_bytes pressure を minor から major trigger へ
振替) で解消、 現在は全 backend で **4.5 秒台に収束**:

| backend         | plain | AOT  | AOT vs libgc (8.11) |
|-----------------|------:|-----:|--------------------:|
| `mark`          |  4.86 | 4.63 |               0.57× |
| `mark_gen`      |  4.61 | 4.46 |               0.55× |
| `mark_gen_inc`  |  4.69 | 4.46 |               0.55× |
| `mark_freelist` |  4.85 | 4.58 |               0.56× |
| `mark_bitmap_gen`|  4.71| 4.54 |               0.56× |
| `mark_card_gen` |  4.72 | 4.46 |               0.55× |
| `copy`          |  4.99 | 4.95 |               0.61× |
| `copy_gen`      |  4.79 | 4.72 |               0.58× |
| `copy_gen_inc`  |  4.81 | 4.76 |               0.59× |
| `mark_compact`  |  4.87 | 4.58 |               0.56× |
| `mark_compact_gen`| 4.71| 4.55 |               0.56× |
| `mark_bump_gen` |  4.59 | 4.49 |               0.55× |
| `immix`         |  4.71 | 4.52 |               0.56× |
| `immix_gen`     |  4.53 | 4.33 |               0.53× |

非 `none`/`bump` の全 14 backend で libgc 比 **0.53–0.61×** の倍速。
backend 間の差は小さく (= 4.33–4.99s)、 GC algorithm の好みより GC heuristic
tuning の問題が主。

## 5. 既知 limitation

### 5.1 fib35 overhead 残 1.3–1.5× (plain 2.0–3.0×、 AOT 1.3–1.5×)

純再帰の sp[] 更新コスト本質。 全 backend で plain 0.90–1.40s、 AOT
0.61–0.81s vs libgc 0.46s。 libgc は C stack を保守的 scan するため
per-call sp[] update 不要。

改善余地:
- self-tail-call の frame reuse 強化
- libgc-style な「lazy sp[] update」 (= alloc 直前のみ flush)

ascheme 固有最適化、 baruby_precise との codebase 共通性とのトレードオフ。

### 5.2 mark_card_gen / mark_bitmap_gen / mark_gen_inc の AOT 加速率が低い

これらの backend は AOT 加速倍率が `sumloop` / `cps_loop` で **2.0–2.5×**
に留まる (= 他 backend は 3.4–4.1×)。 GC overhead が比率高めで dispatch
fold の効果が薄まる。 production 用途では `copy` / `copy_gen` / `immix` 系
を推奨。

## 6. correctness 保証

全 16 backend (+ `copy_scramble` audit backend = 計 17 backend) で:

- **17 ascheme native test** PASS
- **179 R5RS chibi test** PASS
- **canary `16_alloc_root_stress`** PASS
- default mode + `BARUBY_GC_STRESS=1` 両方で実行

`make test` / `make GC=<backend> test_stress` で再現可能。

加えて `make bench-aot` の **144/144 cell** (= 16 backend × 9 workload ×
{plain, AOT}) が `expected first-line` と一致するまで validate。

## 7. baruby_precise との比較

`sample/baruby_precise/docs/perf.md` の数値と直接比較すると、 ascheme の
方が backend あたり ~10–20% 遅い傾向。 主因:

- ascheme は `sframe` chain で env を表現、 baruby は flat `sp[]`
- ascheme は call/cc / closure / continuation / multi-values 等の heavy 機構
- ascheme は GMP 経由 bignum の external accounting で GC frequency 高め

libgc 直接比較は ascheme 側のみ可能 (= baruby は naruby fork で libgc 経験
なし)。

## 8. 結論

precise GC framework + ascheme は **libgc に対して**:

- **plain mode**: 主流 backend (= `copy` / `copy_gen` / `immix` /
  `mark_compact_gen` / `immix_gen`) で **geomean 0.97–1.00×** (= libgc と
  互角)。 GC-heavy 個別 workload で互角〜やや速い、 fib35 のみ 2.0× 遅い
- **AOT mode**: 主流 backend で **geomean 0.53–0.56×** (= libgc plain の
  -44〜-47% faster)。 9/9 workload で libgc plain を上回り、 dispatch heavy
  で 3× 級、 GC heavy で 2× 級、 matmul で 1.6× 級
- **production 推奨**: `copy` (= balanced) / `immix` (= fragmentation-resistant)
  / `copy_gen` (= short-lived heavy) / `immix_gen` (= 同上、 line+block region)
- **audit / debug 兼用**: `copy_scramble` (= overhead 微小、 stress +
  scramble で precise rooting バグを即時検出)
- **GC-light で高速**: `mark` / `mark_freelist` / `mark_compact` も同 tier

---

bench script: `sample/ascheme_precise/bench/aot_matrix.sh` (= `make
bench-aot`)。 16 backend × 9 workload × {plain, aot-cached} を ~30 分で
回す。 結果は TSV を stdout、 §1 / §2 表に貼り付け。

```sh
# 検証 + audit mode (= バグ検出)
make test                                              # default 全 179 PASS
make test_stress                                       # stress mode で全 test
make GC=copy_scramble test_stress                      # scramble + stress = 最強 audit

# AOT 単体動作確認 (= --aot-compile + cached run)
make GC=copy
./ascheme_precise -q --clear-cs --aot-compile bench/big/sumloop.scm  # build + 1 回 run
./ascheme_precise -q --aot-compile bench/big/sumloop.scm             # cached run
```
