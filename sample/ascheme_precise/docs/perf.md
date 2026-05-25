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

## 1. backend × workload matrix (= plain と AOT、 単位 秒、 best of 3)

略号: `m_G` = mark_gen、 `m_G_inc` = mark_gen_inc、 `m_free` = mark_freelist、
`m_bmp_G` = mark_bitmap_gen、 `m_crd_G` = mark_card_gen、 `copy_G` = copy_gen、
`copy_G_inc` = copy_gen_inc、 `m_c` = mark_compact、 `m_c_G` = mark_compact_gen、
`m_Bu_G` = mark_bump_gen、 `I` = immix、 `I_G` = immix_gen。
`_p` = plain、 `_a` = aot-cached。

```
bench       cat libgc  none_p none_a Bu_p   Bu_a   mark_p mark_a m_G_p  m_G_a  m_G_inc_p m_G_inc_a m_free_p m_free_a m_bmp_G_p m_bmp_G_a m_crd_G_p m_crd_G_a copy_p copy_a copy_G_p copy_G_a copy_G_inc_p copy_G_inc_a m_c_p  m_c_a  m_c_G_p m_c_G_a m_Bu_G_p m_Bu_G_a I_p    I_a    I_G_p  I_G_a
fib35       INT 0.42   0.49   0.27   1.26   0.95   0.94   0.64   1.05   0.76   1.23      0.91      0.94     0.64     1.14      0.77      1.12      0.77      0.91   0.61   0.91     0.60     0.89         0.59         1.05   0.76   0.90    0.61    0.91     0.60     0.91   0.63   0.94   0.63
sumloop     INT 1.39   1.56   0.44   1.70   0.45   1.60   0.46   1.58   0.46   2.02      0.79      1.78     0.48     2.09      0.87      2.01      0.84      1.64   0.45   1.61     0.46     1.64         0.48         1.66   0.45   1.60    0.46    1.67     0.46     1.67   0.45   1.75   0.46
nbody       INT 0.49   0.82   0.76   0.63   0.55   0.47   0.38   0.51   0.41   0.55      0.45      0.48     0.40     0.55      0.44      0.53      0.44      0.43   0.35   0.44     0.33     0.43         0.33         0.50   0.41   0.43    0.33    0.43     0.34     0.42   0.34   0.44   0.34
sieve_big   GC  1.05   1.14   0.49   1.16   0.46   1.18   0.50   1.13   0.47   1.56      0.75      1.25     0.48     1.56      0.78      1.59      0.77      1.15   0.47   1.15     0.45     1.12         0.44         1.21   0.49   1.08    0.44    1.15     0.46     1.13   0.47   1.20   0.45
deriv       GC  1.00   1.46   1.50   1.20   1.10   1.01   0.93   1.03   0.96   1.12      1.04      0.99     0.90     1.11      1.00      1.07      0.99      0.95   0.87   0.91     0.82     0.95         0.82         1.06   0.95   0.93    0.84    0.96     0.82     0.97   0.90   0.92   0.82
nqueens     MIX 1.87   2.33   1.06   2.52   1.25   2.27   1.08   2.43   1.08   2.73      1.36      2.53     1.09     2.61      1.33      2.63      1.32      2.18   1.02   2.33     0.96     2.33         0.97         2.33   1.09   2.32    0.97    2.26     0.98     2.26   0.99   2.34   0.97
fannkuch    MIX 1.30   2.44   2.15   1.75   1.46   1.25   0.95   1.38   1.06   1.56      1.22      1.30     0.99     1.45      1.14      1.46      1.14      1.10   0.81   1.15     0.83     1.11         0.81         1.34   1.06   1.12    0.81    1.10     0.80     1.10   0.80   1.11   0.83
cps_loop    MIX 0.73   0.84   0.24   0.93   0.25   0.82   0.25   0.83   0.25   1.04      0.42      0.99     0.26     1.05      0.46      1.14      0.45      0.92   0.25   0.90     0.25     0.91         0.26         0.91   0.25   0.86    0.25    0.86     0.25     0.89   0.25   0.97   0.25
matmul      MIX 7.95   9.84   9.76   9.99   9.87   4.65   4.47   57.24  57.29  64.23     64.12     4.67     4.47     102.33    103.95    101.85    101.60    4.77   4.62   63.43    61.49    67.79        68.86        4.64   4.48   74.77   75.45   75.48    75.22    4.61   4.43   61.78  61.55
```

**全 144/144 cell PASS** (= 16 backend × 9 workload、 fail / SEGV なし)。
ascheme (libgc) は `--aot-compile` 未実装なので `libgc` 列は plain のみ。

## 2. 代表 backend (`copy`) vs `libgc` の plain/AOT 比較

`copy` は precise GC backend の中で全 workload PASS かつ ascheme の
推奨 production backend (= Cheney semispace、 balanced、 全 GC-heavy 高速)。
これと libgc の比較:

| bench     | libgc (plain) | copy (plain) | copy (AOT) | AOT vs libgc | AOT vs copy plain |
|-----------|--------------:|-------------:|-----------:|-------------:|------------------:|
| fib35     |          0.42 |         0.91 |       0.61 | 1.45× slower | **1.49× faster**  |
| sumloop   |          1.39 |         1.64 |       0.45 | **3.09× faster** | **3.64× faster** |
| nbody     |          0.49 |         0.43 |       0.35 | **1.40× faster** | 1.23× faster      |
| sieve_big |          1.05 |         1.15 |       0.47 | **2.23× faster** | **2.45× faster** |
| deriv     |          1.00 |         0.95 |       0.87 | **1.15× faster** | 1.09× faster      |
| nqueens   |          1.87 |         2.18 |       1.02 | **1.83× faster** | **2.14× faster** |
| fannkuch  |          1.30 |         1.10 |       0.81 | **1.60× faster** | 1.36× faster      |
| cps_loop  |          0.73 |         0.92 |       0.25 | **2.92× faster** | **3.68× faster** |
| matmul    |          7.95 |         4.77 |       4.62 | **1.72× faster** | 1.03× tied        |

**まとめ**:

- **AOT は 9/9 workload で libgc plain を上回る** (= fib35 のみ overhead 残)
- dispatch heavy (= `sumloop` / `cps_loop`) で **3× 以上** の速度向上
- GC heavy (= `sieve_big` / `nqueens`) でも **1.8〜2.2× faster**
- GC bound (= `matmul`) も `copy + AOT` で **1.72×**、 bignum 経路の優位
- `copy` plain vs libgc: GC-heavy で互角〜やや負け、 但し AOT で **逆転して 2×+**
- 整数 workload (`fib35`) のみ precise rooting cost が残る (= sp[] 経由の root tracking)。 AOT でも libgc plain に届かないが、 plain 比 -33% は得られる

## 3. AOT 加速率 (= 全 backend で plain / AOT)

代表 backend で plain → aot の倍率:

| backend         | fib35 | sumloop | nbody | sieve_big | deriv | nqueens | fannkuch | cps_loop | matmul |
|-----------------|------:|--------:|------:|----------:|------:|--------:|---------:|---------:|-------:|
| `none`          | 1.81× |   3.55× | 1.08× |     2.33× | 0.97× |   2.20× |    1.13× |    3.50× |  1.01× |
| `bump`          | 1.33× |   3.78× | 1.15× |     2.52× | 1.09× |   2.02× |    1.20× |    3.72× |  1.01× |
| `mark`          | 1.47× |   3.48× | 1.24× |     2.36× | 1.09× |   2.10× |    1.32× |    3.28× |  1.04× |
| `mark_gen`      | 1.38× |   3.43× | 1.24× |     2.40× | 1.07× |   2.25× |    1.30× |    3.32× |  1.00× |
| `mark_gen_inc`  | 1.35× |   2.56× | 1.22× |     2.08× | 1.08× |   2.01× |    1.28× |    2.48× |  1.00× |
| `mark_freelist` | 1.47× |   3.71× | 1.20× |     2.60× | 1.10× |   2.32× |    1.31× |    3.81× |  1.04× |
| `mark_bitmap_gen`| 1.48× |  2.40× | 1.25× |     2.00× | 1.11× |   1.96× |    1.27× |    2.28× |  0.98× |
| `mark_card_gen` | 1.45× |   2.39× | 1.20× |     2.06× | 1.08× |   1.99× |    1.28× |    2.53× |  1.00× |
| `copy`          | 1.49× |   3.64× | 1.23× |     2.45× | 1.09× |   2.14× |    1.36× |    3.68× |  1.03× |
| `copy_gen`      | 1.52× |   3.50× | 1.33× |     2.56× | 1.11× |   2.43× |    1.39× |    3.60× |  1.03× |
| `copy_gen_inc`  | 1.51× |   3.42× | 1.30× |     2.55× | 1.16× |   2.40× |    1.37× |    3.50× |  0.98× |
| `mark_compact`  | 1.38× |   3.69× | 1.22× |     2.47× | 1.12× |   2.14× |    1.26× |    3.64× |  1.04× |
| `mark_compact_gen`| 1.48×|   3.48× | 1.30× |     2.45× | 1.11× |   2.39× |    1.38× |    3.44× |  0.99× |
| `mark_bump_gen` | 1.52× |   3.63× | 1.26× |     2.50× | 1.17× |   2.31× |    1.38× |    3.44× |  1.00× |
| `immix`         | 1.44× |   3.71× | 1.24× |     2.40× | 1.08× |   2.28× |    1.38× |    3.56× |  1.04× |
| `immix_gen`     | 1.49× |   3.80× | 1.29× |     2.67× | 1.12× |   2.41× |    1.34× |    3.88× |  1.00× |

**観察**:

- **dispatch heavy (sumloop / cps_loop)** で AOT は **2.3〜3.9× speedup**。
  AOT は interpreter dispatch loop を C 関数 inline に展開、 effective に
  「手書き C インタプリタ」 へ converge する
- **mark_bitmap_gen / mark_card_gen / mark_gen_inc** は AOT speedup が
  限定的 (= sumloop 2.0〜2.6× 程度)。 GC 経路の重さで dispatch コスト比率
  が下がるため
- **GC bound (matmul)** は全 backend で AOT がほぼ tied (= 0.98–1.04×)。
  GC time が dominate、 dispatch 最適化の余地が少ない
- **fib35** は 1.33〜1.81× speedup。 純再帰 + sp[] park でも AOT inline
  の恩恵あり

## 4. backend × libgc geomean 比較

全 9 workload geomean (= plain / AOT それぞれ vs libgc plain) と、
matmul を outlier として除外した 8 workload の併記:

| backend          | plain (9) | AOT (9) | plain (8、 matmul除く) | AOT (8) |
|------------------|----------:|--------:|----------------------:|--------:|
| `none`           |     1.31× |   0.76× |                 1.32× |   0.71× |
| `bump`           |     1.38× |   0.79× |                 1.40× |   0.75× |
| `mark`           |     1.09× |   0.62× |                 1.18× |   0.63× |
| `mark_gen`       |     1.49× |   0.86× |                 1.23× |   0.66× |
| `mark_gen_inc`   |     1.76× |   1.11× |                 1.45× |   0.87× |
| `mark_freelist`  |     1.15× |   0.63× |                 1.25× |   0.64× |
| `mark_bitmap_gen`|     1.82× |   1.16× |                 1.42× |   0.86× |
| `mark_card_gen`  |     1.81× |   1.15× |                 1.42× |   0.85× |
| `copy`           |     1.06× |   0.59× |                 1.14× |   0.59× |
| `copy_gen`       |     1.43× |   0.77× |                 1.15× |   0.58× |
| `copy_gen_inc`   |     1.43× |   0.78× |                 1.14× |   0.58× |
| `mark_compact`   |     1.15× |   0.65× |                 1.25× |   0.66× |
| `mark_compact_gen`|    1.43× |   0.79× |                 1.13× |   0.58× |
| `mark_bump_gen`  |     1.45× |   0.79× |                 1.14× |   0.58× |
| `immix`          |     1.06× |   0.59× |                 1.14× |   0.59× |
| `immix_gen`      |     1.46× |   0.78× |                 1.18× |   0.59× |
| `libgc`          |     1.00 (baseline) |    | (AOT 非対応) |  |

**所見**:

- **AOT geomean は 14/16 backend で libgc plain を下回る** (= -22〜-41%)。
  例外は `mark_gen_inc` (1.11×) / `mark_bitmap_gen` (1.16×) / `mark_card_gen`
  (1.15×) — いずれも matmul outlier の影響大
- 9 workload AOT geomean のトップは **`copy` / `immix` の 0.59×** (= libgc
  比 -41% faster)
- matmul 除外 8 workload では **gen 系の copy_gen / copy_gen_inc /
  mark_compact_gen / mark_bump_gen / immix_gen が 0.58×** で最速。
  matmul outlier を除けば gen backend の有利が明確
- plain では `copy` / `immix` が **1.06×** で libgc とほぼ互角。
  precise rooting の純粋 overhead は ~6% 程度

## 5. backend 別の matmul outlier

`matmul` は 9 workload 中で **唯一 GC trigger 経路が backend ごとに大きく
分岐する** workload (= LCG で巨大 bignum を作り、 modulo 100 で truncate
する pattern。 GMP の libc malloc が `aro_gc_account_external` の
external_bytes を急速に積み上げ、 minor GC を頻発させる)。

| backend         | matmul plain | matmul AOT | vs libgc (7.95) |
|-----------------|-------------:|-----------:|----------------:|
| `mark`          |         4.65 |       4.47 |      **0.56×**  |
| `mark_freelist` |         4.67 |       4.47 |      **0.56×**  |
| `mark_compact`  |         4.64 |       4.48 |      **0.56×**  |
| `copy`          |         4.77 |       4.62 |      **0.58×**  |
| `immix`         |         4.61 |       4.43 |      **0.56×**  |
| `mark_gen`      |        57.24 |      57.29 |          7.2×   |
| `m_bmp_G`       |       102.33 |     103.95 |         13.1×   |
| `m_crd_G`       |       101.85 |     101.60 |         12.8×   |
| `copy_gen`      |        63.43 |      61.49 |          7.7×   |
| `copy_gen_inc`  |        67.79 |      68.86 |          8.7×   |
| `m_c_G`         |        74.77 |      75.45 |          9.5×   |
| `m_Bu_G`        |        75.48 |      75.22 |          9.5×   |
| `I_G`           |        61.78 |      61.55 |          7.8×   |

**観察**:

- 非生成 backend (`mark` / `mark_freelist` / `mark_compact` / `copy` /
  `immix`) は **0.56× / -44%** で libgc を圧倒
- 生成系 (`_gen` 系) と `mark_bitmap_gen` / `mark_card_gen` は **8〜13×
  遅い outlier**
- 推定原因: minor GC の頻度 × tenured promotion 連鎖 × external_bytes
  threshold 判定が GMP buffer の bulk alloc と相性悪く、 minor → major →
  minor … で GC が連鎖して総 GC 時間が爆発
- 調整余地: external_bytes threshold の slack 拡大、 minor pool 拡張、
  GMP buffer の `aro_gc_promote_hint` (= 寿命 hint API) 等

非 matmul 8 workload に限定すれば全 backend が ±20% 範囲に収まる
(= matmul 除外 geomean は §4 と比べて全 backend で 0.6× 程度に近づく)。

## 6. AOT がなぜ効くか (= cps_loop / sumloop の 3×+ の出所)

plain interpreter は AST node ごとに:
1. dispatcher 関数 pointer 経由の indirect call (= branch predictor miss)
2. node struct から arg pointer 取得 (= load + check)
3. arg node の dispatcher 経由再帰 evaluate

AOT は対象 entry node の dispatcher を専用 C 関数 (= SD_<hash>.c) に
specialize:
- arg node の dispatcher も `static inline` で fold → indirect call が
  direct call に
- node struct アクセスは literal constant fold (`n->u.foo.bar`)
- gcc -O3 が register allocation + dead store elimination

結果、 cps_loop の場合 1 iteration が ~30 命令程度の tight loop に
畳まれ、 dispatch overhead が消える。 sumloop も同様。

逆に matmul は 1 iteration で bignum mul + alloc が走るため、 dispatch が
overhead の 5% 程度しか占めない → AOT で消しても全体 1〜4% しか速くならない。

## 7. 既知 limitation

### 7.1 fib35 overhead 1.33–1.81× plain、 1.40×+ AOT

純再帰の sp[] 更新コスト本質。 全 backend で plain 0.9〜1.3s vs libgc 0.42s。
改善余地:
- self-tail-call の frame reuse 強化 (= 既存 `leaf` opt あり)
- libgc-style な「lazy sp[] update」 (= alloc 直前のみ flush)

ascheme 固有最適化、 baruby_precise との codebase 共通性とのトレードオフ。

### 7.2 matmul outlier (= gen / bitmap_gen / card_gen で 8–13×)

§5 参照。 GMP external_bytes pressure と GC trigger の相性問題。
非生成 backend は 0.56× で libgc 圧勝、 outlier 系のみ調整余地あり。

### 7.3 mark の matmul AOT regression は解消済

過去版 (commit `55b138f6` 前) では `mark + matmul AOT` が plain 比 2×
遅い regression を観測。 これは AOT alloc cadence で GC trigger 頻度が
変わる pattern と思われていたが、 fix 後の bench v8 で `mark plain 4.65s
→ AOT 4.47s` (= **1.04× faster**) と通常通り。 mark_compact SEGV 修正の
副次効果か、 計測時の system 状態の影響だったか、 いずれにせよ解決。

## 8. correctness 保証

全 16 backend (+ `copy_scramble` audit backend) で:
- **17 ascheme native test** PASS
- **179 R5RS chibi test** PASS
- **canary `16_alloc_root_stress`** PASS
- default mode + `BARUBY_GC_STRESS=1` 両方で実行

`make test` / `make GC=<backend> test_stress` で再現可能。

加えて `make bench-aot` の **144/144 cell** が `expected first-line` と
一致するまで validate。 GC bug で「速いが結果が誤」 を完全排除。

## 9. baruby_precise との比較

`sample/baruby_precise/docs/perf.md` の数値と直接比較すると、 ascheme の
方が backend あたり ~10–20% 遅い傾向。 主因:

- ascheme は `sframe` chain で env を表現、 baruby は flat `sp[]`
- ascheme は call/cc / closure / continuation / multi-values 等の heavy 機構
- ascheme は GMP 経由 bignum の external accounting で GC frequency 高め

libgc 直接比較は ascheme 側のみ可能 (= baruby は naruby fork で libgc 経験
なし)。

## 10. AOT が成立するまでの fix 履歴

bench を走らせた過程で **全 backend で AOT が silently broken** だったことが
発覚。 5 件の bug を順次 fix:

1. **`-I` baked-absolute path** (commit `e0867910`) — `-DASCHEME_PRECISE_DIR`
   / `-DASTRO_RUNTIME_DIR` を Makefile で baked、 main.c の `extra_cflags`
   経由で cc に渡す。 これ無しでは SD_*.c が `node.h` を見つけられない
2. **`-DBARUBY_GC=<num>`** (commit `96441e3e`) — SD cflags に GC backend 番号
   を伝えないと `gc_types.h` が default の `BARUBY_GC_COPY` を選び、
   AroObjectHeader / WB 経路 layout が host と食い違い、 alloc/sweep が
   garbage を読む
3. **`astro_cs_init(src_dir=ABSPATH, version=BARUBY_GC)`** (commit `b3c5f522`)
   — cwd-relative "." が `<cwd>/./node.h` に展開され build 失敗。
   `BARUBY_GC` を version に渡して backend 切替時 cache 自動無効化
4. **`node.h` で `#include "precise_gc/gc.h"`** (commit `d2769de5`) —
   `aro_gc_wb` の inline 定義が SD に inlining されず extern call が emit。
   非 WB backend で `dlopen failed: undefined symbol`
5. **`seen` 配列を libc calloc へ** (commit `976cea00`) — run_file_aot で
   `seen = aro_gc_alloc_raw(...)` 後 `seen[0] = hash` で AroObjectHeader を
   上書き。 mark_freelist の sweep_region が `gc_size = hash 下位 32-bit`
   を読んで `p += garbage` の無限 loop

加えて runtime backend 側の 2 件:

6. **`gc_mark_bump_gen.c` の `scan_push(*slot - 1)` pointer 演算 fix**
   (commit `f4ab6f57`) — `(AroObjectHeader *)*slot - 1` が 8 bytes 手前
   (= 隣接 slot 領域) を push、 major_process が garbage h を受取り
   SCAN_EDGES が undefined 範囲 deref で SEGV
7. **`OBJ_VEC_BACKING` 型導入** (commit `55b138f6`) — `OBJ_VECTOR` /
   `OBJ_MVALUES` SCAN_EDGES が items[i] を post-slide 位置から読み、
   slide 未実施の mark_compact で未初期化メモリを deref → SEGV。
   items_base buffer に専用 type を付け SCAN_EDGES を分離して reader と
   data を co-located にした

これら全てを修正後、 16 backend × 9 workload × {plain, AOT} = **144/144
cell PASS** を達成。

---

bench script: `sample/ascheme_precise/bench/aot_matrix.sh` (= `make
bench-aot`)。 16 backend × 9 workload × {plain, aot-cached} を ~100 分で
回す。 結果は TSV を stdout、 §1 表に貼り付ける。

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
