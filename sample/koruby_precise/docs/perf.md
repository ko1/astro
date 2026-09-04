# perf.md — koruby 性能改善の記録 (成功 / 失敗)

本書は **どんな最適化を試したか** と **その結果** を一覧する。
成功例だけでなく **見送ったもの** も同じ重みで記録する (再評価のために)。

## 2026-09-04: rubyspec 追従後の microbench 再計測

sp4（Ryzen 9 8945HS、8C/16T、Linux 7.0、performance governor）で、ASTro
`0a908f2a` を gcc 15.2 / `-O2 -flto=auto` でビルドし、CRuby 4.0.6 +PRISM と
比較した。対象は `sample/rubyharness/bench` の 53 本、各モード best-of-3。
`aot+compile` は毎回 code store を消して bake 時間を含め、`aot+cached` は
同じ store の実行時間だけを測る。全セルで出力一致を確認した。

### Geomean（実時間、CRuby = 1.00、値が小さいほど高速）

| mode | relative time | CRuby 比の読み方 |
|---|---:|---|
| CRuby (no JIT) | 1.00x | 基準 |
| CRuby + YJIT | 0.49x | 2.04x faster |
| koruby interpreter | 0.74x | 1.35x faster |
| koruby AOT cold | 1.04x | CRuby とほぼ同じ（bake 込み） |
| koruby AOT warm | **0.37x** | **2.70x faster** |

warm AOT は YJIT に 32/53 本で勝ち、CRuby には 51/53 本で勝った。YJIT が勝つ
代表例は再帰・method send・ivar/object 系（`ackermann`, `fib`, `tak`, `send`,
`method_call`, `ivar`, `structacc`）。AOT が特に強いのは `while`, `bitops`,
`closures`, `rangeeach`, `intdiv` などである。

### ベンチごとの YJIT 正規化値

下表は各行の CRuby+YJIT を 1.00 とした実時間比（`aot+cached < 1` が YJIT より速い）。

| bench | CRuby | YJIT | interp | AOT cold | AOT warm |
|---|---:|---:|---:|---:|---:|
| `ackermann` | 7.27 | 1.00 | 5.71 | 3.86 | 2.07 |
| `array_access` | 2.63 | 1.00 | 2.78 | 2.11 | 1.10 |
| `ary` | 1.71 | 1.00 | 1.79 | 1.43 | 0.84 |
| `aryidx` | 1.10 | 1.00 | 0.66 | 5.74 | 0.55 |
| `bignum` | 1.18 | 1.00 | 1.00 | 2.59 | 0.91 |
| `binary_trees` | 2.51 | 1.00 | 1.95 | 3.75 | 1.50 |
| `bitops` | 3.98 | 1.00 | 2.86 | 1.14 | 0.27 |
| `block` | 1.22 | 1.00 | 0.47 | 0.74 | 0.35 |
| `block_yield_kernel` | 2.49 | 1.00 | 0.88 | 1.66 | 0.50 |
| `casewhen` | 3.18 | 1.00 | 3.35 | 2.69 | 1.09 |
| `closures` | 0.91 | 1.00 | 0.44 | 0.75 | 0.30 |
| `cmpsort` | 1.48 | 1.00 | 1.17 | 1.95 | 1.04 |
| `collatz` | 2.29 | 1.00 | 2.33 | 1.79 | 0.84 |
| `exception` | 1.05 | 1.00 | 0.88 | 2.22 | 0.61 |
| `fannkuch` | 3.75 | 1.00 | 3.00 | 5.40 | 0.72 |
| `fib` | 6.06 | 1.00 | 3.81 | 4.18 | 1.57 |
| `floatcalc` | 2.06 | 1.00 | 1.69 | 2.33 | 0.57 |
| `gc_bigobj` | 1.15 | 1.00 | 0.45 | 1.34 | 0.37 |
| `gc_wb` | 1.00 | 1.00 | 0.73 | 2.69 | 0.41 |
| `gcchurn` | 1.45 | 1.00 | 1.43 | 1.49 | 0.64 |
| `gcd` | 5.10 | 1.00 | 2.97 | 2.18 | 1.05 |
| `gen_gc` | 1.09 | 1.00 | 1.58 | 3.19 | 1.34 |
| `hash` | 1.59 | 1.00 | 1.79 | 1.75 | 1.12 |
| `hashiter` | 1.31 | 1.00 | 0.60 | 1.48 | 0.45 |
| `intdiv` | 2.50 | 1.00 | 1.83 | 1.92 | 0.37 |
| `iterators` | 1.56 | 1.00 | 0.61 | 0.71 | 0.43 |
| `ivar` | 6.11 | 1.00 | 4.59 | 2.83 | 1.50 |
| `kwargs` | 3.39 | 1.00 | 2.21 | 2.42 | 0.84 |
| `mandelbrot` | 2.34 | 1.00 | 1.97 | 2.10 | 0.61 |
| `mapreduce` | 1.49 | 1.00 | 0.51 | 0.75 | 0.38 |
| `mathfn` | 1.57 | 1.00 | 1.62 | 2.81 | 1.12 |
| `method_call` | 6.07 | 1.00 | 3.87 | 2.41 | 1.41 |
| `methodchain` | 1.50 | 1.00 | 0.63 | 0.96 | 0.55 |
| `nbody` | 2.84 | 1.00 | 2.04 | 9.57 | 1.13 |
| `nested_loop` | 6.48 | 1.00 | 7.77 | 3.51 | 0.89 |
| `nesteddata` | 1.12 | 1.00 | 1.00 | 4.07 | 0.81 |
| `object` | 2.08 | 1.00 | 1.56 | 2.46 | 1.03 |
| `poly` | 2.75 | 1.00 | 1.95 | 2.82 | 0.88 |
| `rangeeach` | 1.20 | 1.00 | 0.48 | 1.03 | 0.36 |
| `render_span_kernel` | 0.99 | 1.00 | 0.67 | 1.53 | 0.33 |
| `send` | 5.20 | 1.00 | 3.94 | 2.69 | 1.79 |
| `sieve` | 1.85 | 1.00 | 1.75 | 2.70 | 0.50 |
| `sort` | 1.12 | 1.00 | 0.67 | 1.33 | 0.47 |
| `sprintfb` | 1.10 | 1.00 | 1.15 | 1.86 | 1.04 |
| `str` | 1.32 | 1.00 | 1.34 | 1.64 | 1.11 |
| `strcmp` | 1.05 | 1.00 | 1.02 | 4.36 | 0.95 |
| `strfmt` | 1.15 | 1.00 | 1.81 | 2.33 | 1.65 |
| `strops` | 1.30 | 1.00 | 1.36 | 2.66 | 1.24 |
| `strscan` | 1.08 | 1.00 | 0.75 | 1.67 | 0.75 |
| `structacc` | 4.10 | 1.00 | 2.34 | 4.30 | 1.38 |
| `tak` | 6.32 | 1.00 | 4.37 | 3.14 | 1.74 |
| `while` | 1.02 | 1.00 | 1.26 | 0.45 | 0.14 |
| `while2` | 5.40 | 1.00 | 4.77 | 2.88 | 0.82 |

raw 出力、7反復の optcarrot、環境、再現手順は
[`koruby_precise-perf-20260904`](/home/ko1/ruby/src/trials/koruby_precise-perf-20260904/README.md)
に保存した。なお sp4 の `/usr/bin/timeout` は uutils 版で短いコマンドにも約100 ms
加算するため、microbench では campaign 内の BusyBox timeout を PATH 先頭に置いた。


### GC backend 比較（同一 microbench、AOT warm）

上の主結果は default の `GC=copy`。同じ条件で8 backendを測定し、53本すべてを **YJIT=1.00** 基準で比較する（値が小さいほど高速）。`bump` は回収なしの下限、その他はGC backend。

| bench | bump | copy | copy_gen | immix | immix_gen | mark_gen | mark_compact_gen | mark_bump_gen |
|---|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| `ackermann` | 2.09 | 2.07 | 2.07 | 2.02 | 2.08 | 2.06 | 2.08 | 1.96 |
| `array_access` | 1.41 | 1.10 | 1.07 | 1.49 | 1.54 | 1.60 | 1.11 | 1.10 |
| `ary` | 1.06 | 0.84 | 0.57 | 1.10 | 1.17 | 1.11 | 0.56 | 0.58 |
| `aryidx` | 0.56 | 0.55 | 0.56 | 0.61 | 0.56 | 0.57 | 0.54 | 0.56 |
| `bignum` | 1.54 | 0.91 | 0.91 | 0.90 | 0.93 | 0.97 | 0.96 | 0.92 |
| `binary_trees` | 1.91 | 1.50 | 1.51 | 1.46 | 1.53 | 1.86 | 1.75 | 1.49 |
| `bitops` | 0.27 | 0.27 | 0.27 | 0.28 | 0.28 | 0.27 | 0.27 | 0.27 |
| `block` | 0.34 | 0.35 | 0.35 | 0.37 | 0.34 | 0.34 | 0.35 | 0.35 |
| `block_yield_kernel` | 0.49 | 0.50 | 0.50 | 0.49 | 0.49 | 0.49 | 0.49 | 0.49 |
| `casewhen` | 1.08 | 1.09 | 1.07 | 1.09 | 1.08 | 1.08 | 1.18 | 1.05 |
| `closures` | 0.51 | 0.30 | 0.32 | 0.31 | 0.31 | 0.44 | 0.34 | 0.32 |
| `cmpsort` | 1.08 | 1.04 | 1.09 | 1.01 | 1.10 | 1.13 | 1.11 | 1.09 |
| `collatz` | 0.76 | 0.84 | 0.80 | 0.79 | 0.78 | 0.78 | 0.78 | 0.77 |
| `exception` | 0.73 | 0.61 | 0.65 | 0.59 | 0.63 | 0.71 | 0.64 | 0.63 |
| `fannkuch` | 0.72 | 0.72 | 0.74 | 0.71 | 0.73 | 0.74 | 0.73 | 0.73 |
| `fib` | 1.57 | 1.57 | 1.56 | 1.58 | 1.60 | 1.58 | 1.58 | 1.57 |
| `floatcalc` | 0.57 | 0.57 | 0.58 | 0.57 | 0.56 | 0.55 | 0.58 | 0.56 |
| `gc_bigobj` | 0.45 | 0.37 | 0.40 | 0.38 | 0.40 | 0.56 | 0.40 | 0.40 |
| `gc_wb` | 0.52 | 0.41 | 0.40 | 0.38 | 0.46 | 0.56 | 0.50 | 0.42 |
| `gcchurn` | 1.41 | 0.64 | 0.70 | 0.69 | 0.67 | 1.08 | 0.80 | 0.68 |
| `gcd` | 1.06 | 1.05 | 1.03 | 1.02 | 1.05 | 1.05 | 1.01 | 1.06 |
| `gen_gc` | 1.60 | 1.34 | 1.07 | 1.13 | 1.07 | 1.47 | 1.15 | 1.09 |
| `hash` | 1.09 | 1.12 | 1.11 | 1.10 | 1.13 | 1.14 | 1.08 | 1.19 |
| `hashiter` | 0.46 | 0.45 | 0.46 | 0.46 | 0.46 | 0.46 | 0.46 | 0.46 |
| `intdiv` | 0.39 | 0.37 | 0.40 | 0.36 | 0.36 | 0.37 | 0.37 | 0.37 |
| `iterators` | 0.43 | 0.43 | 0.43 | 0.44 | 0.43 | 0.43 | 0.43 | 0.43 |
| `ivar` | 1.61 | 1.50 | 1.62 | 1.53 | 1.60 | 1.60 | 1.61 | 1.61 |
| `kwargs` | 0.86 | 0.84 | 0.85 | 0.82 | 0.83 | 0.86 | 0.84 | 0.85 |
| `mandelbrot` | 0.61 | 0.61 | 0.61 | 0.61 | 0.61 | 0.65 | 0.61 | 0.61 |
| `mapreduce` | 0.48 | 0.38 | 0.40 | 0.48 | 0.50 | 0.50 | 0.40 | 0.40 |
| `mathfn` | 1.10 | 1.12 | 1.14 | 1.14 | 1.12 | 1.14 | 1.08 | 1.13 |
| `method_call` | 1.40 | 1.41 | 1.40 | 1.42 | 1.41 | 1.41 | 1.41 | 1.41 |
| `methodchain` | 0.71 | 0.55 | 0.56 | 0.53 | 0.56 | 0.65 | 0.58 | 0.56 |
| `nbody` | 1.15 | 1.13 | 1.18 | 1.13 | 1.18 | 1.18 | 1.15 | 1.18 |
| `nested_loop` | 0.89 | 0.89 | 0.87 | 0.88 | 0.88 | 0.88 | 0.88 | 0.87 |
| `nesteddata` | 1.57 | 0.81 | 0.84 | 0.79 | 0.83 | 1.22 | 0.95 | 0.83 |
| `object` | 1.68 | 1.03 | 1.12 | 1.10 | 1.08 | 1.41 | 1.22 | 1.12 |
| `poly` | 1.01 | 0.88 | 1.09 | 0.91 | 0.93 | 0.90 | 0.88 | 0.89 |
| `rangeeach` | 0.36 | 0.36 | 0.36 | 0.36 | 0.36 | 0.37 | 0.37 | 0.36 |
| `render_span_kernel` | 0.34 | 0.33 | 0.33 | 0.33 | 0.32 | 0.34 | 0.33 | 0.33 |
| `send` | 1.80 | 1.79 | 1.76 | 1.79 | 1.78 | 1.81 | 1.81 | 1.81 |
| `sieve` | 0.53 | 0.50 | 0.51 | 0.51 | 0.52 | 0.52 | 0.51 | 0.50 |
| `sort` | 0.54 | 0.47 | 0.49 | 0.54 | 0.55 | 0.55 | 0.50 | 0.49 |
| `sprintfb` | 1.33 | 1.04 | 1.05 | 1.03 | 1.09 | 1.26 | 1.12 | 1.09 |
| `str` | 1.64 | 1.11 | 1.08 | 1.16 | 1.17 | 1.47 | 1.20 | 1.12 |
| `strcmp` | 0.95 | 0.95 | 0.96 | 0.90 | 0.96 | 1.09 | 1.02 | 0.96 |
| `strfmt` | 2.60 | 1.65 | 1.68 | 1.70 | 1.62 | 2.24 | 1.75 | 1.67 |
| `strops` | 1.69 | 1.24 | 1.28 | 1.26 | 1.27 | 1.58 | 1.31 | 1.27 |
| `strscan` | 1.31 | 0.75 | 0.81 | 0.81 | 0.79 | 1.85 | 0.91 | 0.81 |
| `structacc` | 1.37 | 1.38 | 1.38 | 1.37 | 1.38 | 1.37 | 1.38 | 1.40 |
| `tak` | 1.71 | 1.74 | 1.71 | 1.73 | 1.72 | 1.73 | 1.72 | 1.72 |
| `while` | 0.15 | 0.14 | 0.14 | 0.12 | 0.15 | 0.14 | 0.15 | 0.14 |
| `while2` | 0.78 | 0.82 | 0.81 | 0.80 | 0.80 | 0.82 | 0.84 | 0.82 |

Geomean（CRuby=1.00）は `bump` **0.42x** / `copy` **0.37x** / `copy_gen` **0.38x** / `immix` **0.38x** / `immix_gen` **0.38x** / `mark_gen` **0.42x** / `mark_compact_gen` **0.38x** / `mark_bump_gen` **0.37x**。同じ run の YJIT=1.00 基準なので、backend 間の傾向を per-benchmark で比較できる。`mark_gen` 系は churn で遅く、`copy` / `copy_gen` / `immix` 系が概ね堅実だった。

raw 出力は campaign の `artifacts/microbench-5mode-<backend>-x3.txt` に保存した。

## 2026-06-24: block-yield に simple-block fast path (成功・block 系 ~17-23%)

profile (`structacc` aot+cached) で **`korb_block_yield` が 43% self**。全 param-binding
ケース (destructure/rest/opt/kw) を1関数で処理する monolith なので、`|p|` のような
scalar block でも ~150B の worst-case フレーム prologue/epilogue + locals 初期化の libc
`memset` 呼びを毎 yield 払っていた。`korb_block_yield` を薄い wrapper に分割: scalar
required param のみ (kw/destructure/rest/opt 無し) の圧倒的多数を小フレームで inline 処理し、
それ以外は `korb_block_yield_full` (バイト等価リネーム) に委譲。commit b599e0f6。

| bench | aot+cached 旧 [秒] | aot+cached 新 [秒] | YJIT [秒] |
|---|--:|--:|--:|
| structacc | 0.192 | **0.16** | 0.141 |
| iterators | 0.518 | **0.42** | 0.946 |
| mapreduce | 0.324 | **0.27** | 0.803 |
| rangeeach | 0.104 | **0.08** | 0.271 |
| block | 0.191 | **0.15** | 0.468 |

各列 = 総実行時間 [秒] (best-of-9, 小さいほど速い)。~17-23% 短縮。corpus 89300/0/0、
block 全 param 形 + closure escape を CRuby 一致、STRESS+PURGE clean、AOT 一致。
**作業中に pre-existing バグ発見** (nested block が中間レベル変数を closure 捕捉すると
depth-2 変数解決が壊れる; HEAD でも再現・corpus 未検出) → docs/todo.md に記録。

## 2026-06-24: format/sprintf を vm-cached memstream で高速化 (成功)

profile (`sprintfb` aot+cached) で時間の **~35% が `open_memstream` の per-call
malloc + stdio バッファ zeroing** だった (`__memset_avx2` 17.6% + `_int_malloc`
6.3% + `open_memstream` 12%)。`format()` ごとに新規 memstream を開く代わりに vm に
1本キャッシュして rewind 再利用 (再入は `fmt_busy` で検出し自前 open_memstream に
fallback)。commit aa50a61b。

| bench | aot+cached 旧 [秒] | aot+cached 新 [秒] | YJIT [秒] |
|---|--:|--:|--:|
| sprintfb | 0.612 | **0.44** (best-of-9) | 0.49 |

→ ~28% 短縮で **YJIT を上回った** (1.25× → 0.90×)。`__memset_avx2` は 17.6%→2.4%。
`strfmt` は format 不使用 (string 補間) なので不変。corpus 89300/0/0、format 差分
(フラグ/幅/精度/%x%o%b/%e%g/%c/%p/%名) + 再入 #to_s/#inspect すべて CRuby 一致、
STRESS+PURGE clean、AOT 一致。残: `%x/%o/%b` が fixnum でも GMP 経由
(`__gmpz_get_str` 3%) + 各 spec の `fprintf` 自体 (将来の小ネタ)。

## 2026-06-24: 静音マシンでの再計測 + 計測トラップ + cold-bake 最適化 (prelude→preload.so)

久々に idle なマシン (load 0.4、fib --plain 変動 ~5%) で AOT vs YJIT を再計測。
**結論: 定常実行 (aot+cached) は YJIT と競合水準で、ここの低リスク最適化は無く残差は
architectural floor。一方 cold-bake (aot+compile) は prelude を preload.so に分離して
大幅短縮した (geomean 2.15x→0.82x、下記「※ 更新」)。**

**⚠️ 計測トラップ (これで一度ニセの "6× 遅い" gap map を出した)**:
`make bench BENCHMODES=...,aot+cached` を **`aot+compile` 無しで**回すと AOT でなく
**interp** を測る。`aot+cached` の prep は `code_store/all.so` が無い時だけ bake するため、
2本目以降は 1本目の stale store を再利用 → interp fallback。
**必ず `BENCHMODES=cruby+yjit,aot+compile,aot+cached`** (aot+compile が各 bench を
wipe+bake、aot+cached が warm run を計測)。単体 sanity: `--aot-compile X && --compiled-only X`
(nested_loop=0.08s=0.5× YJIT)。生 baseline は bench-report/20260624-013411-perf-baseline.txt。

**実測テーブル (`make bench` default 5 mode, best-of-3, 秒)**: CRuby 比は出さない
(geomean は母集団で動くので判断は per-benchmark)。列は全て**実時間 [秒]、小さいほど速い**:
- **cruby** = CRuby (YJIT 無効) / **cruby+yjit** = CRuby + YJIT
- **interp** = koruby tree-walker (--plain) / **aot+compile** = koruby AOT cold (SD bake 込み +1回)
  / **aot+cached** = koruby AOT warm (bake 償却後の実行のみ)

`aot+compile` は **毎 bench `rm -rf code_store` してから空 store を `--aot-compile` で
コンパイル** (run_bench.rb の WIPE→compile、CCACHE_DISABLE=1 で ccache も無効) =
プログラム固有 SD の cold/from-scratch bake 時間込み。YJIT は runtime JIT で別 bake
段が無いので cold でも速い。`aot+cached` は逆に **store が既存なら bake せず** 実行だけ
計測 (warm)。**定常実行速度の比較は aot+cached 列**。

**※ 2026-06-24 更新 — prelude を preload.so に分離 (commit 96082b53)**: 以前は
`aot+compile` 列が軒並み ~1s 超だった。真因は **gcc が遅いのではなく、プログラムが
使うかに関係なく毎回 Enumerable prelude 全体 (SD ~73個) を焼いていた**こと (`p 1+2`
でも prelude 73 SD + 自分 1 SD)。prelude は全プログラム同一なので `preload_store/all.so`
に **1回だけ** bake → 毎回 dlopen に変更し、`aot+compile` は **プログラム固有 SD のみ**
焼くようになった。下表はこの変更後の値で、本計測前に preload を warm 済み (preload の
初回ビルド 1.0s は全 run で償却されるため列に含めない)。短い bench で大幅短縮:
ackermann 1.40→0.49 / aryidx 1.05→0.23 / while 1.07→0.25 / while2 0.99→0.19、
geomean 2.15x→0.82x。残る重い列 (nbody 2.62 / mandelbrot 0.99 / strfmt 0.91 等) は
**プログラム自身の SD 量**が多いケース。詳細は [[project_koruby_precise_preload_so]] /
`docs/code_store_quirks.md §活用1`。

| bench | cruby | cruby+yjit | interp | aot+compile | aot+cached |
|---|--:|--:|--:|--:|--:|
| ackermann | 1.173 | 0.164 | 0.858 | 0.493 | 0.283 |
| array_access | 0.924 | 0.320 | 0.807 | 0.543 | 0.278 |
| ary | 0.700 | 0.359 | 0.527 | 0.358 | 0.200 |
| aryidx | 0.061 | 0.058 | 0.016 | 0.226 | 0.010 |
| bignum | 0.164 | 0.144 | 0.115 | 0.276 | 0.104 |
| binary_trees | 0.349 | 0.152 | 0.248 | 0.434 | 0.179 |
| bitops | 1.596 | 0.383 | 0.908 | 0.355 | 0.093 |
| block | 0.575 | 0.468 | 0.256 | 0.353 | 0.191 |
| casewhen | 0.475 | 0.176 | 0.504 | 0.336 | 0.136 |
| closures | 0.746 | 0.732 | 0.284 | 0.411 | 0.184 |
| cmpsort | 0.415 | 0.282 | 0.274 | 0.395 | 0.220 |
| collatz | 0.809 | 0.347 | 0.810 | 0.541 | 0.285 |
| exception | 0.281 | 0.264 | 0.121 | 0.349 | 0.058 |
| fannkuch | 0.760 | 0.270 | 0.648 | 1.080 | 0.170 |
| fib | 0.684 | 0.113 | 0.419 | 0.389 | 0.156 |
| floatcalc | 0.590 | 0.254 | 0.394 | 0.581 | 0.240 |
| gc_bigobj | 0.557 | 0.501 | 0.237 | 0.516 | 0.183 |
| gc_wb | 0.248 | 0.253 | 0.162 | 0.566 | 0.088 |
| gcchurn | 0.601 | 0.404 | 0.428 | 0.374 | 0.181 |
| gcd | 1.218 | 0.264 | 0.804 | 0.504 | 0.272 |
| gen_gc | 0.364 | 0.342 | 0.396 | 0.803 | 0.326 |
| hash | 0.827 | 0.537 | 0.727 | 0.639 | 0.448 |
| hashiter | 0.455 | 0.363 | 0.206 | 0.394 | 0.144 |
| intdiv | 0.532 | 0.207 | 0.411 | 0.359 | 0.060 |
| iterators | 1.512 | 0.946 | 0.702 | 0.719 | 0.518 |
| ivar | 1.314 | 0.203 | 0.803 | 0.446 | 0.273 |
| kwargs | 0.559 | 0.196 | 0.373 | 0.342 | 0.134 |
| mandelbrot | 0.962 | 0.369 | 0.921 | 0.994 | 0.363 |
| mapreduce | 1.195 | 0.803 | 0.412 | 0.520 | 0.324 |
| mathfn | 0.387 | 0.231 | 0.359 | 0.657 | 0.240 |
| method_call | 1.507 | 0.245 | 0.960 | 0.449 | 0.283 |
| methodchain | 0.953 | 0.678 | 0.422 | 0.573 | 0.366 |
| nbody | 0.644 | 0.228 | 0.450 | 2.619 | 0.348 |
| nested_loop | 1.091 | 0.143 | 0.909 | 0.340 | 0.089 |
| nesteddata | 0.152 | 0.144 | 0.102 | 0.392 | 0.075 |
| object | 0.499 | 0.256 | 0.327 | 0.436 | 0.216 |
| poly | 0.592 | 0.218 | 0.372 | 0.528 | 0.154 |
| rangeeach | 0.337 | 0.271 | 0.137 | 0.274 | 0.104 |
| send | 1.141 | 0.238 | 0.783 | 0.418 | 0.255 |
| sieve | 0.437 | 0.241 | 0.348 | 0.522 | 0.110 |
| sort | 0.399 | 0.343 | 0.204 | 0.390 | 0.139 |
| sprintfb | 0.536 | 0.490 | 0.651 | 0.879 | 0.612 |
| str | 0.811 | 0.631 | 0.635 | 0.656 | 0.465 |
| strcmp | 0.155 | 0.137 | 0.101 | 0.502 | 0.088 |
| strfmt | 0.725 | 0.623 | 0.736 | 0.909 | 0.647 |
| strops | 0.499 | 0.392 | 0.271 | 0.581 | 0.235 |
| strscan | 0.569 | 0.561 | 0.191 | 0.426 | 0.196 |
| structacc | 0.601 | 0.141 | 0.319 | 0.428 | 0.192 |
| tak | 1.465 | 0.219 | 0.975 | 0.567 | 0.312 |
| while | 0.768 | 0.792 | 0.833 | 0.246 | 0.072 |
| while2 | 0.390 | 0.093 | 0.445 | 0.190 | 0.026 |
| **geomean (参考)** | 1.00x | 0.49x | 0.65x | 0.82x | 0.30x |

(geomean 行は `make bench` の出力をそのまま転記した参考値。raw:
`bench-report/20260624-062852-96082b53.txt`。)

**所感 (サブ) — profile (worst: ackermann / nbody)**: 時間はほぼ method の SD 自身 (ackermann は ack の
SD に 90% self、nbody は各 method SD に分散、boxing helper は hot に出ない=flonum 効いてる)。
call fast-path (korb_call_cached の top-level 分岐 → korb_invoke_simple 直接) と
korb_invoke_simple は既に lean (arg-only method は memset 無し、最小 frame setup)。
**残差 = tree-walk node dispatch + flonum tag/untag vs YJIT の native register call / XMM
= documented cross-call devirtualization floor** (過去 session で low-ROI/high-risk と評価、
revert 歴あり)。今回も新規最適化は見送り。

## 2026-06-21: per-frame EP cell (設計 A) — TOPLEVEL_BINDING の return 課税を解消 (解決済)

**問題**: eager TOPLEVEL_BINDING で Binding が toplevel frame 上に open env を張り
`vm->open_env_cnt >= 1` が恒常化 → 全 method/block return が `if (open_env_cnt)
korb_close_ret` で open-env list を walk (method_call の ~40%)。bench は hot loop を
`def bench` ×1000 で呼ぶので大影響 (nested_loop 6.34・ackermann 5.69・while2 4.83 等)。

**失敗 1 — lazy 化** (revert): 参照検出時だけ作成。速いが eval/const_get/defined? が
壊れる (定数が常に存在すべき)。撤回。

**解決 — per-frame EP cell (設計 A)**: open env を**各 frame の base[-1] (= 受け手スロット、
self 読み出し後は消費可)** に置く。return は `korb_frame_escaped(base)` で自分の base[-1]
だけ見る → **グローバル不読**。open_envs/open_env_cnt/register/close_envs を全廃 (env は
base[-1] の slot 走査で GC-root)。
- 全 call 形態が self/recv を base[-1] に積む (node_send は元々、node_call/kw/splat/blk/
  blkproc は self を argv[0] として積むよう変更、super は restage、eval は fb=slots+1)。
- node_eget/eset は mixed-chain (odd=live base / even=KorbEnv を level 毎に切替)。
- korb_make_proc/binding が base[-1]=E (E->prev に元の外側リンク)。
- toplevel/fiber slots は先頭スラック1セル (base[-1] OOB 回避、走査も -1 から)。

結果 (aot+cached vs-YJIT): nested_loop **6.34→0.59**、while2 4.83→0.24、ackermann
5.69→1.39、method_call 3.65→1.21、fib 1.08。korb_close_envs は method_call profile から
消滅。**TOPLEVEL_BINDING は eager のまま (dynamic access 正常)**。corpus 89295/5、
STRESS+PURGE clean (closure/binding/fiber/nested-eval)、AOT match。

教訓: base[-1] の recv 再利用は implicit-self でも **self を recv として積めば**成立
(全面規約変更は不要だった)。CRuby の "magic" 同様、base[-2] に型/フラグ/署名を載せる拡張も可。

## ベンチマーク環境

- CPU: x86_64 (AMD Ryzen 9 5900HX)
- OS: Linux 6.8 (Ubuntu 24.04)
- コンパイラ: gcc 13.3 (-O2 / -O3)
- Ruby (比較対象): CRuby 4.0.2 +PRISM (no-JIT / `--yjit`)

## 2026-05-09 tenth pass 後の性能評価

spec 改善 fix が大量に入った後の sustained 計測 (1000 frames optcarrot
+ micro bench)。 chilled string / FL_CHILLED 追加、 to_enum redispatch、
NameError ivar、 30 件の Kernel privatize 等を含む。

### optcarrot (1000 frames, headless, best of 1)

| target           | FPS    | total[s] | vs CRuby |
|------------------|-------:|---------:|---------:|
| ruby             | 37.45  | 26.23    | 1.00x    |
| ruby --yjit      | 146.12 | 7.12     | 3.90x    |
| koruby (interp)  | 42.14  | 25.66    | **1.13x**|
| koruby AOT-cached| 73.82  | 12.97    | **1.97x**|

checksum 60838 が全行一致 (= 同じ計算をしている保証)。

- **koruby interp は CRuby を 12% 上回る**。 yjit には負ける。
- **AOT-cached は CRuby の 2 倍 / yjit の 0.51 倍**。

### Micro bench (sustained ~1s scale)

| bench         | CRuby   | --yjit  | koruby (interp) | vs CRuby |
|---------------|--------:|--------:|----------------:|---------:|
| fib(35)       |  0.842s |  0.102s |  0.823s         |  1.02x   |
| ack(3, 10)    |  1.607s |  0.167s |  2.549s         |  0.63x   |
| array map+sum |  0.226s |  0.086s |  0.377s         |  0.60x   |
| hash insert+lookup | 0.522s | 0.379s | 0.416s        |  **1.25x** |
| string concat |  0.484s |  0.312s |  0.607s         |  0.80x   |

- fib は parity (call dispatch / immediate Integer 演算が良い形に乗る)。
- ack は再帰深度が深く、 frame 確保コストが効く (CRuby の VM frame は
  もっと薄い)。
- Hash は逆に koruby の方が速い — 単純な insertion-order テーブルが
  CRuby の table cookie / RB_HASH_TYPE_AR 切替よりオーバーヘッドが少ない。
- string concat は capa double + 即値 FixNum→str 変換のコストで遅い。

### 退行なし

spec 改善で性能影響を心配したのは:
1. chilled string FL_CHILLED 判定 → string mutation hot path に分岐 1 つ追加。
   実測退行なし。
2. to_enum で @__source_obj を 4 ivar set → enum_for 自体が常時 hot ではないので
   問題なし。
3. Object#initialize_copy/clone/dup の Ruby 版追加 → method_missing が
   hot 経路から外れた限り問題なし。

## 2026-05-08 Binding 完全実装後の再測定 (HEAD)

Binding object を C 実装し、 prologue で cref を常に save (`mc->def_cref`
が定義時の cref を保持するよう) した上で、 frame に bindings_head を
init / epilogue snapshot 呼出を追加。 計算量的には call あたり数ストア
程度のオーバーヘッドが発生するため fib (~30M 呼出) で計測。

| bench | ruby (no JIT) | ruby --yjit | **koruby+aot** | 2026-05-06 baseline |
|---|---:|---:|---:|---:|
| fib(36) | 1.41 | 0.22 | **0.78** | 0.732 (koruby+aot) |
| optcarrot 600f (FPS) | 44.8 | — | **85.3** | 74.0 |
| optcarrot 600f (wall) | 14.04 | — | **7.32** | 8.5 |

- fib の小幅な絶対値悪化 (0.732 → 0.78、 +6%) は誤差 + Ruby バージョン更新で
  CRuby 側も速くなっている (1.632 → 1.41) ためと判定。 vs-ruby 比は
  0.45× → 0.55× にやや劣化したが、 sustained ベンチでは改善。
- **optcarrot は逆に大幅改善** (74 → 85.3 FPS、 wall 8.5 → 7.32s)。 inline
  cache + ivar shape など別系統の改善が効いている。
- **cref save guard**: prologue で `c->cref == mc->def_cref` ならスワップ
  スキップ (toplevel fib のような cref 同一ケースでロード/ストア 4 op を
  削減)。 fib では 0.80 → 0.78 と微小改善。 防御的に残す。
- bindings_head の init / null check は計測上の影響無し (UNLIKELY 側で
  branch predictor に乗っている)。

### 教訓

- **lvar 周りの prologue 変更は call-heavy bench に効きやすい** が、
  sustained ベンチでは別の最適化 (inline cache、 ivar shape、 method
  dispatch) が勝つことが多い。 最初に sustained で測ってから micro bench で
  詰める順番。
- vs-ruby 比較は CRuby のバージョン更新で動くので、 絶対値と比率の両方を
  perf.md に残す方がよい。

## 2026-05-06 リグレッション + 互換性大改修後 (HEAD `52489bc + fix`)

CRuby tu_shim 互換性 +2,236 pass (Proc cref / post / &blk / etc) と
**`korb_check_basic_op_redef` の scope 修正** が同時期。Hash#`<` を
bootstrap.rb で定義したことで全 FIXNUM/FLONUM fast path が無効化され、
fib(36) が 1.07s → 5.5s に **5× regress** していたのを発見・修正。
Integer/Float/Numeric の basic op 再定義のみが flag を flip するように。

### `benchmark/run.rb` (n=1, best of 1)

| bench | ruby | ruby+yjit | abruby+pgc | **koruby+aot** |
|---|---:|---:|---:|---:|
| ack | 1.574 | 0.217 | 0.086 | 0.811 |
| array | 0.980 | 0.338 | 0.089 | 0.429 |
| array_access | 1.034 | 0.340 | 0.087 | 0.410 |
| array_push | 0.956 | 0.407 | 0.088 | 0.468 |
| binary_trees | 0.533 | 0.296 | 0.087 | 0.610 |
| collatz | 1.542 | 0.244 | 0.087 | **0.261** |
| dispatch | 1.875 | 0.228 | 0.091 | 0.773 |
| each | 1.691 | 0.325 | 0.091 | **0.315** |
| factorial | 0.819 | 0.545 | 0.099 | 0.996 |
| fannkuch | 0.850 | 0.291 | 0.091 | **0.263** |
| fib | 1.632 | 0.217 | 0.088 | 0.732 |
| gcd | 1.288 | 0.491 | 0.085 | 0.598 |
| hash | 0.639 | 0.393 | 0.087 | 0.405 |
| inject | 0.878 | 0.188 | 0.088 | **0.216** |
| ivar | 1.413 | 0.219 | 0.089 | 0.618 |
| mandelbrot | 1.048 | 0.404 | 0.090 | 0.716 |
| map | 0.136 | 0.087 | 0.096 | 0.140 |
| method_call | 1.709 | 0.275 | 0.099 | 0.690 |
| nbody | 0.232 | 0.110 | 0.101 | 0.232 |
| nested_loop | 1.324 | 0.171 | 0.102 | **0.263** |
| object | 0.586 | 0.232 | 0.098 | 0.614 |
| sieve | 0.841 | 0.290 | 0.102 | 0.326 |
| string | 0.514 | 0.396 | 0.104 | **0.296** |
| tak | 1.802 | 0.256 | 0.104 | 1.022 |
| times | 1.897 | 0.308 | 0.113 | 0.396 |
| while | 1.823 | 0.271 | 0.123 | **0.346** |

太字は koruby が ruby (no JIT) の 2× 以上速い ベンチ。

**観察:**
- 純 Fixnum / 整数ループ (collatz, while, nested_loop, fannkuch) で **5–6× CRuby**。
- 集合・反復 (each, inject, string) も **4× 以上**。
- Call-heavy (fib, ack, tak) は **2×** 程度 — yjit には負ける (yjit の inline cache + JIT が強い)。
- abruby+pgc は AST 段階で部分評価しているので 10–15× 速く、koruby は次の層。

### optcarrot (NES emu, 600 frames sustained)

| 構成 | FPS | wall | vs CRuby (no JIT) |
|---|---:|---:|---:|
| ruby (no JIT) | 37.9 | 17.2 s | 1.00× |
| ruby --yjit | 154.8 | 5.0 s | 4.08× |
| **koruby (interp)** | 35.9 | 16.5 s | 0.95× |
| **koruby (AOT)** | 74.0 | 8.5 s | 1.95× |

checksum 60838 全構成一致。

### 重要バグ (5× regression) の発見

`bootstrap.rb` に `Hash#<` `<=` `>` `>=` を Hash 比較用に追加した直後、
fib が 1.07s → 5.5s に **5× regress** していた。原因:
`korb_check_basic_op_redef` が Hash/Array/String 等の "basic class"
全てを redef target として認識していて、Hash#`<` の追加で
**`korb_g_basic_op_redefined` flag が立ち**、Integer の Fixnum fast path
(`FIXNUM_P(l) && FIXNUM_P(r) && !redef`) が常に slow path に落ちていた。
fix: redef target を Integer/Float/Numeric だけに絞る (Hash redef は
arithmetic dispatch に影響しない)。perf record で `korb_int_minus` /
`__gmpz_init_set_si` が 10% を占めているのを見て発見。

教訓: **fast path の guard flag は scope を最小に保つ**。一見無害な
"common method 追加" でも、guard が広いと熱い path が死ぬ。

## 2026-05-03 コンパイラ別 optcarrot bench (180 frames, best/3)

System: ruby 4.0.2 +PRISM, x86_64 Linux 6.8, koruby HEAD `04553d7`
optcarrot: `sample/abruby/benchmark/optcarrot/bin/optcarrot-bench`
checksum: 59662 (ruby と全 koruby ビルド一致 ✅)

### Baseline

| runner | fps |
|---|---:|
| ruby (no JIT) | 42.30 |
| ruby --yjit | 177.97 |

### koruby interp / AOT (compiler matrix)

`-O3 -flto=auto` baseline; AOT additionally compiles each SD_*.c with
`-O3 -fPIC -fno-plt -fno-semantic-interposition -march=native`.

| compiler | -O2 interp | -O3 interp | AOT (-O3) | AOT speedup |
|---|---:|---:|---:|---:|
| gcc-13 | 46.36 | 50.63 | **100.96** | 1.94× |
| gcc-14 | 48.00 | 50.73 | 99.12 | 1.95× |
| gcc-15 | 45.43 | 51.62 | 97.21 | 1.94× |
| gcc-16 | 46.59 | 50.90 | 99.77 | 2.05× |
| clang-17 | 48.95 | 47.27 | 89.41 | 1.92× |
| clang-18 | 47.84 | 49.90 | 90.78 | 1.88× |
| clang-19 | 49.61 | 49.62 | 88.31 | 1.84× |
| clang-20 | 50.11 | 50.66 | 92.32 | 1.85× |
| clang-21 | 49.26 | 49.35 | 90.82 | 1.92× |

### gcc-16 PGO 効果

| variant | fps |
|---|---:|
| gcc-16 -O3 interp | 50.90 |
| gcc-16 PGO interp | 52.88 |
| gcc-16 -O3 AOT | 99.77 |
| gcc-16 PGO + AOT | 100.75 |

### 観察

- **interp**: clang-20 (-O2: 50.11) と gcc-15 (-O3: 51.62) がほぼ同着
- **AOT**: gcc 系全員 ~100 fps、 clang は ~90 fps で頭打ち。
  AOT-emitted SD_*.c には @noinline / 局所 DISPATCH 構造が多く、 gcc の方が
  アグレッシブに inline + clone してくれているように見える
- AOT speedup は ~2× (interp の 50 → AOT の 100)
- PGO 追加効果は ~1 fps と限定的 — AOT 化された SD_*.c がホットパスを
  取り切るので、 PGO で main 側の dispatch を絞ってもほぼ無関係
- vs ruby (no JIT): koruby AOT は **2.4× 速い**
- vs ruby --yjit: yjit が **1.76× 速い** (koruby AOT は yjit の 0.57×)

---

## 2026-05-03 拡張スイープ: コンパイラ × フラグ ~100 通り

`tools/bench-matrix.sh` で系統的に全コンパイラ × 主要フラグを総当たり後、
top の 14 + cross-compiler 8 を `RUNS=10 FRAMES=300` で再測 (validate)。
checksum 検証付き、 `taskset -c 0`。

**ルール**: AOT all.so は `-fno-lto` 固定 (LTO は実用 PGO とぶつかる)。
koruby 本体は通常 `optflags=-O3 -flto=auto` (Makefile デフォルト)。

### 最終ランキング (gcc-15 系のフラグ・スイープ, RUNS=10 FRAMES=300)

| AOT 追加フラグ                              | best fps | median fps | size  |
|---------------------------------------------|---------:|-----------:|------:|
| `-O2 -march=native` (baseline)              |  106.32  | **105.61** | 3.5MB |
| `-O2 -march=native -Wl,-O3`                 |  106.24  |   105.17   | 3.5MB |
| `-O2 -march=native -fipa-cp-clone`          |  106.04  |   105.14   | 3.5MB |
| `-O2 -march=native -fno-tree-vectorize`     |  106.27  |   105.02   | 3.6MB |
| `-O2 -march=native -fno-stack-protector`    |  105.92  |   104.82   | 3.5MB |
| `-O2 -march=native -fuse-ld=gold`           |  105.39  |   104.79   | 3.5MB |
| `-O2 -march=native --param=max-inline-insns-auto=300` | 105.00 | 104.52 | 3.5MB |
| `-O2 -march=native -Wl,--gc-sections`       |  105.49  |   104.37   | 3.5MB |

**結論**: ベース `-O2 -march=native` がそのまま最強。 試した advanced
フラグ (`-fno-tree-vectorize`, `--param=max-inline-insns-auto=*`,
`-fipa-pta`, `-fipa-cp-clone`, `-funroll-loops`, `--param=inline-unit-growth=*`,
`-fno-stack-protector`, `-Wl,-O3`, `-Wl,--gc-sections`, `-fuse-ld=gold/lld`,
`-Wl,--icf=all`, ...) はどれもベース ±1 fps、 median ベースで **全敗**。

### Cross-compiler / -O level (gcc-15 vs 13/14/16, vs clang-21)

| compiler  | AOT flags             | best fps | median fps | size  |
|-----------|-----------------------|---------:|-----------:|------:|
| gcc-15    | -O2 -march=native     |  105.72  | **104.79** | 3.5MB |
| gcc-15    | -O2 (no -march)       |  105.01  |   104.01   | 3.4MB |
| gcc-13    | -O2 -march=native     |  104.58  |   103.73   | 3.3MB |
| gcc-14    | -O2 -march=native     |  103.80  |   102.35   | 3.4MB |
| gcc-15    | -O3 -march=native     |  102.37  |   100.48   | 4.0MB |
| clang-21  | -O3 -march=native     |   95.50  |    94.41   | 3.8MB |
| clang-21  | -O2 -march=native     |   95.56  |    93.45   | 3.8MB |
| gcc-15    | -Os -march=native     |   85.52  |    84.94   | 2.2MB |

(gcc-16 baseline は別 run で 104.31; 今回スイープでは gcc-15 とほぼ同等。)

### 重要な観察

1. **gcc-15 が最強。 gcc-14 で軽い回帰 (-2 fps median)**、 gcc-16 で持ち直すが
   gcc-15 を超えない。 gcc-13 は gcc-15 の -1 fps 程度。
2. **AOT は `-O2` が `-O3` より速い**: median で +4 fps、 size 3.5MB → 4.0MB。
   AOT-emitted SD_*.c は既に手で `@noinline` + 局所 DISPATCH で
   構造化されており、 `-O3` の追加 inline / vectorize は icache 圧を
   増やすだけ。
3. **clang は gcc から ~10 fps 離される** (`-O2`/`-O3` どちらでも)。
   AOT-emitted の cold/hot dispatch パターンに対する code-gen で
   gcc が優位 (おそらく switch-table と局所 inline のバランス)。
4. **`-march=native` の効果は ~1 fps** (gcc-15 比較で 104.01 → 104.79)。
   tight loop は SSE 程度しか触っていないので AVX も大差ない。
5. **linker option / advanced -f フラグはほぼ全敗**。 `-Wl,-O3`,
   `--gc-sections`, `-fuse-ld=gold/lld`, `-Wl,--icf=all` も baseline と
   同等以内。 `-fno-tree-vectorize` も僅差で負ける (validate run 時点では)。
6. **-Os は破壊的 (-20 fps)** — size 2.2MB と小さくなるが hot path が
   inline されず壊滅。

### 採用設定 (推奨)

```sh
# koruby 本体 (Makefile デフォルトでよい)
CC=gcc-15 optflags="-O3 -flto=auto" make

# AOT (all.so + 各 SD_*.so)
CC=gcc-15 \
CFLAGS="-O2 -march=native -fPIC -fno-plt -fno-semantic-interposition" \
LDFLAGS="-fno-lto" \
./koruby --aot-compile <prog>
```

`tools/bench-matrix.sh` は今後の compiler 探索を再現可能にするため
スクリプトを `sample/koruby/tools/` に commit 済 (cf. `bench-matrix.sh`,
`bench-summary.sh`, `bench-validate.sh`)。

### 真 PGO (koruby 本体 instrument → optcarrot 60f profile → 再リンク)

`tools/bench-pgo.sh` で `make koruby-pgo` (Makefile に既存) を回した結果、
RUNS=10 FRAMES=300:

| compiler  | koruby PGO | AOT flags          | best fps | median fps | size  |
|-----------|------------|--------------------|---------:|-----------:|------:|
| gcc-15    | -O2 + PGO  | -O2 -march=native  |  107.62  | **106.71** | 3.5MB |
| gcc-15    | -O3 + PGO  | -O2 -march=native  |  107.00  |   106.51   | 3.5MB |
| gcc-16    | -O2 + PGO  | -O2 -march=native  |  106.78  |   105.61   | 3.7MB |
| gcc-13    | -O2 + PGO  | -O2 -march=native  |  102.87  |   102.15   | 3.4MB |
| gcc-15    | -O2 + PGO  | -O3 -march=native  |  102.36  |   101.38   | 4.0MB |

**PGO 効果**: gcc-15 baseline 比で median **+1.1 fps (+1.0%)**。
小さいが安定的。 hot dispatch loop の branch hint と inline 判断が
profile-guided で改善される。 AOT 化されてもなお koruby 本体の dispatch
コードは大半触られるため効く。

PGO 下でも `-O2 koruby + -O2 -march=native AOT` が最強。 `-O3` は
ここでも regression、 gcc-13 は -4 fps の大きな差。

### AOT-level PGO (all.so 自体を profile-guided でリビルド)

`tools/bench-aot-pgo.sh` で 3-pass を実装:
1. koruby を普通にビルド
2. all.so を `-fprofile-generate=$DIR` で instrument 付きでビルド
3. optcarrot 120f 走らせて profile 収集
4. all.so の `o/*.o` を `rm` → `-fprofile-use=$DIR -fprofile-correction` で再ビルド (.so 再リンク)
5. 計測

ルール (`-fno-lto` on all.so) は維持。 結果 (RUNS=10 FRAMES=300, 複数 run の median):

| compiler  | koruby PGO | AOT flags          | best fps | median fps | size  |
|-----------|------------|--------------------|---------:|-----------:|------:|
| gcc-16    | -          | -O3 -march=native  |  113.11  | **112.04** | 3.0MB | ★
| gcc-16    | -          | -O2 -march=native  |  112.42  |   111.56   | 2.9MB |
| gcc-16    | + (-O2)    | -O2 -march=native  |  113.51  |   111.53   | 2.9MB |
| gcc-15    | -          | -O2 -march=native  |  112.05  |   110.94   | 2.7MB |
| gcc-15    | -          | -O3 -march=native  |  111.22  |   110.15   | 2.8MB |
| gcc-15    | + (-O2)    | -O2 -march=native  |  110.93  |   109.75   | 2.7MB |
| gcc-13    | -          | -O2 -march=native  |  107.27  |   105.87   | 2.7MB |

**AOT-PGO の効果は劇的**:
- 非 PGO baseline (gcc-15): median 105.61
- AOT-PGO (gcc-16):         median 112.04 → **+6.4 fps (+6.1%)**
- koruby-PGO 単独 (gcc-15): median 106.71 → +1.1 fps のみ

ホットコードの大半は AOT-emitted SD_*.c に集中しているので、 そっちを
profile-guided に並べ替える効果が圧倒的。 koruby 本体の PGO は、 既に
AOT 化された後だと効きどころが少なく、 AOT-PGO に上乗せしても
no-op に近い (median 111.53 vs 111.56)。

**追加観察**:
- AOT-PGO 下では **`-O3` が `-O2` を 0.5 fps 上回る**: 普段は -O3 の
  bloat が icache を圧迫するが、 PGO で hot block first にレイアウト
  されると -O3 の追加 inline / unroll が効くようになる。
- AOT-PGO 下で **gcc-16 が gcc-15 を 1 fps 上回る** (普段は逆)。
  PGO meta-data 読み込み + register allocation の改善が新世代 gcc で
  進んでる模様。
- size: 非 PGO 3.5MB → AOT-PGO 2.9-3.0MB。 cold path が削られて
  20% 小さくなる (副次効果として配布サイズも改善)。
- gcc-13 は AOT-PGO でも 105 fps 止まり (-6 fps 差)。 PGO 関連の
  改善が gcc-15+ で大きく入ったことが伺える。

### 最終確定 ベスト構成 (production)

```sh
# 1. koruby 本体は普通にビルド (-O3 -flto=auto, Makefile デフォルト)
CC=gcc-16 make

# 2. AOT を instrument 付きでビルド (PGO pass 1)
CC=gcc-16 \
CFLAGS="-O3 -march=native -fPIC -fno-plt -fno-semantic-interposition -fprofile-generate=$PWD/aot-pgo-data" \
LDFLAGS="-fno-lto -fprofile-generate=$PWD/aot-pgo-data" \
./koruby --aot-compile <prog>

# 3. workload を 1 度走らせて profile 収集
./koruby <prog>

# 4. all.so を profile-use で再ビルド
rm code_store/o/*.o code_store/all.so
CC=gcc-16 \
CFLAGS="-O3 -march=native -fPIC -fno-plt -fno-semantic-interposition -fprofile-use=$PWD/aot-pgo-data -fprofile-correction" \
LDFLAGS="-fno-lto" \
make -C code_store all.so

# 5. 本番実行
taskset -c 0 ./koruby <prog>   # → ~112 fps
```

実装としては `tools/bench-aot-pgo.sh` の `run_aot_pgo_config` がそのまま
雛形。 Makefile に `koruby-aot-pgo` ターゲットとして組み込めば
1 コマンドで完結 (今後 todo)。

vs CRuby 4.0 + YJIT (177.97 fps): **0.63×** (旧 0.57× → 改善)。
AOT 単独 105 → AOT+PGO 112 で **1.07× 改善**。

> **続編**: ASTro 自前 PGO (`prologue_ast_simple_static_inl` を wire up
> して call dispatch を直接 call 化) を `koruby_gen.rb` override で
> 試した。 +2-4 fps の安定 win、 gcc PGO と stack して合計 **median
> 114 fps** まで届いた。 仕切り直し中で本実装はまだ。 詳細レポート:
> [`experiments/2026-05-04_pgbake.md`](./experiments/2026-05-04_pgbake.md)

---

## 2026-05-02 現状サマリ (検証付き)

過去の数字は checksum 検証なしで取られていて、optcarrot の rendering loop
が壊れた状態 (空フレーム返す bug) のまま速い数字を記録していた。
今は `tools/bench-optcarrot.sh` が checksum 行を比較して mismatch 検出
するので、以下は **CRuby 出力と一致した状態での実測**。

### optcarrot (Lan_Master.nes, 600 frames headless)

`taskset -c 0`, koruby は `gcc -O2 -flto`, SDs は `gcc -O3 -fPIC -fno-plt -march=native`.

| 構成 | fps | checksum | vs CRuby no-JIT |
|---|---:|---:|---:|
| ruby (no JIT) | 38.0 | 60838 | 1.00× |
| **koruby (interp)** | **50.1** | 60838 | **1.32×** |
| **koruby (AOT-cached, `--aot-compile`)** | **87.4** | 60838 | **2.30×** |
| ruby --yjit | 162.7 | 60838 | 4.28× |

YJIT との差は ~1.86×。 まだ縮める余地あり。

### whileloop (`n += i; i += 1` を 100M iter)

| 構成 | wall time |
|---|---:|
| ruby (no JIT) | 1.61 s |
| ruby --yjit | 1.58 s |
| koruby (interp) | 2.02 s |
| **koruby (AOT-cached)** | **0.28 s** ← yjit の 5.7×、ruby の 5.8× |

整数しか出ない tight loop は AOT の partial-eval が効いて圧勝。
optcarrot のように method call + ivar + array indexing 中心になると
2.3× にとどまる。

### 直近の修正で効いた correctness 系 (perf 数字を歪めていたバグ)

* **masgn の attribute setter** — `@a, @b, @cpu.next_frame_clock = ...` の
  3つ目が無視されていて optcarrot の PPU が常に空フレーム返してた。
  fix 後も "速さ" は変わらず (むしろ正しく描画する分遅くなる) が、
  **以前の "248 fps" は嘘** で、実際は CRuby に勝ってもなかった。
* **基本演算 redef guard の絞り込み** — `class Integer; def gcd; end` で
  `+` の fast path まで無効化されていた。 名前が basic op か検査するよう
  修正 → whileloop 1.65s → 0.30s (5.5×)、 fib(30) 0.49s → 0.17s (2.9×)。
* **block-as-&proc slot collision (parse.c)** — `f arg, ary.map(&proc)` で
  arg が proc の param 値で上書きされていた。 `pop_frame` で
  parent.arg_index を child.max_cnt まで上げる + `block_floor` で
  rewind を抑止。 perf には直接効かないが正しく動かない code が動く。
* **proc.call の env 共有化** — `proc.call` が env を snapshot して block
  内の outer 変数書き戻しが伝搬しなかった。 直接共有に変更で正しく動く
  (パフォーマンスは ~わずかに上がる: 1 アロケーションを削減)。
* **per-iteration closure capture** — `(1..3).each { |i| procs << proc { i } }`
  で全 proc が同じ env を見ていた。 parse-time に block 体を walk して
  inner block_literal がある block にだけ `creates_proc` flag を立て、
  yield 時に fresh env を allocate + outer slot copy-back する slow path に
  切替。 普通の `each` body (proc を作らない) は fast path のまま。

---

## 過去の summary table (historical, 2026-04-30 以前)

過去のベンチ表は checksum 検証前の数字。 そのまま残す:

| 構成 | fps (median) | vs CRuby no-JIT |
|---|---:|---:|
| ruby (no JIT) | 41.0 fps | 1.00× |
| abruby (plain interp, CRuby C-ext) | 42 fps | 1.02× |
| koruby (interp, plain) | 42 fps | 1.02× |
| koruby (interp + PGO — `make koruby-pgo`) | 51 fps | 1.24× |
| abruby (--aot-compile-first, AOT only) | 71 fps | 1.73× |
| abruby (--aot + --pg-compile, AOT + PGC) | 75 fps | 1.83× |
| koruby (AOT — `make koruby-aot`) | 80 fps | 1.95× |
| koruby (AOT + PGO — `make koruby-pgo-aot`) | 110 fps | 2.68× |
| ruby --yjit / --jit | 175 fps | 4.27× |

**注**: `koruby AOT + PGO` の 110 fps は当時の rendering loop が完全に
動いてたかは怪しい。 今は AOT-cached で ~87 fps が再現可能な値。
PGO + AOT は再測定 (TODO)。

#### 26 bench の現状 (best of 3, taskset -c 0)

| 系統 | bench (vs ruby --yjit, smaller=koruby better) |
|---|---|
| **win (6)** | collatz 0.9×, each 0.83×, fannkuch 0.7×, gcd 0.8×, string 0.8×, times 0.9× |
| **tied 5%以内 (7)** | array, array_access, array_push, hash, inject, sieve, while |
| Float-heavy | mandelbrot 1.66×, nbody 1.42× ← FLONUM + 即値 fast path で改善 (元 7×/3.7×) |
| call-heavy | ack 2.4×, fib 2.3×, tak 2.5×, dispatch 2.4×, method_call 1.7×, factorial 1.9× |
| block-heavy | (each/inject/times は yield inline で勝ちまたは引き分けに) |
| その他 | ivar 1.89×, object 2.31×, binary_trees 1.79×, map 1.60× |

直近 round の主な投入:
- **inline `korb_yield`** (single-arg, single-param fast path): each 0.49→0.23s (2×)
- **`korb_object_new` ivar 事前確保**: object 0.53→0.42s (-20%)
- **leaf-pure prologue** (yield/super/block_given/const 不使用 method): fib -27%, tak -26%, method_call -28%, ack -21%
- **immediate Float (FLONUM)** + Float fast path: mandelbrot 7×→1.7× behind YJIT
- **basic-op redef guard** (correctness): `class Integer; def +; end` を尊重
- **`==` identity short-circuit + NaN!=NaN preserve**

残ったレバーは:
- **method body inlining (PGSD)**: fib/ack/tak の prologue を call 側にインライン化、~50% 削減見込み
- **polymorphic IC (3+entry)**: dispatch bench は 3-way poly で 2-entry では効かず
- **map allocator**: GC pressure (現状 Boehm GC、bump allocator にすると map/binary_trees 改善)

#### この round の big wins

render_pixel kernel を抽出して perf 解析しながら段階的に 80 → 110 fps:

| 変更 | optcarrot fps | kernel ms |
|---|---:|---:|
| baseline | 80 | 905 |
| block body を `code_repo` に登録 (parse.c) — block 内の hot loop が SD 化 | 85 | 670 |
| Array#<< fast path + `korb_ary_aref` を object.h で inline (SD 内に展開) | 87 | 250 |
| `korb_hash_aref` (FIXNUM/SYMBOL key) + `korb_class_of_class` (heap path) を object.h で inline | 98 | 195 |
| `EVAL_node_(method/func)_call(_block)` を `korb_dispatch_call_cached` 経由に — IC + prologue を SD 内 inline | **110** | 185 |

ASTro 原則: 全部 fast-path を SD の TU に持ち込んだだけ。新しい NODE 種別も
AST rewrite も入れていない。 C コンパイラに委ねられる範囲を広げただけ。

(注: cold-tail を `korb_node_*_slow` として koruby 本体に hoist
し SD 内に複製しないことで `all.so` のサイズ増を抑える。LTO は koruby
本体のみ、all.so の 388 SDs は LTO 関係外。)

#### abruby +cf にキャッチアップ済み (72 fps)

koruby AOT は abruby +cf と同等の 72 fps に到達した。実装した最適化:

1. **specialized prologues** — `mc->prologue` 機構: `prologue_cfunc` /
   `prologue_ast_simple_{0,1,2,3,N}` / `prologue_ast_general` から
   method_cache_fill 時に選択。 dispatch は **単一 indirect call** で
   方策分岐なし。
2. **inline prologues** (`prologues.h`) — `static inline always_inline` の
   prologue 本体を header 化。 各 SD `.so` が独自のコピーを持ち、
   `korb_dispatch_call_cached` の guarded direct call が、 SD の TU 内で
   prologue 本体を直接インライン展開。 cross-`.so` indirect call も
   関数呼出 frame もなくなる。
3. **inline ivar_get_ic / ivar_set_ic** (`object.h`) — fast path を
   ヘッダに移して SDs に直接インライン化。 cache hit 時は load + 比較 +
   load の 3 命令で完了。
4. **TLS 廃止** — `current_block` を `__thread` から外すだけで +3 fps。
   single-threaded 実行では `__tls_get_addr` 呼び出しは pure tax。
5. **stack overflow check 省略** — 16M slot の値スタックは
   pathological 再帰でしか溢れず、 SIGSEGV で落ちれば backtrace で
   分かる。 hot path から外して 1 比較分削減。
6. **frame trim** — `caller_node` / `fp` / `locals_cnt` フィールド省略
   (3 stores/call 削減)。

#### 段階的な改善

* **24 fps** (compare_by_identity 修正後 — レンダリング正常化直後)
* **32 fps** ←← Hash#aref が 47% / aset が 19% 食ってた。実装が full linear
  scan O(N) になっていたのを proper chained hash に直して 1.65× 高速化。
* **43 fps** ←← ivar_get が 36%。`class.ivar_names` を線形スキャンしていた
  のを、AST node に `struct ivar_cache { klass; slot; }` を `@ref` operand
  として inline cache 化して 1.6× 高速化。
* **44 fps** ←← `-flto=auto`。クロス TU の関数インライン化。
* **51 fps** ←← **PGO** (`make koruby-pgo`)。 optcarrot の 60-frame run で
  プロファイル収集 → re-build with `-fprofile-use`。 ホット分岐の予測が改善。
* **54 fps** ←← **AOT 特化** (`make koruby-aot`)。abruby と同じ per-method
  SD_*.c → all.so → dlopen 方式。 optcarrot を 30 フレーム走らせて全 entry
  を `code_store/c/SD_<hash>.c` に書き出し、`gcc -O3 -fPIC -fno-plt -march=native`
  で 388 個の SD を `code_store/all.so` に。 起動時に `koruby_cs_init` が
  dlopen し、`OPTIMIZE` で各 NODE の hash 値で `dlsym("SD_<hash>")` を引いて
  dispatcher を差し替え。
* **60 fps** ←← **specialized prologues + inline cache fast path**。
  `method_cache.prologue` フィールドを追加し、`method_cache_fill` の時点で
  `prologue_ast_simple` / `prologue_ast_general` / `prologue_cfunc` の中から
  選ぶ。 dispatch は `mc->prologue(...)` の **単一 indirect call** で内部に
  cfunc vs AST 判定なし。 さらに `EVAL_node_method_call` 内に inline cache 
  hit fast path を追加し、 cache hit 時は `korb_dispatch_call` を呼ばずに
  直接 `mc->prologue` を呼ぶ。 frame init も .caller_node / .fp / .locals_cnt
  を省いて 3 stores 削減。

#### ビルド方法

```sh
make koruby-pgo    # PGO build (51 fps)
make koruby-aot    # AOT build (54 fps) — 実行時 KORUBY_CODE_STORE で dir 指定可
```

#### YJIT との差 — 2.4× (理論的には縮められる)

YJIT 174 fps に対して koruby は 72 fps、 2.4× の差がある。
**原理的には koruby も C compiler 経由で生 x86 を吐いている** ので、
YJIT に勝てる余地はある。 残っている主な技術:

* **PGSD (Profile-Guided Specialized Dispatcher)** — 実行プロファイルから
  call site ごとの `mc->prologue` を観測し、 PGSD として **直接呼出** で
  焼き込む。 現状 inline prologue は guarded direct call で実現してるが、
  guard 自体が runtime overhead。 PGSD なら guard も消せる。
* **method body inlining** — abruby compiled でもやっていない、 YJIT 専有の
  技。 hot な caller-callee ペアで callee の body を caller の SD に直接
  インライン化する。 polymorphic ならガード付きで 2-3 件まで対応。
* **型に基づくノード rewrite** (`node_plus` → `node_fixnum_plus`) — 算術
  演算でメソッドディスパッチを完全省略。
* **FLONUM 即値化** — 現在 Float はヒープ。 即値化すればアロケーションが
  消える。
* **polymorphic IC** — `mc->klass[2]` 程度。 type-flapping call site で毎回
  miss するのが防げる。

### fib(35)

| 構成 | 時間 |
|---|---:|
| ruby (no JIT) | 1.17 s |
| **ruby --yjit** | **0.23 s** |
| koruby (interp, -O2) | 1.00 s |
| koruby (interp + AOT 特化, -O3) | 0.24 s |

### fib(40)

| 構成 | 時間 |
|---|---:|
| ruby (no JIT) | 4.5 s |
| **ruby --yjit** | **1.20 s** |
| koruby (interp + AOT 特化, -O3) | 2.69 s |

### 解釈
- 純インタプリタ単体 (-O2) で fib では **CRuby (no-JIT) より 1.17× 速い**
- AOT 特化で **5× 速い**
- optcarrot のように method call + ivar 中心のワークロードでも **CRuby (no-JIT) を 1.05× 超える**
- YJIT には負けるが (interp vs JIT なので妥当)、optcarrot で 3× 程度

## 成功した改善

### ✅ 1. ASTro AOT 特化
**効果**: fib(35) 0.55s → 0.24s (2.3× 高速化)

仕組み:
- 各 AST ノードに対し `SD_<hash>(c, n)` という関数を生成
- 子ノードへのディスパッチも特化された `SD_<child_hash>` を直接呼ぶ
- 関数は static inline 連鎖になるので、C コンパイラがツリー全体を 1 関数にインライン化できる
- 特に `node_plus(node_lvar_get, node_int_lit)` のような閉じた小さなサブツリーは数行のアセンブリに畳まれる

実装ファイル:
- `node.def` / `koruby_gen.rb` で SPECIALIZE タスク
- `./koruby --aot-compile script.rb` で `code_store/c/SD_*.c` を吐き、
  `astro_cs_build` が `code_store/all.so` をリンク
- 次回以降の `./koruby script.rb` は `astro_cs_load` で `all.so` を dlopen
  → `OPTIMIZE()` が hash → SD pointer に swap

**学び**: C コンパイラに任せられる範囲が想像より広い。`-O3` + `static inline` + ノードハッシュ共有 の3点で大きな効果。

### ✅ 2. インラインメソッドキャッシュ (`struct method_cache`)
**効果**: fib のような呼出主体ベンチで顕著 (推定 30-40% 削減)

ナイーブなディスパッチ:
```c
m = klass->method_table_lookup(name);  // hash search
m->u.ast.body->head.dispatcher(c, m->u.ast.body);  // 2-step indirect call
```

キャッシュ済みディスパッチ:
```c
if (mc->serial == method_serial && mc->klass == klass) {
    mc->dispatcher(c, mc->body);  // 1-step indirect call
}
```

`mc` は call site の NODE に **per-site インライン**で埋め込まれる。`method_serial` はメソッド再定義のたびに +1 してキャッシュ全体を無効化。

実装上のポイント:
- 当初 `mc->method->u.ast.body->head.dispatcher` を毎回辿っていたが、3 段階間接参照だった
- `mc->body` と `mc->dispatcher` を直接持たせて 1 段階に減らした
- `mc->locals_cnt`/`mc->required_params_cnt` も持たせて method 構造体への参照を完全に消した

### ✅ 3. Fixnum 高速パス (オーバフロー検出付き)
**効果**: 算術重ベンチで重要

```c
NODE_DEF
node_plus(...) {
    VALUE l = EA(c, lhs); VALUE r = EA(c, rhs);
    if (LIKELY(FIXNUM_P(l) && FIXNUM_P(r))) {
        long a = FIX2LONG(l), b = FIX2LONG(r), s;
        if (LIKELY(!__builtin_add_overflow(a, b, &s) && FIXABLE(s)))
            return INT2FIX(s);
        return korb_int_plus(l, r);  // GMP に昇格
    }
    /* method dispatch fallback */
}
```

- `__builtin_add_overflow` は x86-64 で 1 命令 (`jo` / `jno`)
- FIXABLE は SHL/SHR で範囲チェック (CRuby 互換)

### ✅ 4. CRuby 互換 VALUE 表現
**効果**: 即値判定が極めて軽い

```c
#define FIXNUM_P(v)  (((VALUE)(v)) & FIXNUM_FLAG)   // 1 命令 (test)
#define NIL_P(v)     ((v) == Qnil)                  // 1 命令 (cmp)
#define RTEST(v)     (((VALUE)(v)) & ~Qnil)         // 1 命令 (test)
```

CRuby と同じビット配置にしたので、`RTEST` は `Qnil` と `Qfalse` を同時に false 判定できる魔法のマスク (両者の AND が 0) が効く。

### ✅ 5. EVAL_ARG マクロでの状態伝搬
**効果**: setjmp/longjmp 不要 + コンパイラ最適化が効く

```c
#define EA(c, n) ({                                       \
    VALUE _v = EVAL_ARG(c, n);                            \
    if (UNLIKELY((c)->state != KORB_NORMAL)) return Qnil; \
    _v;                                                   \
})
```

- 例外を起こせない部分木 (整数演算のみなど) では C コンパイラが state チェックを **完全に DCE**
- 一方 method call を含む部分木では分岐が残る (正しい)
- branch predictor 的にも `UNLIKELY` で正常パス側にバイアスがかかる

### ✅ 6. 共有 fp によるクロージャ
**効果**: yield のオーバヘッドが極小

`each { |i| s = s + i }` のようなループでは、

- ブロック作成: `proc->env = c->fp;` だけ (コピーなし)
- yield: `c->fp` 変更なし、`fp[param_base + i] = arg` でパラメータ書き込みのみ

CRuby 比でも軽い (CRuby は CFP を作って locals 配列をリンク)。

### ✅ 7. クラスごとの ivar shape
**効果**: ivar アクセスは配列添字 1 回

クラス側に「@x が slot 0、@y が slot 1...」というテーブルを持たせ、オブジェクト側は素朴な `VALUE *ivars` 配列。**書き込み初回だけ** klass の hash table を見る (新 slot 確保のため)。読み取りは固定 slot で 1 メモリアクセス。

CRuby の object_shape はもっと洗練されているが (transition tree)、最適化観点での効果は近い。

### ✅ 8. Boehm GC (実装速度の最適化)
直接の性能改善ではないが、**実装速度が劇的に上がった**:
- mark 関数を1個も書かなくて良い
- ルート登録不要 (C スタックも自動)
- ノード / オブジェクトに mark フラグや hook を付ける必要なし

これにより koruby 全体を短期間で立ち上げられた。中長期的には世代別 GC 等への移行を検討するが、現段階では Boehm のオーバヘッド (mark cost) は許容範囲内。

## 試したが採用しなかった改善

### ❌ NaN-boxing
当初検討したが **ユーザ指示で禁止**。理由は:
- VALUE 表現を CRuby と分けると CRuby のソースを流用しにくくなる
- Float が固定 16 バイトでもメモリ局所性的に大差なし

代わりに **将来 FLONUM 即値化** (CRuby と同じ low-2-bits=0b10) を入れる予定 (`todo.md` 参照)。

### ❌ 自前 mark/sweep GC (短期)
- 実装コストが大きい (object.c の各 struct に mark 関数が必要)
- Boehm のオーバヘッドはまだ計測上の問題ではなかった
- 中長期では考える ([todo.md](./todo.md))

### ❌ 例外処理を setjmp/longjmp に変更
- C コンパイラ的に setjmp は **副作用順序の barrier** になる
- ASTro 特化されたコード (深いインライン展開された SD_xxx 関数群) との相性が悪い
- 正常パスのコストは setjmp の方が安いが、**特化された subtree の最適化を犠牲にする** ほどではない
- abruby も同じ判断 (RESULT 構造体の 2 レジスタ伝搬)

### ❌ block を escape 対応にする
yield 用に共有 fp 方式を採用した結果、**escape する Proc では env がスタックに残れない**。env を heap 化する案もあったが:
- escape しないブロックが大半 (yield ベース)
- 二段階構造 (escape したら heap 化) は実装コストが高く、まだボトルネックではない

そのため現状は escape しない前提で「速さ優先」。

## 計測 / 観察ノート

### perf stat (fib(40), AOT 特化)

```
2.62 sec real
4.73 IPC
0.02% branch miss
9.78 G branches
50.2 G instructions
```

非常に IPC が高く branch predictor も良好。**コードパスが短くて密** な状態。
これ以上短縮するには C 命令そのものを減らす方向 (PG-baked call_static) が必要。

### YJIT との差の分解 (推測)

| 項目 | YJIT | koruby (AOT) | 差 |
|---|---|---|---|
| メソッド call ディスパッチ | inline 完全展開 | mc キャッシュ + 間接呼び出し | 主因 |
| Fixnum 演算 | 直接アセンブリ | 直接 C 演算 | 同等 |
| GC | mark-sweep + compact | Boehm (mark のみ) | YJIT 有利 |
| frame setup | minimal | fp += arg_index, zero locals | YJIT 有利 |

ホットループ 1 反復あたり 5-10 ns 程度の差で、ほぼメソッド呼出オーバヘッド由来と推測。

## 今後の優先候補 (perf 観点)

詳細 → [todo.md](./todo.md) の「性能向上のための課題」セクション。

1. **PG-baked call_static**: プロファイル後に call site の dispatcher を呼出先 SD に焼き直す → YJIT 並みを狙う
2. **RESULT 構造体化**: state を 2 レジスタ伝搬で
3. **型に基づくノード rewrite** (`node_plus` → `node_fixnum_plus`)
4. **FLONUM 即値化**
5. **polymorphic IC** (mc->klass[2] 程度)

## 世代別GC ベンチ (gen_gc, 2026-06-19)

`bench/gen_gc.rb` — 長寿命の retained set (40k hash, old gen 化) + call ごとの
大量短命アロケーション (young gen)。世代別 GC が「minor は young だけ走査し
retained を copy しない」効果を出すワークロード。`KORUBY_GC_STATS=1` で計測:

| GC | collections | GC time | wall |
|----|-------------|---------|------|
| copy (非世代) | 20 full | 0.092s | 0.48s |
| copy_gen (世代別) | 26 minor / **0 major** | **0.025s (−73%)** | 0.43s |

copy_gen は **GC 時間 3.7× 削減**(retained を一度も copy しない)。出力は CRuby 一致
(60002883500)、STRESS+PURGE / AOT も green。`make GC=copy_gen` で再現。
`KORUBY_GC_STATS=1` の出力に minor/major/gc_seconds/max_pause を追加。

## 2026-07-01: O(1) symbol interning
`korb_intern` は symbol table を線形走査し、比較ごとに `strlen(sym_names[i])` を呼んでいた。
perf-record で symbol-heavy path の 61% が `__strlen_avx2`(動的 symbol `:"k#{i}"` / `.to_sym` /
method 名解決が O(n)/call を払っていた)。**open-addressing hash index(FNV-1a, slot→id+1)+ 長さ
cache(sym_lens[])** で O(1) 化(0.75 load で倍化 + 再挿入、遅延初期化で既存 symbol も index)。挙動不変。
混合 bench(fib+float+symbol+truthy)で **28.4B→7.0B instructions / ~4.2s→~0.66s(約 4x)**。
また VALUE 即値タグ再編(3c7b3f5a)で FLONUM_P が 4→2 ops、flonum の sign remap 撤廃 → float 判定が安価に。

## 2026-07-13: post-test ループを別ノードに分離（node_post_while, 成功）

`node_while` は `while cond;body;end`（前判定）と `begin;body;end while cond`（後判定）を
1つのノードで `uint32_t post` フラグ分岐していた。`post` は**パース時に確定するループ構造**
（実行時に変わらない）なので、ASTro 的にはノード種別で表現するのが素直:

- **`node_while`** = 前判定専用（`post` operand を削除、単一ループ）。
- **`node_post_while`**（新規）= 後判定専用。parser（parse.c の PM_WHILE/UNTIL）が
  `PM_LOOP_FLAGS_BEGIN_MODIFIER` を見て振り分ける。`negate`（while/until）は安いので両ノード共有。

結果、interp dispatcher が `post` を一切読まない/分岐しない単一ループになる。generator が
`ALLOC_/DISPATCH_/SPECIALIZE_/HASH_/DUMP_/REPLACE_node_post_while` を node.def から自動生成
（手を入れたのは node.def 本体2つと parse.c の2行のみ）。break/next は各ループノード本体が
`KORB_BREAK`/`KORB_NEXT` を捕まえる RESULT 方式なので追加登録は不要。

instruction count（pre-hoist baseline 比、`perf stat -e instructions`）:

| bench | baseline | node_post_while | delta |
|---|--:|--:|--:|
| while2 | 6.259e9 | 6.059e9 | **−3.2%** |
| nested_loop | 12.332e9 | 12.125e9 | **−1.7%** |
| fib（再帰・while 非依存） | 7.226e9 | 7.218e9 | ±0 |

**AOT では中立**: `post`/`negate` は SPECIALIZE_node_while が SD に整数リテラルで焼く
（`fprintf(fp,"%u",n->u.node_while.post)`）→ `always_inline` の EVAL でコンパイル時に定数畳み込み
→ 死枝 DCE。つまり AOT は元々 branch を消しており、この分離は interp 専用の効き（前段の
「本体内でループを2分割」する版より更に良い＝dispatcher から post 読みが完全消滅）。
検証: while/until/break/next/modifier/post-test すべて ruby 一致、corpus 93,399 green、
AOT fresh compile + `--compiled-only` で正しく動作、STRESS+PURGE clean。

## 2026-07-13: optcarrot / バイナリサイズ実測

optcarrot 180 フレーム（checksum 59662, CRuby と完全一致）:

| 実行系 | fps | 対 素の CRuby | 対 CRuby+YJIT |
|---|---:|---:|---:|
| koruby AOT | **73.4** | **2.40×** | 0.63× |
| koruby interp (--plain) | 34.7 | 1.14× | 0.30× |
| CRuby (no yjit) | 30.5 | 1.00× | — |
| CRuby + YJIT | 115.8 | 3.79× | 1.00× |

AOT は素の CRuby を 2.4×、interp 単体でも素の CRuby を上回る。YJIT には optcarrot
（method-call/object 支配）で負ける（memo: マイクロベンチは多くで AOT>YJIT、再帰と object で負け）。
6月ベースライン（copy backend best 60.6 fps）から **+21%**。

バイナリサイズ:
- 本体 koruby_precise: ファイル 9.15 MB だが **7.20 MB は DWARF デバッグ情報**（`-ggdb3`）。
  **strip 後 1.77 MB**（.text 1.83 MB）。うち tree-walk dispatcher（`DISPATCH_/EVAL_/HASH_node_*`）は
  **わずか 68 KB / 235 関数**、prism パーサ 167 KB。
- optcarrot 全体の AOT `code_store/all.so` = **2.9 MB**（~943 SD / 523 TU）。中間物込みで code_store 全体 ~14.5 MB。

「小さい共通インタプリタ（68 KB のコア）+ プログラム別の特殊化コード（AOT で ~2.9 MB）」という
ASTro の構図がサイズにも表れている。

## 2026-07-14: korb_invoke_simple の frame-locals zeroing を小カウント inline 化（成功）

AOT optcarrot の profile で `__memset_avx2` が 4.19%。`korb_invoke_simple` は body locals
（args を除く genuine locals）を毎 call `memset(base+argc, 0, nz*8)` で zero するが、`nz` は
runtime 値なのでコンパイラは inline できず __memset を PLT 経由で呼んでいた。多くのメソッドは
locals が少数（≤4）なので、fallthrough switch で inline store、それ以外だけ memset に落とす:

```c
switch (nz) {
    case 4: z[3]=0; case 3: z[2]=0; case 2: z[1]=0; case 1: z[0]=0; break;
    default: memset(z, 0, nz*sizeof(VALUE));
}
```

GC 中立（同じ zeroing）。deterministic instruction count:

| bench | 旧 | 新 | delta |
|---|--:|--:|--:|
| method_call | 15.159e9 | 14.911e9 | **−1.6%** |
| send | 12.116e9 | 11.965e9 | **−1.2%** |
| optcarrot AOT (total, startup込) | 25.704e9 | 25.403e9 | **−1.2%** |
| fib / nested_loop | — | — | ±0（arg-only / while に body-locals memset 無し） |

`korb_invoke_simple` は `always_inline` なので AOT SD にも波及。corpus 93,399 green、
STRESS+PURGE ALL_AUDIT_PASS、AOT fresh-compile 正常。

**fps 相当（per-frame）で測り直し**: optcarrot の fps は emulation ループの定常フレームレートで
startup（parse/code-store load/HASH）を含まない。per-frame = (instr₁₈₀ − instr₃₀)/150 で
startup を差し引くと、frame-zero + alloc-zero 合算で **119.55M → 117.74M = −1.52%**（total の
−1.26% は不変の startup 4.19B で薄まった値）。startup 自体は +0.05%(誤差) で不変。**memset 系
最適化は per-frame を確かに縮めており fps に効く**。

### 見送り: 共有 runtime `aro_gc_alloc_raw` の memset（同 profile で 4.19%）
`runtime/precise_gc/gc_copy.c` の `aro_gc_alloc_raw` も payload を毎 alloc memset で zero する
（GC-required）。同じ小カウント inline 化で ~2-4% 見込みだが、**precise_gc を共有する 3 sample
（ascheme_precise / baruby_precise / koruby_precise）全部の検証が要る cross-sample 変更**なので、
単独 push 前には手を出さない。将来、3 sample の test/STRESS を揃えて回すときにまとめてやる候補。

## 2026-07-14: AOT startup コストの分析（HASH caching = 次の最大の的、ただし要設計）

optcarrot AOT を frames で割ると、**固定 startup ≈ 4.19B instructions**（parse + code-store
load）で、180 フレームでは **全体の 16.5%**（1 frame ≈ 117.7M instr）。この startup の大きな
部分が **node の構造ハッシュ**（`HASH_node_*` が profile で合計 ~8%）。code-store load は
各 SD を parse 済み AST に「構造ハッシュ一致」で結びつけるので、**全ノードに HASH() を呼ぶ**。

`HASH()`（runtime/astro_node.c）は **caching 無効**（`has_hash_value` を誰も立てない）。理由は
コメント通り: dispatcher patching が hash 後に node の `kind` を書き換える（ascheme_precise の
`lref → lref_sp`）と、cached hash が stale 化し、無効化できない親 cache に伝播する。よって
`hash_node()` は毎回 `HASH()` に落ちて **subtree を再計算** → load で全ノードに呼ぶと
**O(n·depth)**。optcarrot の深い式木で startup を膨らませている。

**次にやるなら**: hash caching の再導入。ただし失敗モードが「SD↔node 取り違え = silent code
corruption」で怖く、共有 astro_node.c を使う 3 sample（ascheme/baruby/koruby_precise）全部に
影響する。安全にやるには (a) kind-mutation 全サイトに明示的 invalidation hook を通す、または
(b) 「全 kind 確定後（cs_load 時点）に1回だけ bottom-up で全ハッシュを memo 化し、以後 execution
は kind を変えない」ことを保証したうえで load-phase 限定 cache にする。いずれも要設計・要 3-sample
検証なので、単独 push 前の即席変更にはしない。**startup-only = optcarrot の fps を一切改善しない**（fps は emulation ループの定常フレーム
レートで startup を含まない。per-frame 実測で確認済み）。効くのは total wall-clock / cold-start /
短いスクリプトだけ。長時間ランでは償却される（実ゲームは数千フレーム）。**「fps で YJIT に迫る」
目的では的外れ**なので、その目的なら per-frame ホット（optcarrot 自身の SD / Array#rotate! の
memmove / korb_send_impl の CFUNC dispatch）を対象にすること。

安全な小手先（型名ハッシュ `hash_cstr("node_X")` の定数畳み込み）は、コンパイラが既に fold
している可能性 + recursion/hash_merge 支配で効果小、と判断し見送り。
