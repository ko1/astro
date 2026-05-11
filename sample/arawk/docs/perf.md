# arawk perf

最後の計測: 2026-05-11 (Phase 1 完了 + do-while/nextfile 後)。

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

| Test                       | arawk-plain | arawk-aot |    gawk |    mawk |   goawk |
|----------------------------|------------:|----------:|--------:|--------:|--------:|
| tt.01_print                |       5.456 |     5.785 |   0.990 |   0.503 |   0.552 |
| tt.02_print_NR_NF          |       1.735 |     1.936 |   1.001 |   0.728 |   0.764 |
| tt.02a_print_length        |       1.989 |     1.945 |   1.047 |   0.754 |   0.896 |
| tt.03_sum_length           |       4.285 |     4.289 |   0.819 |   1.249 |   1.608 |
| tt.03a_sum_field           |       4.675 |     4.730 |   0.871 |   1.414 |   1.751 |
| tt.04_printf_fields        |       1.753 |     1.613 |   1.018 |   0.608 |   1.473 |
| tt.05_concat_fields        |       1.656 |     1.564 |   0.992 |   0.731 |   1.121 |
| tt.06_count_lengths        |       3.023 |     3.267 |   0.860 |   0.961 |   1.224 |
| tt.07_even_fields          |       2.404 |     3.191 |   1.830 |   1.593 |   2.406 |
| tt.08_even_lengths         |       2.341 |     2.021 |   0.527 |   0.125 |   0.223 |
| tt.08z_regex_simple        |         n/a |       n/a |   0.620 |   0.200 |   0.396 |
| tt.09_regex_starts_with    |         n/a |       n/a |   0.562 |   0.146 |   0.398 |
| tt.10_regex_ends_with      |         n/a |       n/a |   0.806 |   0.183 |   3.101 |
| tt.10a_regex_ends_with_var |         n/a |       n/a |   1.065 |   0.237 |   3.933 |
| tt.11_substr               |       2.880 |     2.830 |   0.724 |   0.155 |   0.237 |
| tt.12_update_fields        |       1.982 |     1.842 |   0.849 |   0.751 |   0.929 |
| tt.13_array_ops            |       0.902 |     0.831 |   1.120 |   0.997 |   1.604 |
| tt.13a_array_printf        |       0.913 |     1.101 |   1.471 |   0.525 |   1.319 |
| tt.14_function_call        |       0.129 |     0.121 |   0.055 |   0.017 |   0.026 |
| tt.15_format_lines         |         n/a |       n/a |   1.094 |   0.492 |   0.743 |
| tt.16_count_words          |       1.260 |     1.163 |   0.661 |   0.364 |   0.458 |
| tt.big_complex_program     |         n/a |       n/a |   1.946 |   1.031 |   1.907 |
| tt.x1_mandelbrot           |       0.741 |     0.620 |   0.527 |   0.288 |   0.285 |
| tt.x2_sum_loop             |       0.400 |     0.288 |   0.546 |   0.231 |   0.317 |

`n/a` は arawk が未実装 (regex 系 6 件; Phase 2 = astrogre 統合で解禁予定)。

## gawk 正規化 (>1 が速い)

| Test                | arawk-plain | arawk-aot | gawk | mawk | goawk |
|---------------------|------------:|----------:|-----:|-----:|------:|
| tt.01_print         |        0.18 |      0.17 | 1.00 | 1.97 |  1.79 |
| tt.02_print_NR_NF   |        0.58 |      0.52 | 1.00 | 1.37 |  1.31 |
| tt.02a_print_length |        0.53 |      0.54 | 1.00 | 1.39 |  1.17 |
| tt.03_sum_length    |        0.19 |      0.19 | 1.00 | 0.66 |  0.51 |
| tt.03a_sum_field    |        0.19 |      0.18 | 1.00 | 0.62 |  0.50 |
| tt.04_printf_fields |        0.58 |      0.63 | 1.00 | 1.67 |  0.69 |
| tt.05_concat_fields |        0.60 |      0.63 | 1.00 | 1.36 |  0.89 |
| tt.06_count_lengths |        0.28 |      0.26 | 1.00 | 0.89 |  0.70 |
| tt.07_even_fields   |        0.76 |      0.57 | 1.00 | 1.15 |  0.76 |
| tt.08_even_lengths  |        0.23 |      0.26 | 1.00 | 4.20 |  2.37 |
| tt.11_substr        |        0.25 |      0.26 | 1.00 | 4.68 |  3.05 |
| tt.12_update_fields |        0.43 |      0.46 | 1.00 | 1.13 |  0.91 |
| tt.13_array_ops     |        1.24 |      1.35 | 1.00 | 1.12 |  0.70 |
| tt.13a_array_printf |        1.61 |      1.34 | 1.00 | 2.80 |  1.12 |
| tt.14_function_call |        0.43 |      0.45 | 1.00 | 3.15 |  2.08 |
| tt.16_count_words   |        0.52 |      0.57 | 1.00 | 1.82 |  1.44 |
| tt.x1_mandelbrot    |        0.71 |      0.85 | 1.00 | 1.83 |  1.85 |
| tt.x2_sum_loop      |        1.36 |      1.90 | 1.00 | 2.37 |  1.73 |
| **geomean**         |    **0.57** |  **0.58** | 1.00 | 1.92 |  1.04 |

- 最終 geomean: **arawk-plain 0.57×, arawk-aot 0.58× vs gawk**
- mawk が 1.92× で圧倒的 (bytecode VM + 内部 fastpath)
- goawk は gawk とほぼ同等 (1.04×)

## Phase 1.10-1.16 + do-while/nextfile 後の前回比

Phase 1.9 直後の前回計測 (geomean plain 0.58 / aot 0.59) から **ほぼ変化なし**:

| 変更 | 影響しそうな点 | 実測 |
|---|---|---|
| `print` / `print_to` が env から OFS/ORS を読む | 出力系全テスト微減 | tt.01 0.17→0.17 / tt.02 0.52→0.52: ノイズ範囲 |
| `arawk_to_cstr` が `ARAWK_CURRENT_CTX->env[CONVFMT]` を読む | float 出力系減 | tt.01 0.17→0.17: 変化なし (CONVFMT 読みは float 経路のみ) |
| `node_gset` に FS/NF 特殊化 branch | 全 global 代入に branch 1 個 | tt.14 0.45→0.45: 影響なし (UNLIKELY 分岐が branch predictor で当たる) |
| 新規 `printf_to` / `close` / `system` / `getline` 群 | 既存テストに影響なし | tt.* は呼ばないので変化なし |
| do-while / nextfile | tt.* に未使用 | 影響なし |

軽微な振れ:
- `tt.07_even_fields` 0.44→0.57 (aot) 改善
- `tt.13a_array_printf` 1.38→1.34 (aot) 微減 / 1.33→1.61 (plain) 改善
- `tt.16_count_words` 0.72→0.57 (aot) 悪化

±0.1× 程度の変動は計測ノイズ範囲 (`MIN_TIME=1s` でも CPU 周波数スケーリング + GC stall で揺れる)。

## arawk が gawk より速い場面 (変わらず)

| Test | arawk-aot | 理由 |
|---|---|---|
| **tt.x2_sum_loop** | **1.90×** | BEGIN だけの 10M 回 fixnum ループ。AOT bake で for 全体が specialize、`AWK_IS_FIX(a)&AWK_IS_FIX(b)` + `__builtin_add_overflow` が `lea`/`add` 数命令に畳まれる |
| **tt.13_array_ops** | **1.35×** | 配列読み書きが連続。arawk の `arawk_arr_*` (FNV-1a + 単純 chained bucket) は gawk の locale-aware hash よりオーバーヘッド少ない |
| **tt.13a_array_printf** | **1.34×** | 同上 + printf |

## 苦手な場面 (変わらず)

| Test | arawk-aot | 理由 |
|---|---|---|
| **tt.01_print** | **0.17×** | `arawk_print_value` が要素ごとに `fwrite` を呼ぶ → syscall を稼ぐ。gawk/mawk は内部 buffer で chunked write |
| **tt.03_sum_length / tt.03a_sum_field** | **0.18-0.19×** | 全行で field 取得 + 算術。`arawk_split_fields` が毎行 strnum allocate (GC_malloc_atomic × 6/行) して GC 圧力高い |
| **tt.08_even_lengths** | **0.26×** | `length` を `$0` に対して呼ぶ。arawk_length の cstr 経由が重い |
| **tt.11_substr** | **0.26×** | `substr` 毎回 fresh string allocation |
| **tt.14_function_call** | **0.45×** | 関数呼び出し毎に `c->func_set` を線形検索 (strcmp)。callcache 未実装 |

## AOT 効果 (plain → aot)

- **tt.x2_sum_loop**: 0.400s → 0.288s (**28% 短縮**) — 内側ループ全体が specialize される最強形
- **tt.x1_mandelbrot**: 0.741s → 0.620s (16% 短縮) — float 演算ループ
- **tt.13a_array_printf**: 0.913s → 1.101s (悪化) — AOT が printf の variadic 経路で specialize しにくい場合あり

I/O 系 (tt.01, tt.02) や builtin 系 (tt.11, tt.06) は AOT の効果ほぼなし。

## 計測時間

- `make test`: 1.6s (smoke 98×2 modes) + 9.6s (tt.* 18×2 modes) = **~11s**
- `make bench`: 全 24 tt.* を 5 awk 実装 × 5 runs (scaling 含む) = **数分**

## 次の perf 改善案 (未着手)

1. **`print` の chunked write**: 1 文 1 fwrite に集約 → tt.01/02 に効くはず
2. **field の lazy strnum**: 全 field を毎回 allocate せず、`$N` アクセス時に必要なものだけ → tt.03 系
3. **`substr` の copy-on-write**: heap ptr 共有 → tt.11
4. **function call cache**: callsite に `function_entry *` を inline 保持 → tt.14
5. **AOT 内 builtin inline**: `length`/`substr`/`int` 等の hot builtin を SD body に直接埋め込む
