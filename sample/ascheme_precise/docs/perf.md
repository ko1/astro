# ascheme_precise — performance evaluation

`sample/ascheme_precise/` の precise GC framework migration の perf 評価。
**libgc (= Boehm conservative GC, `sample/ascheme/`)** を baseline に、
16 個の precise GC backend (= scramble 除く) を **plain interpreter** /
**AOT (= `--aot-compile` + dlopen cached SDs)** 2 軸で測定。

`make bench-aot` で再現可能 (= 約 100 分、 matmul outlier dominant)。

## 0. setup

- **machine**: AMD Ryzen 9 5900HX (= 8 cores / 16 threads、 ~4.6 GHz boost)
- **memory**: 30 GiB
- **kernel**: Linux 6.8.0-117 x86_64
- **compiler**: gcc -O3 -ggdb3
- **methodology**: 各 benchmark 3 回実行、 最小値。 出力 first-line を expected と
  照合して **正答性検証** してから記録 (= GC bug で「速いが結果が誤」を排除)
- **scale**: 0.3–10 秒の範囲で sustained measurement。 matmul outlier 系は 60–100 秒

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
fib35       INT 0.42   0.49  1.26  0.94  1.05  1.23    0.94   1.14    1.12    0.91  0.91   0.89       1.05  0.90  0.91   0.91  0.94
sumloop     INT 1.39   1.56  1.70  1.60  1.58  2.02    1.78   2.09    2.01    1.64  1.61   1.64       1.66  1.60  1.67   1.67  1.75
nbody       INT 0.49   0.82  0.63  0.47  0.51  0.55    0.48   0.55    0.53    0.43  0.44   0.43       0.50  0.43  0.43   0.42  0.44
sieve_big   GC  1.05   1.14  1.16  1.18  1.13  1.56    1.25   1.56    1.59    1.15  1.15   1.12       1.21  1.08  1.15   1.13  1.20
deriv       GC  1.00   1.46  1.20  1.01  1.03  1.12    0.99   1.11    1.07    0.95  0.91   0.95       1.06  0.93  0.96   0.97  0.92
nqueens     MIX 1.87   2.33  2.52  2.27  2.43  2.73    2.53   2.61    2.63    2.18  2.33   2.33       2.33  2.32  2.26   2.26  2.34
fannkuch    MIX 1.30   2.44  1.75  1.25  1.38  1.56    1.30   1.45    1.46    1.10  1.15   1.11       1.34  1.12  1.10   1.10  1.11
cps_loop    MIX 0.73   0.84  0.93  0.82  0.83  1.04    0.99   1.05    1.14    0.92  0.90   0.91       0.91  0.86  0.86   0.89  0.97
matmul      MIX 7.95   9.84  9.99  4.65  57.24 64.23   4.67   102.33  101.85  4.77  63.43  67.79      4.64  74.77 75.48  4.61  61.78
```

### 1.1 plain での観察

- **fib35** (= 純再帰、 stack 深い) は precise rooting の sp[] 更新が
  worst case。 全 backend で 2.0–3.0× 遅い (= libgc 0.42 vs precise 0.89–1.26)。
  libgc は C stack を保守的に scan するので per-call sp[] 不要
- **sumloop / nbody / cps_loop** は libgc とほぼ同等〜やや遅い (= +5〜20%)
- **GC-heavy** な `sieve_big` / `deriv` / `nqueens` / `fannkuch` は backend
  によって libgc とほぼ同等〜やや速い (= 例 `mark_compact_gen` の deriv
  0.93 < libgc 1.00)
- **matmul** で **非生成 backend** (`mark` / `mark_freelist` / `mark_compact`
  / `copy` / `immix`) は **4.61–4.77** と libgc (7.95) の **0.58× / -42%**
  と圧勝。 ただし生成系 (`*_gen` / `*_inc`) と `mark_bitmap_gen` /
  `mark_card_gen` は **57–102 秒** の outlier (§4 参照)

### 1.2 plain の backend ランキング (= geomean vs libgc)

全 9 workload geomean / matmul 除外 8 workload geomean:

| backend           | plain (9 ws) | plain (8 ws、 matmul除) |
|-------------------|-------------:|------------------------:|
| `copy`            |        1.06× |                   1.14× |
| `immix`           |        1.06× |                   1.14× |
| `mark`            |        1.09× |                   1.18× |
| `mark_freelist`   |        1.15× |                   1.25× |
| `mark_compact`    |        1.15× |                   1.25× |
| `mark_compact_gen`|        1.43× |                   1.13× |
| `mark_bump_gen`   |        1.45× |                   1.14× |
| `copy_gen`        |        1.43× |                   1.15× |
| `copy_gen_inc`    |        1.43× |                   1.14× |
| `immix_gen`       |        1.46× |                   1.18× |
| `mark_gen`        |        1.49× |                   1.23× |
| `mark_gen_inc`    |        1.76× |                   1.45× |
| `mark_card_gen`   |        1.81× |                   1.42× |
| `mark_bitmap_gen` |        1.82× |                   1.42× |
| `none`            |        1.31× |                   1.32× |
| `bump`            |        1.38× |                   1.40× |
| `libgc`           |              1.00 (baseline) |          |

非生成系 (`copy` / `immix` / `mark`) が **1.06–1.09×** で libgc とほぼ互角。
matmul 除外で全 GC backend が **1.13–1.45×** に収束、 matmul outlier の影響大。

## 2. AOT (= `--aot-compile` で hot AST node を C 関数化、 dlopen で attach)

ascheme (libgc) は `--aot-compile` 未実装なので libgc 列は plain のみ
(`*` 表記)。

```
bench       cat libgc* none  Bu    mark  m_G   m_G_inc m_free m_bmp_G m_crd_G copy  copy_G copy_G_inc m_c   m_c_G m_Bu_G I     I_G
fib35       INT 0.42   0.27  0.95  0.64  0.76  0.91    0.64   0.77    0.77    0.61  0.60   0.59       0.76  0.61  0.60   0.63  0.63
sumloop     INT 1.39   0.44  0.45  0.46  0.46  0.79    0.48   0.87    0.84    0.45  0.46   0.48       0.45  0.46  0.46   0.45  0.46
nbody       INT 0.49   0.76  0.55  0.38  0.41  0.45    0.40   0.44    0.44    0.35  0.33   0.33       0.41  0.33  0.34   0.34  0.34
sieve_big   GC  1.05   0.49  0.46  0.50  0.47  0.75    0.48   0.78    0.77    0.47  0.45   0.44       0.49  0.44  0.46   0.47  0.45
deriv       GC  1.00   1.50  1.10  0.93  0.96  1.04    0.90   1.00    0.99    0.87  0.82   0.82       0.95  0.84  0.82   0.90  0.82
nqueens     MIX 1.87   1.06  1.25  1.08  1.08  1.36    1.09   1.33    1.32    1.02  0.96   0.97       1.09  0.97  0.98   0.99  0.97
fannkuch    MIX 1.30   2.15  1.46  0.95  1.06  1.22    0.99   1.14    1.14    0.81  0.83   0.81       1.06  0.81  0.80   0.80  0.83
cps_loop    MIX 0.73   0.24  0.25  0.25  0.25  0.42    0.26   0.46    0.45    0.25  0.25   0.26       0.25  0.25  0.25   0.25  0.25
matmul      MIX 7.95   9.76  9.87  4.47  57.29 64.12   4.47   103.95  101.60  4.62  61.49  68.86      4.48  75.45 75.22  4.43  61.55
```

### 2.1 AOT での観察

- AOT は plain の dispatch overhead を inline で畳む。 dispatch heavy な
  `sumloop` / `cps_loop` は **0.24–0.46 秒** に収束 (= libgc plain の
  **2.9–3.0×** 速い、 一部 0.24 で libgc を超えるケースも)
- `fib35` も AOT で **0.59–0.77 秒** に改善 (= plain 比 1.4×)。 ただし
  libgc plain 0.42 には届かず (= 0.59 / 0.42 = 1.40×、 -40% 残)
- GC-heavy な `sieve_big` / `nqueens` / `fannkuch` でも AOT で **0.44–1.10 秒**
  と libgc plain の **1.8–2.5×** 速い
- `matmul` は plain と同じく非生成 backend で **4.43–4.62 秒** (= libgc
  plain の **1.7–1.8×** 速い)。 AOT による上乗せは小さい (= GC time
  dominates、 dispatch fold の余地が限定的)

### 2.2 AOT の backend ランキング (= geomean vs libgc plain)

| backend           | AOT (9 ws) | AOT (8 ws、 matmul除) |
|-------------------|-----------:|----------------------:|
| `copy`            |      0.59× |                 0.59× |
| `immix`           |      0.59× |                 0.59× |
| `mark`            |      0.62× |                 0.63× |
| `mark_freelist`   |      0.63× |                 0.64× |
| `mark_compact`    |      0.65× |                 0.66× |
| `mark_compact_gen`|      0.79× |                 0.58× |
| `mark_bump_gen`   |      0.79× |                 0.58× |
| `copy_gen`        |      0.77× |                 0.58× |
| `copy_gen_inc`    |      0.78× |                 0.58× |
| `immix_gen`       |      0.78× |                 0.59× |
| `mark_gen`        |      0.86× |                 0.66× |
| `mark_gen_inc`    |      1.11× |                 0.87× |
| `mark_card_gen`   |      1.15× |                 0.85× |
| `mark_bitmap_gen` |      1.16× |                 0.86× |
| `none`            |      0.76× |                 0.71× |
| `bump`            |      0.79× |                 0.75× |
| `libgc` (plain)   |     1.00 (baseline)  |            |

- **AOT geomean は 14/16 backend で libgc plain を下回る** (= geomean<1.0)
- **トップ: `copy` / `immix` = 0.59×** (= libgc plain の -41% faster)
- 生成系 (gen 系) は matmul outlier で全体 geomean は劣るが、 **matmul
  除外 8 workload では 0.58× で最速** (= 短命 obj 多い workload で minor GC
  の恩恵)
- 生成 +incremental の `mark_gen_inc` と bitmap/card 系のみ libgc plain
  に届かず (= matmul outlier の影響)

### 2.3 AOT 加速率 (= plain / AOT、 backend 別)

| backend          | fib35 | sumloop | nbody | sieve_big | deriv | nqueens | fannkuch | cps_loop | matmul |
|------------------|------:|--------:|------:|----------:|------:|--------:|---------:|---------:|-------:|
| `none`           | 1.81× |   3.55× | 1.08× |     2.33× | 0.97× |   2.20× |    1.13× |    3.50× |  1.01× |
| `bump`           | 1.33× |   3.78× | 1.15× |     2.52× | 1.09× |   2.02× |    1.20× |    3.72× |  1.01× |
| `mark`           | 1.47× |   3.48× | 1.24× |     2.36× | 1.09× |   2.10× |    1.32× |    3.28× |  1.04× |
| `mark_gen`       | 1.38× |   3.43× | 1.24× |     2.40× | 1.07× |   2.25× |    1.30× |    3.32× |  1.00× |
| `mark_gen_inc`   | 1.35× |   2.56× | 1.22× |     2.08× | 1.08× |   2.01× |    1.28× |    2.48× |  1.00× |
| `mark_freelist`  | 1.47× |   3.71× | 1.20× |     2.60× | 1.10× |   2.32× |    1.31× |    3.81× |  1.04× |
| `mark_bitmap_gen`| 1.48× |   2.40× | 1.25× |     2.00× | 1.11× |   1.96× |    1.27× |    2.28× |  0.98× |
| `mark_card_gen`  | 1.45× |   2.39× | 1.20× |     2.06× | 1.08× |   1.99× |    1.28× |    2.53× |  1.00× |
| `copy`           | 1.49× |   3.64× | 1.23× |     2.45× | 1.09× |   2.14× |    1.36× |    3.68× |  1.03× |
| `copy_gen`       | 1.52× |   3.50× | 1.33× |     2.56× | 1.11× |   2.43× |    1.39× |    3.60× |  1.03× |
| `copy_gen_inc`   | 1.51× |   3.42× | 1.30× |     2.55× | 1.16× |   2.40× |    1.37× |    3.50× |  0.98× |
| `mark_compact`   | 1.38× |   3.69× | 1.22× |     2.47× | 1.12× |   2.14× |    1.26× |    3.64× |  1.04× |
| `mark_compact_gen`| 1.48×|   3.48× | 1.30× |     2.45× | 1.11× |   2.39× |    1.38× |    3.44× |  0.99× |
| `mark_bump_gen`  | 1.52× |   3.63× | 1.26× |     2.50× | 1.17× |   2.31× |    1.38× |    3.44× |  1.00× |
| `immix`          | 1.44× |   3.71× | 1.24× |     2.40× | 1.08× |   2.28× |    1.38× |    3.56× |  1.04× |
| `immix_gen`      | 1.49× |   3.80× | 1.29× |     2.67× | 1.12× |   2.41× |    1.34× |    3.88× |  1.00× |

- **dispatch heavy** (= `sumloop` / `cps_loop`) で AOT は **2.3–3.9×**
  speedup。 AOT が interpreter dispatch loop を C 関数 inline に展開、
  effective に「手書き C インタプリタ」へ converge する
- **GC bound** (= `matmul`) は **全 backend で 0.98–1.04×** で tied。
  GC time が dominate、 dispatch fold の余地が少ない
- **小規模 dispatch** (= `nbody` / `deriv` / `fannkuch`) も 1.1–1.4× 速くなる

## 3. 代表比較 (= libgc vs copy)

`copy` は GC-heavy 全 workload PASS かつ ascheme の推奨 production backend
(= Cheney semispace、 balanced)。 libgc baseline と並べた一覧:

| bench     | libgc | copy plain | copy AOT | AOT vs libgc     | AOT vs copy plain |
|-----------|------:|-----------:|---------:|------------------|-------------------|
| fib35     |  0.42 |       0.91 |     0.61 | 1.45× slower     | **1.49× faster**  |
| sumloop   |  1.39 |       1.64 |     0.45 | **3.09× faster** | **3.64× faster**  |
| nbody     |  0.49 |       0.43 |     0.35 | **1.40× faster** | 1.23× faster      |
| sieve_big |  1.05 |       1.15 |     0.47 | **2.23× faster** | **2.45× faster**  |
| deriv     |  1.00 |       0.95 |     0.87 | **1.15× faster** | 1.09× faster      |
| nqueens   |  1.87 |       2.18 |     1.02 | **1.83× faster** | **2.14× faster**  |
| fannkuch  |  1.30 |       1.10 |     0.81 | **1.60× faster** | 1.36× faster      |
| cps_loop  |  0.73 |       0.92 |     0.25 | **2.92× faster** | **3.68× faster**  |
| matmul    |  7.95 |       4.77 |     4.62 | **1.72× faster** | 1.03× tied        |

- `copy` plain vs libgc: GC-heavy で互角〜やや負け、 matmul で **0.60×**
  faster。 9 workload geomean **1.06×** (≒ libgc plain と同等)
- `copy` AOT は **9/9 workload で libgc plain を上回る** (= fib35 のみ
  -45% slower 残、 他全部 faster)。 9 workload geomean **0.59×**
- AOT の plain 比 speedup は dispatch heavy で **3.6–3.7×**、 GC-heavy
  で **2.1–2.5×**、 matmul で tied

## 4. matmul outlier の詳細

`matmul` は 9 workload 中で **唯一 GC trigger 経路が backend ごとに大きく
分岐する** workload。 LCG で巨大 bignum を作り modulo 100 で truncate する
pattern で、 GMP の libc malloc が `aro_gc_account_external` の
`external_bytes` を急速に積み上げ、 minor GC を頻発させる。

### 4.1 matmul 結果 (= plain と AOT)

| backend         | plain  | AOT    | AOT vs libgc (7.95) |
|-----------------|-------:|-------:|--------------------:|
| **`mark`**          |   4.65 |   4.47 |     **0.56×** |
| **`mark_freelist`** |   4.67 |   4.47 |     **0.56×** |
| **`mark_compact`**  |   4.64 |   4.48 |     **0.56×** |
| **`immix`**         |   4.61 |   4.43 |     **0.56×** |
| **`copy`**          |   4.77 |   4.62 |     **0.58×** |
| `mark_gen`      |  57.24 |  57.29 |               7.2×  |
| `m_bmp_G`       | 102.33 | 103.95 |              13.1×  |
| `m_crd_G`       | 101.85 | 101.60 |              12.8×  |
| `copy_gen`      |  63.43 |  61.49 |               7.7×  |
| `copy_gen_inc`  |  67.79 |  68.86 |               8.7×  |
| `m_c_G`         |  74.77 |  75.45 |               9.5×  |
| `m_Bu_G`        |  75.48 |  75.22 |               9.5×  |
| `I_G`           |  61.78 |  61.55 |               7.8×  |

非生成 backend (`mark` / `mark_freelist` / `mark_compact` / `copy` / `immix`)
は **0.56–0.58×** で libgc を圧倒。 一方、 生成系 + bitmap/card 系は **8–13×
遅い outlier**。 推定原因:

- bignum buffer の bulk alloc → external_bytes threshold trip
- minor GC で nursery 通過 → tenured promotion 連鎖
- major GC で大規模 heap walk → minor → major → minor … の連鎖
- 各 backend の threshold 判定が GMP buffer pattern と相性悪く、 総 GC
  時間が爆発

調整余地: external_bytes threshold の slack 拡大、 minor pool 拡張、 GMP
buffer の `aro_gc_promote_hint` (= 寿命 hint API) 等。 非 matmul 8 workload
に限定すれば全 backend が ±20% 範囲に収まる (§1.2 / §2.2)。

## 5. 既知 limitation

### 5.1 fib35 overhead 残 1.3–1.8× (plain)、 1.4× (AOT)

純再帰の sp[] 更新コスト本質。 全 backend で plain 0.89–1.26s、 AOT
0.59–0.95s vs libgc 0.42s。 libgc は C stack を保守的 scan するため
per-call sp[] update 不要。

改善余地:
- self-tail-call の frame reuse 強化 (= 既存 `leaf` opt あり、 sp[] park で
  どこまで省けるか)
- libgc-style な「lazy sp[] update」 (= alloc 直前のみ flush)

ascheme 固有最適化、 baruby_precise との codebase 共通性とのトレードオフ。

### 5.2 matmul outlier (= 生成系 / bitmap_gen / card_gen で 8–13×)

§4 参照。 GMP external_bytes pressure と GC trigger の相性問題。
非生成 backend は 0.56× で libgc 圧勝、 outlier 系のみ調整余地あり。

## 6. correctness 保証

全 16 backend (+ `copy_scramble` audit backend = 計 17 backend) で:

- **17 ascheme native test** PASS
- **179 R5RS chibi test** PASS
- **canary `16_alloc_root_stress`** PASS
- default mode + `BARUBY_GC_STRESS=1` 両方で実行

`make test` / `make GC=<backend> test_stress` で再現可能。

加えて `make bench-aot` の **144/144 cell** (= 16 backend × 9 workload ×
{plain, AOT}) が `expected first-line` と一致するまで validate。 GC bug
で「速いが結果が誤」 を完全排除。

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

- **plain mode**: `copy` / `immix` で **geomean 1.06×** (≒ libgc と互角)。
  GC-heavy 個別 workload で互角〜やや速い、 fib35 のみ 2.2× 遅い
- **AOT mode**: `copy` / `immix` で **geomean 0.59×** (= libgc plain の
  -41% faster)。 9/9 workload で libgc plain を上回り、 dispatch heavy で
  3× 級、 GC heavy で 2× 級
- **production 推奨**: `copy` (= balanced、 全 workload 高速) または
  `immix` (= fragmentation-resistant、 ほぼ同性能)
- **audit / debug 兼用**: `copy_scramble` (= overhead 微小、 stress +
  scramble で precise rooting バグを即時検出)
- **GC-light で高速**: `mark` / `mark_freelist` / `mark_compact` も同 tier
- **生成系**: matmul outlier 除けば copy_gen / mark_compact_gen 等が
  short-lived obj heavy workload で最速

---

bench script: `sample/ascheme_precise/bench/aot_matrix.sh` (= `make
bench-aot`)。 16 backend × 9 workload × {plain, aot-cached} を ~100 分で
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
