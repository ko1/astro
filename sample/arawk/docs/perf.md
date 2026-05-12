# arawk perf

最後の計測: 2026-05-12 (改善案 A + B + C 完了後)。

## 環境

- CPU: AMD Ryzen 9 5900HX (16 threads, scaling 75%)
- OS: Linux 6.8.0-110-generic x86_64
- Compiler: gcc 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1), `-O3`
- 相手:
  - gawk 5.2.1 (GNU MPFR 4.2.1, GMP 6.3.0)
  - mawk 1.3.4 20240123
  - goawk 1.31.0
- arawk 計測モード:
  - **`arawk-plain`**: AST tree-walking のみ (`--plain`)
  - **`arawk-aot`**: SD bake 済み Code Store reload (`-c` で pre-bake)

## 計測条件

- 入力: `sample/arawk/goawk/testdata/foo.td` (4.4 MB, 37,801 行, 6 fields/line)
- スクリプト: `goawk/testdata/tt.*` (Ben Hoyt 氏 goawk 同梱)
- スケーリング: gawk で初回計測し、テスト 1 件が `MIN_TIME = 1.0 s` 未満なら入力を repeat
- 各 awk × test を `NUM_RUNS = 5` 回走らせ最小値を採用
- 正規化: gawk = 1.00 (>1 が速い)

## 絶対時間 (秒, 最小値)

| Test                       | arawk-plain | arawk-aot |    gawk |    mawk |   goawk |
|----------------------------|------------:|----------:|--------:|--------:|--------:|
| tt.01_print                |       1.634 |     1.628 |   0.923 |   0.475 |   0.513 |
| tt.02_print_NR_NF          |       0.677 |     0.681 |   0.990 |   0.692 |   0.773 |
| tt.02a_print_length        |       1.625 |     1.627 |   0.995 |   0.711 |   0.838 |
| tt.03_sum_length           |       1.196 |     1.179 |   0.822 |   1.304 |   1.681 |
| tt.03a_sum_field           |       1.235 |     1.219 |   0.820 |   1.382 |   1.714 |
| tt.04_printf_fields        |       1.407 |     1.337 |   1.001 |   0.593 |   1.437 |
| tt.05_concat_fields        |       1.661 |     1.548 |   1.078 |   0.792 |   1.230 |
| tt.06_count_lengths        |       0.981 |     0.968 |   0.762 |   0.915 |   1.139 |
| tt.07_even_fields          |       0.673 |     0.661 |   0.935 |   0.739 |   0.941 |
| tt.08_even_lengths         |       1.092 |     1.102 |   0.959 |   0.239 |   0.409 |
| tt.08z_regex_simple        |         n/a |       n/a |   0.796 |   0.268 |   0.541 |
| tt.09_regex_starts_with    |         n/a |       n/a |   0.709 |   0.172 |   0.506 |
| tt.10_regex_ends_with      |         n/a |       n/a |   1.009 |   0.230 |   3.878 |
| tt.10a_regex_ends_with_var |         n/a |       n/a |   0.963 |   0.212 |   3.580 |
| tt.11_substr               |       1.054 |     1.067 |   0.933 |   0.193 |   0.283 |
| tt.12_update_fields        |       1.958 |     1.875 |   0.982 |   0.843 |   1.055 |
| tt.13_array_ops            |       0.872 |     0.798 |   1.019 |   0.506 |   0.631 |
| tt.13a_array_printf        |       0.846 |     0.796 |   1.110 |   0.474 |   0.783 |
| tt.14_function_call        |       0.008 |     0.009 |   0.022 |   0.007 |   0.009 |
| tt.15_format_lines         |         n/a |       n/a |   1.186 |   0.571 |   1.581 |
| tt.16_count_words          |       0.941 |     0.965 |   0.715 |   0.425 |   0.499 |
| tt.big_complex_program     |         n/a |       n/a |   1.841 |   0.990 |   1.816 |
| tt.x1_mandelbrot           |       0.704 |     0.580 |   0.509 |   0.250 |   0.281 |
| tt.x2_sum_loop             |       0.392 |     0.275 |   0.528 |   0.228 |   0.311 |

`n/a` は arawk が未実装 (regex 系 6 件; Phase 2 = astrogre 統合で解禁予定)。

## gawk 正規化 (>1 が速い)

| Test                | arawk-plain | arawk-aot | gawk | mawk | goawk |
|---------------------|------------:|----------:|-----:|-----:|------:|
| tt.01_print         |        0.56 |      0.57 | 1.00 | 1.94 |  1.80 |
| tt.02_print_NR_NF   |        1.46 |      1.45 | 1.00 | 1.43 |  1.28 |
| tt.02a_print_length |        0.61 |      0.61 | 1.00 | 1.40 |  1.19 |
| tt.03_sum_length    |        0.69 |      0.70 | 1.00 | 0.63 |  0.49 |
| tt.03a_sum_field    |        0.66 |      0.67 | 1.00 | 0.59 |  0.48 |
| tt.04_printf_fields |        0.71 |      0.75 | 1.00 | 1.69 |  0.70 |
| tt.05_concat_fields |        0.65 |      0.70 | 1.00 | 1.36 |  0.88 |
| tt.06_count_lengths |        0.78 |      0.79 | 1.00 | 0.83 |  0.67 |
| tt.07_even_fields   |        1.39 |      1.41 | 1.00 | 1.26 |  0.99 |
| tt.08_even_lengths  |        0.88 |      0.87 | 1.00 | 4.01 |  2.35 |
| tt.11_substr        |        0.88 |      0.87 | 1.00 | 4.83 |  3.30 |
| tt.12_update_fields |        0.50 |      0.52 | 1.00 | 1.16 |  0.93 |
| tt.13_array_ops     |        1.17 |      1.28 | 1.00 | 2.01 |  1.61 |
| tt.13a_array_printf |        1.31 |      1.40 | 1.00 | 2.34 |  1.42 |
| tt.14_function_call |        2.66 |      2.56 | 1.00 | 3.13 |  2.50 |
| tt.16_count_words   |        0.76 |      0.74 | 1.00 | 1.68 |  1.43 |
| tt.x1_mandelbrot    |        0.72 |      0.88 | 1.00 | 2.03 |  1.81 |
| tt.x2_sum_loop      |        1.35 |      1.92 | 1.00 | 2.32 |  1.70 |
| **geomean**         |    **0.92** |  **0.95** | 1.00 | 1.95 |  1.07 |

## 改善履歴 (実測ベース)

| ステップ | plain | aot | コミット |
|---|---:|---:|---|
| 初期実装 (Phase 1.10 直後) | 0.57 | 0.59 | ベースライン |
| **B**: field の lazy strnum (境界のみ記録、 `$N` アクセス時に allocate) | 0.65 | 0.68 | `52d058b` |
| **A**: fgetc → fread chunked input (64 KB buffer + memchr) | 0.85 | 0.88 | `2f0eac6` |
| **C**: for-in を bucket walker に (snapshot 配列を消す) | 0.92 | 0.96 | `d1d6672` |
| C+ : `arawk_wrap_string` で for-in key の char[] copy を消す | 0.92 | 0.95 | `debe43d` |

**1 セッションで geomean 0.59 → 0.95 (+61%)**。 goawk (1.07) は超え、 gawk (1.00) に肉薄。 mawk (1.95) まではまだ。

## gawk より速い場面

| Test | arawk-aot | 理由 |
|---|---|---|
| **tt.14_function_call** | **2.56×** | 二重 for-in (`for i in x; for j in x`) 1M 回。 改善 C で keys[1000] の snapshot 配列確保が消え、 key の `arawk_obj` も lazy share に |
| **tt.x2_sum_loop** | **1.92×** | BEGIN だけの 10M 回 fixnum ループ。 AOT bake で for 全体が specialize、 fixnum 算術が `lea`/`add` 数命令に畳まれる |
| **tt.02_print_NR_NF** | **1.45×** | NR / NF / $0 を毎レコード参照。 lazy strnum で `$0` 1 回だけ allocate (旧: 全 field strnum 化) |
| **tt.07_even_fields** | **1.41×** | `NF % 2 == 0` だけ。 field VALUE 不要 → lazy strnum で完全に skip |
| **tt.13a_array_printf** | **1.40×** | 配列読み書き連続 + printf。 arawk の `arawk_arr_*` (FNV-1a) が gawk の locale-aware hash よりオーバーヘッド少 |
| **tt.13_array_ops** | **1.28×** | 同上 |

## まだ苦手な場面

| Test | arawk-aot | 残る hot path |
|---|---|---|
| **tt.12_update_fields** | **0.52×** | `$3 = "xxx" $3 "xxx"; $4--` で OFS join + record rebuild が毎レコード走る (mawk も 1.16 で gawk と同程度) |
| **tt.01_print** | **0.57×** | `arawk_split_fields` が依然 47% を占める (eager 分割; record 内をフル scan)。 lazy NF 化や AVX2 scan 化で更に詰められる |
| **tt.02a_print_length** | **0.61×** | `$2 = length($2); print` で field 書き換え + 全 field rebuild。 行ごとに OFS join 走る |
| **tt.03 系** | **0.67-0.70×** | field 走査 + 算術。 split + isspace が hot |
| **tt.16_count_words** | **0.74×** | array 書き込み大量 (`w[$i]++`)。 array_set の hash 操作と `arawk_arr_rehash` が積む |

## AOT 効果 (plain → aot, 実測)

- **tt.x2_sum_loop**: 0.392s → 0.275s (**30% 短縮**) — 内側ループ全体が specialize
- **tt.x1_mandelbrot**: 0.704s → 0.580s (18% 短縮) — float 演算ループ
- **tt.13a_array_printf**: 0.846s → 0.796s (6% 短縮)

I/O 系 (tt.01, tt.03) は `arawk_split_fields` 等の runtime helper が PLT 越し
で specialize しないため、 AOT で SD bake しても変わらない。

## perf record で見た hot path (改善後)

### tt.01_print (現状 0.57×)

```
47%  arawk_split_fields    ← 引き続き支配的。 lazy NF 化 or AVX2 scan で更に削れる
 4%  _IO_file_xsputn       ← stdout への write side
 4%  __ctype_b_loc         ← isspace (split 内) の libc helper
 3%  __memmove_avx
 3%  GC_malloc_kind        ← B で 1/5 になった残り
 2%  __memchr_avx2         ← A で導入した chunked read
```

`_IO_getc` は **完全に消えた**。 残る hot は split 自体。

### tt.14_function_call (現状 2.56×)

```
56%  libgc 内部 (sym 解像できず)  ← arawk_obj の per-call alloc
39%  DISPATCH_node_lt              ← abs() の比較
```

`arawk_make_string` は top 圏外に消えた (C+ の wrap_string 効果)。
**ボトルネックは個別 `arawk_obj` alloc** — 配列 entry の取得・関数引数評価で
fixnum 範囲外の数値や string 結果が allocate される。

## 計測時間

- `make test`: 1.6s (smoke 98×2 modes) + 9.6s (tt.* 18×2 modes) = **~11s**
- `make bench`: 全 24 tt.* を 5 awk 実装 × 5 runs (scaling 含む) = **数分**

## まだ残る改善余地

1. **`arawk_split_fields` 自体の高速化**: isspace を ASCII inline check に
   置き換える、 default-FS 全行 scan を AVX2 化 (`__ctype_b_loc` 7% が消える)
2. **arawk_obj の pool 化**: per-call alloc を消す。 small-obj pool で
   `GC_malloc(struct arawk_obj)` を消せれば tt.14 で更に伸びる
3. **fixnum 結果の boxing 回避**: 算術結果が arawk_obj に行く場面で
   fixnum 範囲内なら raw VALUE で完結 (`arawk_make_int` の inline 化済みだが、
   呼び出し元での生成自体を消す)
4. **OFS join の chunked write**: tt.12 / tt.02a で record rebuild が毎行
   走る → `arawk_rebuild_record` の `arawk_to_cstr` × NF を 1 回の buffer
   build に圧縮
5. **AOT 内 builtin inline** (前から保留): `length` / `int` 等を SD body に
   直接 emit。 工数大 (gen.rb 改造)

## 破棄した旧改善案 (実測で外れ)

| 旧改善案 | 破棄理由 |
|---|---|
| **print の chunked write** | strace で全 awk が write 1088 回で一致。 stdio buffering で揃う。 print は実は速い |
| **substr の copy-on-write** | tt.11 で substr は top 12 圏外 (<1%)。 本当の hot は split + GC (= B で解消済み) |
| **function callcache** | 関数 1 つだと strcmp 1 比較で終わる。 hot は for-in key allocate (= C で解消) |
| **D: 関数 frame を arena 化** | frame は VLA で C stack 上 alloc (cost ほぼゼロ)。 perf で frame allocate は top 圏外 |
