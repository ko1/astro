# arawk perf

最後の計測: 2026-05-11。

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
- スケーリング: gawk で初回計測し、テスト 1 件が `MIN_TIME = 1.0 s` 未満なら入力を repeat (`benchmark/scratch/foo.td.xN` キャッシュ)
- 各 awk × test を `NUM_RUNS = 5` 回走らせ最小値を採用
- 正規化: gawk = 1.00 (高いほど速い)

## 絶対時間 (秒, 最小値)

```
Test                           | arawk-plain |  arawk-aot |    gawk |    mawk |   goawk
-------------------------------+-------------+------------+---------+---------+--------
tt.01_print                    |       5.370 |      5.336 |   0.920 |   0.479 |   0.534
tt.02_print_NR_NF              |       1.927 |      1.917 |   0.994 |   0.724 |   0.806
tt.02a_print_length            |       1.820 |      1.801 |   0.986 |   0.737 |   0.895
tt.03_sum_length               |       4.340 |      4.291 |   0.842 |   1.322 |   1.682
tt.03a_sum_field               |       4.538 |      5.581 |   1.033 |   1.672 |   1.698
tt.04_printf_fields            |       1.744 |      1.739 |   1.235 |   0.734 |   1.708
tt.05_concat_fields            |       1.559 |      1.446 |   1.007 |   0.733 |   1.064
tt.06_count_lengths            |       2.314 |      2.282 |   0.634 |   0.743 |   0.956
tt.07_even_fields              |       2.028 |      2.061 |   0.907 |   0.715 |   0.869
tt.08_even_lengths             |       2.784 |      2.709 |   0.737 |   0.189 |   0.319
tt.08z_regex_simple            |         n/a |        n/a |   0.591 |   0.198 |   0.393
tt.09_regex_starts_with        |         n/a |        n/a |   0.512 |   0.126 |   0.360
tt.10_regex_ends_with          |         n/a |        n/a |   0.827 |   0.197 |   3.238
tt.10a_regex_ends_with_var     |         n/a |        n/a |   0.841 |   0.196 |   3.110
tt.11_substr                   |       2.691 |      2.657 |   0.718 |   0.155 |   0.241
tt.12_update_fields            |       1.906 |      1.789 |   0.844 |   0.743 |   0.959
tt.13_array_ops                |       0.896 |      0.840 |   1.049 |   0.523 |   0.659
tt.13a_array_printf            |       0.900 |      0.864 |   1.194 |   0.491 |   0.805
tt.14_function_call            |       0.050 |      0.049 |   0.022 |   0.007 |   0.009
tt.15_format_lines             |         n/a |        n/a |   1.202 |   0.592 |   1.619
tt.16_count_words              |       1.066 |      1.015 |   0.731 |   0.437 |   0.518
tt.big_complex_program         |         n/a |        n/a |   1.905 |   1.088 |   1.906
tt.x1_mandelbrot               |       0.688 |      0.596 |   0.531 |   0.254 |   0.282
tt.x2_sum_loop                 |       0.408 |      0.296 |   0.538 |   0.222 |   0.327
```

`n/a` は arawk が未実装 (regex 系 6 件; Phase 2 = astrogre 統合で解禁予定)。

## gawk 正規化 (>1 が速い)

```
Test                           | arawk-plain |  arawk-aot |   gawk |   mawk |  goawk
-------------------------------+-------------+------------+--------+--------+-------
tt.01_print                    |        0.17 |       0.17 |   1.00 |   1.92 |   1.72
tt.02_print_NR_NF              |        0.52 |       0.52 |   1.00 |   1.37 |   1.23
tt.02a_print_length            |        0.54 |       0.55 |   1.00 |   1.34 |   1.10
tt.03_sum_length               |        0.19 |       0.20 |   1.00 |   0.64 |   0.50
tt.03a_sum_field               |        0.23 |       0.19 |   1.00 |   0.62 |   0.61
tt.04_printf_fields            |        0.71 |       0.71 |   1.00 |   1.68 |   0.72
tt.05_concat_fields            |        0.65 |       0.70 |   1.00 |   1.37 |   0.95
tt.06_count_lengths            |        0.27 |       0.28 |   1.00 |   0.85 |   0.66
tt.07_even_fields              |        0.45 |       0.44 |   1.00 |   1.27 |   1.04
tt.08_even_lengths             |        0.26 |       0.27 |   1.00 |   3.89 |   2.31
tt.11_substr                   |        0.27 |       0.27 |   1.00 |   4.62 |   2.97
tt.12_update_fields            |        0.44 |       0.47 |   1.00 |   1.14 |   0.88
tt.13_array_ops                |        1.17 |       1.25 |   1.00 |   2.00 |   1.59
tt.13a_array_printf            |        1.33 |       1.38 |   1.00 |   2.43 |   1.48
tt.14_function_call            |        0.44 |       0.45 |   1.00 |   3.26 |   2.40
tt.16_count_words              |        0.69 |       0.72 |   1.00 |   1.67 |   1.41
tt.x1_mandelbrot               |        0.77 |       0.89 |   1.00 |   2.09 |   1.88
tt.x2_sum_loop                 |        1.32 |       1.82 |   1.00 |   2.43 |   1.65
-------------------------------+-------------+------------+--------+--------+-------
geomean                        |        0.58 |       0.59 |   1.00 |   1.93 |   1.07
```

- 最終 geomean: **arawk-plain 0.58×, arawk-aot 0.59× vs gawk**
- mawk が 1.93× で圧倒的 (bytecode VM + 内部 fastpath)
- goawk は gawk とほぼ同等 (1.07×)

## arawk が gawk より速い場面

| Test | arawk-aot | 理由 |
|---|---|---|
| **tt.x2_sum_loop** | **1.82×** | BEGIN だけの 10M 回 fixnum ループ。AOT bake で for 全体が specialize、`AWK_IS_FIX(a)&AWK_IS_FIX(b)` + `__builtin_add_overflow` が `lea`/`add` 数命令に畳まれる。gawk は内部 all-double |
| **tt.13a_array_printf** | **1.38×** | 配列読み書きが連続。arawk の `awk_arr_*` (FNV-1a + 単純 chained bucket) は gawk の locale-aware hash よりオーバーヘッド少ない |
| **tt.13_array_ops** | **1.25×** | 同上 |

## 苦手な場面

| Test | arawk-aot | 理由 |
|---|---|---|
| **tt.01_print** | **0.17×** | 入力をそのまま `print` 出力。`awk_print_value` が要素ごとに `fwrite` を呼ぶ → syscall を稼ぐ。gawk/mawk は内部 buffer で chunked write |
| **tt.03_sum_length / tt.03a_sum_field** | **0.19-0.23×** | 全行で field 取得 + 算術。`awk_split_fields` が毎行 strnum allocate (GC_malloc_atomic × 6/行) して GC 圧力高い。gawk の field-pointer 共有最適化に対して loss |
| **tt.08_even_lengths** | **0.27×** | `length` を `$0` に対して呼ぶ。awk_length の cstr 経由が重い |
| **tt.11_substr** | **0.27×** | `substr` 毎回 fresh string allocation。gawk は参照ベースの slice 表現 |
| **tt.14_function_call** | **0.45×** | 関数呼び出し毎に `c->func_set` を線形検索 (strcmp)。astr の `astr_callcache` (inline-stored body pointer) 未実装 |

## AOT 効果 (plain → aot)

ほとんどのテストで AOT が 5% 以内の微改善か悪化なし。SD bake が効くのは:

- **tt.x2_sum_loop**: 0.408s → 0.296s (**28% 短縮**) — 内側ループ全体が specialize される最強形
- **tt.x1_mandelbrot**: 0.688s → 0.596s (13% 短縮) — float 演算ループ
- **tt.12_update_fields**: 1.906s → 1.789s (6% 短縮)

I/O 系 (tt.01, tt.02) や builtin 系 (tt.11, tt.06) は AOT の効果ほぼなし。SD inline できるのは tree-walking 部分だけで、runtime 呼び出しは PLT 経由のまま残るため。

## 計測時間

- `make test`: 1.34s (smoke 74×2 modes) + 10.42s (tt.* 18×2 modes) = **~12s**
- `make bench`: 全 24 tt.* を 5 awk 実装 × 5 runs (scaling 含む) = **数分**

## 次の perf 改善案

1. **`print` の chunked write**: 1 文 1 fwrite に統合 → tt.01/02 に効くはず
2. **field の lazy strnum**: 全 field を毎回 allocate せず、`$N` アクセス時に必要なものだけ → tt.03 系
3. **`substr` の copy-on-write**: heap ptr 共有 → tt.11
4. **function call cache**: callsite に `function_entry *` を inline 保持 → tt.14
5. **AOT 内 builtin inline**: `length`/`substr`/`int` 等の hot builtin を SD body に直接埋め込む
