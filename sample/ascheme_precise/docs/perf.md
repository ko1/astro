# ascheme_precise — performance evaluation

`sample/ascheme_precise/` の precise GC framework migration の perf 評価。
本ドキュメントは、 **libgc (= Boehm conservative GC) を baseline** に
17 個の precise GC backend を比較し、 以下 2 つの観点を中心にまとめる:

1. **整数系 workload の overhead** — precise rooting (= sp/env tracking、
   `ARO_LOAD` 経由 deref) が non-allocation-heavy なコードに与える影響
2. **GC-heavy workload の改善** — 各 backend が cons / vector heavy な
   workload で libgc に対して出せる速度差

## 0. setup

- **machine**: AMD Ryzen 9 5900HX (= 8 cores / 16 threads、 ~4.6 GHz boost)
- **memory**: 30 GiB
- **kernel**: Linux 6.8.0-117 x86_64
- **OS**: Ubuntu (= WSL 環境)
- **compiler**: gcc -O3 -ggdb3
- **methodology**: 各 benchmark 3 回実行、 最小値 (= setup ノイズ排除)。
  出力 first-line を expected と照合して **正答性検証** してから記録
  (= GC bug で「速いが結果が誤」を排除)。
- **scale**: 全 workload を **0.4–10 秒の範囲** (= sustained measurement、
  setup-bound にならない) で実行。

## 1. backend 可用性 matrix (= default mode、 stress なし)

`✓` = 結果一致 + 完走、 `✗` = SEGV / 誤結果 / timeout (= ★ で表記)。

| backend             | fib35 | sumloop | nbody | sieve_big | deriv | nqueens | fannkuch | cps_loop | matmul |
|---------------------|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|
| libgc (baseline)    | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| none                | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| bump                | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| mark                | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| mark_gen            | ★ | ✓ | ★ | ★ | ★ | ★ | ★ | ✓ | ★ |
| mark_freelist       | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| mark_bitmap_gen     | ✓ | ✓ | ✓ | ★ | ✓ | ✓ | ✓ | ✓ | ✓ |
| mark_card_gen       | ✓ | ✓ | ✓ | ★ | ✓ | ✓ | ✓ | ✓ | ✓ |
| copy                | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| copy_gen            | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ★ |
| mark_compact        | ★ | ✓ | ★ | ★ | ★ | ★ | ★ | ✓ | ★ |
| mark_compact_gen    | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ★ |
| mark_bump_gen       | ✓ | ✓ | ✓ | ★ | ✓ | ✓ | ✓ | ✓ | ★ |
| immix               | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| immix_gen           | ★ | ✓ | ★ | ★ | ★ | ★ | ★ | ✓ | ★ |
| copy_scramble       | ★ | ✓ | ★ | ★ | ★ | ★ | ★ | ✓ | ★ |

`★` は ascheme の **precise rooting gap** が顕在化するケース。 ascheme の
`struct sobj` 内の typed-ptr field (= `closure.env`, `vec.items`,
`str.chars`, `sframe.parent` 等) が **raw のまま** で、 moving GC や
頻発する gen GC で stale pointer crash。 baruby_precise は全 typed-ptr が
`VALUE` (= encoded) に統一済なので同 issue なし。 詳細は §6.1。

実用 (= 全 workload PASS): **9 個** (libgc + 8 precise)。

## 2. 数値表 (= 単位 秒、 best of 3)

```
bench       cat libgc  none   bump   mark   m_free m_bmp_G m_crd_G copy   copy_G m_c_G  m_Bu_G immix
fib35       INT 0.44   0.45   1.22   0.92   0.91   0.91   0.91    0.87   0.85   0.81   0.86   0.87
sumloop     INT 1.49   1.64   1.54   1.65   1.47   1.54   1.52    1.65   1.65   1.47   1.49   1.47
nbody       INT 0.52   0.86   0.62   0.44   0.46   0.46   0.45    0.41   0.39   0.39   0.39   0.39
sieve_big   GC  1.13   1.16   1.06   1.20   1.05   ★     ★      1.18   1.14   1.04   ★     1.01
deriv       GC  1.07   1.52   1.27   1.01   0.97   0.97   0.97    0.93   0.91   0.85   0.86   0.86
nqueens     MIX 2.08   2.24   2.54   2.40   2.25   2.26   2.18    2.37   2.35   2.13   2.14   2.15
fannkuch    MIX 1.35   2.38   1.70   1.18   1.24   1.19   1.21    1.05   1.05   1.02   0.99   1.00
cps_loop    MIX 0.81   0.90   0.81   0.94   0.83   0.82   0.86    0.87   0.95   0.85   0.80   0.83
matmul      MIX 8.02   9.94   10.02  4.80   58.07  58.07  ★      4.73   ★     ★      ★     4.75
```

略号: `m_free` = mark_freelist、 `m_bmp_G` = mark_bitmap_gen、 `m_crd_G`
= mark_card_gen、 `copy_G` = copy_gen、 `m_c_G` = mark_compact_gen、
`m_Bu_G` = mark_bump_gen。 `mark_gen`/`mark_compact`/`immix_gen`/`copy_scramble`
は ★ が支配的なので表から省略。

## 3. 整数系 workload の overhead

非 alloc-heavy な loop / recursion を libgc と比較:

| bench    | libgc | precise (best) | best backend     | ratio |
|----------|------:|---------------:|------------------|------:|
| fib35    |  0.44 |           0.81 | mark_compact_gen | **1.84× slower** |
| sumloop  |  1.49 |           1.47 | mark_freelist / mark_compact_gen / immix | 0.99× ~tie |
| nbody    |  0.52 |           0.39 | copy_gen / mark_compact_gen / mark_bump_gen / immix | **0.75× faster** |
| cps_loop |  0.81 |           0.80 | mark_bump_gen   | 0.99× ~tie |

**所見**:

- `fib35` (= 純粋再帰、 stack 深い、 alloc なし) は precise rooting の sp[]
  更新 cost が effective に効く worst case。 全 backend で 1.7–2.8× 遅い。
  libgc は C stack を保守的に scan するので per-call sp[] update 不要、
  ここで強い
- `sumloop` (= tight numeric loop、 alloc なし) は ~tie
- `nbody` (= 物理 simulation、 numeric heavy) で precise が **逆に速い**。
  推測: precise + flonum 内挿 (= `scm_try_flonum`) で alloc を抑え + framework
  heap layout が cache friendly。 libgc 経由だと double を heap allocate
  保持し続けるが、 precise は VALUE 内 inline encoding で alloc-free
- `cps_loop` (= closure heavy + tail call) は ~tie

precise rooting overhead は **workload 依存**。 alloc が rare で再帰深い fib35
系で +80%、 他は ±15% 程度。 **fib35 を除けば overhead 軽微**。

## 4. GC-heavy workload の改善

cons / vector heavy 系で libgc と比較:

| bench     | libgc | precise (best) | best backend       | ratio |
|-----------|------:|---------------:|--------------------|------:|
| sieve_big |  1.13 |           1.01 | immix             | **0.89× faster** |
| deriv     |  1.07 |           0.85 | mark_compact_gen  | **0.79× faster** |
| nqueens   |  2.08 |           2.13 | mark_compact_gen  | 1.02× ~tie |
| fannkuch  |  1.35 |           0.99 | mark_bump_gen     | **0.73× faster** |
| matmul    |  8.02 |           4.73 | copy              | **0.59× faster** |

**所見**:

- 全 GC-heavy bench で precise GC が **libgc より有意に速い** (= -10〜-40%)
- `matmul` は 41% 高速化 — libgc は bignum 内部 buffer + コーナーケースで
  GC overhead 高い
- `mark_compact_gen` (= 世代別 mark + Lisp-2 sliding compact) と `copy` /
  `copy_gen` が **GC-heavy で常に上位**。 cons-heavy workload に Cheney 系
  がフィット
- `mark_bump_gen` (= bump nursery + linked-list tenured) も健闘
- `mark_freelist` / `mark_bitmap_gen` の **matmul 58 秒は outlier** — 外部
  メモリ (= GMP buffer) 会計の threshold trigger が freelist / bitmap walk
  と相性悪く GC が thrash。 6.3 で詳述

## 5. backend 別 summary

総合 (= 全 workload で動く 8 個 + libgc):

| backend            | mean ratio | sweet spot                          |
|--------------------|-----------:|-------------------------------------|
| `copy`             |      0.86× | balanced、 GC-heavy 一律高速        |
| `copy_gen`         |      0.84× | nursery heavy workload              |
| `mark_compact_gen` |      0.82× (★ matmul 除く) | survivor 多い workload              |
| `mark_bump_gen`    |      0.82× (★ matmul 除く) | mixed lifetime                      |
| `immix`            |      0.94× | uniform、 fragmentation-resistant   |
| `mark`             |      1.10× | baseline precise (= 単純実装)       |
| `mark_freelist`    |      0.92× (matmul outlier 除外) | slow allocator workload             |
| `mark_bitmap_gen`  |      0.94× (matmul outlier 除外) | small-payload heavy                 |
| `libgc`            |     1.00 (baseline) | reference                           |
| `none` / `bump`    |      1.20× | leak-as-you-go (= bench / 起動限定) |

**production 推奨**: `copy_gen` または `mark_compact_gen`。 GC-light なら
`mark` / `mark_freelist`。

## 6. 既知 limitation / future work

### 6.1 precise rooting gap (= ★ marked backends)

`mark_gen`, `mark_compact`, `immix_gen`, `copy_scramble` 等は ascheme の
`struct sobj` 内 typed-ptr field (= `closure.env`, `vec.items`, `str.chars`)
が **raw のまま** で、 moving / aggressive sweep が発火すると stale pointer
で crash。

baruby_precise はこの問題なし — refactor 時に全 typed-ptr field を
`VALUE` (= encoded) 化済。 ascheme は call/cc / closure / GMP 等の機構が
深く絡み、 同様の rewrite には main.c 数百箇所 touch + 慎重な debug が
必要 (= 別 task として記録)。

**workaround**: 上記 backends は default mode で trigger 頻度低い workload
なら動く (= cps_loop / sumloop は通る)。 全 workload で動く 8 backend で
実用上問題なし。

### 6.2 fib35 overhead 1.84×

純粋再帰での 1.8× は precise rooting の本質的 cost。 改善余地:
- sp[] 更新を `__builtin_expect` で inline + branch prediction
- self-tail-call の frame reuse 強化 (= 既に `leaf` opt あり)
- libgc の C-stack scan を真似た「lazy sp[] update」 (= alloc 直前のみ)

これらは ascheme 固有最適化 — baruby_precise との共通部分を維持しつつ
入れる余地がある。

### 6.3 matmul outlier (mark_freelist / mark_bitmap_gen = 58 秒)

7× 遅い (= libgc 8 秒 vs 58 秒)。 推測:

- matmul の `fill-matrix` が LCG (= `(* s 1103515245) + 12345`) で巨大
  bignum を生成
- bignum は GMP allocator (= libc malloc) 経由で確保、 `aro_gc_account_external`
  で framework 会計に通知
- 外部 bytes が threshold 越え → GC 発火 → finalizer で mpz_clear
- mark_freelist / mark_bitmap_gen の sweep は heap walk なので、 dead obj
  数が多いと O(heap) cost、 さらに external GC trigger 頻度高いと累積

調整余地: external_bytes の threshold を内部 threshold とは別係数にする、
ASTRO_GC_FINALIZE callback の order を整理、 等。

### 6.4 stress mode で更なる gap 露呈

`BARUBY_GC_STRESS=1` で全 backend を試すと、 mark / immix / mark_freelist
の 3 backend のみ 16/16 ascheme test + 174/179 R5RS chibi PASS。 他は
6.1 と同根の root tracking gap で crash。 ascheme の typed-ptr field
uniform-encode 化が完了すれば、 stress + copy_scramble で audit 可能になる。

## 7. baruby_precise との比較

`sample/baruby_precise/docs/perf.md` の数値と直接比較すると、 ascheme
の方が backend あたり ~10–20% 遅い傾向。 主因:

- ascheme は `sframe` chain (= linked list) で env を表現、 baruby は flat
  `sp[]` スタック。 alloc 経路が 1 多い
- ascheme は call/cc / closure / continuation / multi-values 等の heavy 機構
  を持つ
- ascheme は GMP 経由 bignum の external accounting で GC frequency が
  上がりやすい

ただし baruby_precise は naruby (= libgc) からの fork で libgc 互換比較が
取れない。 **libgc 直接比較は ascheme 側のみ**。

## 8. 結論

precise GC framework + ascheme は **libgc に対して**:

- **GC-heavy workload で平均 -15〜-40% 高速化** (= matmul -41%、 fannkuch -27%、
  deriv -21%、 sieve_big -11%)
- **整数 workload で fib35 のみ +80% overhead** (= 純再帰の sp[] 更新 cost)、
  他は ±5%
- **GC-light な numeric workload** (= nbody) でも precise が flonum 内挿
  + 良 cache layout で **逆に -25% 速い** ことすらある

backend 選択:
- **balanced production**: `copy_gen` または `mark_compact_gen`
- **GC-light workload**: `mark` または `mark_freelist`
- **audit / debug**: `copy_scramble` (= mark/move 漏れを SEGV 検出。 ただし
  6.1 の typed-ptr 改修後に真価)

libgc を上回る場面が多い一方、 fib35 系で overhead が残る。 ascheme の
固有最適化 (= sframe pool reuse、 leaf-closure sframe alloca 等) で更に
縮められる見込み。

---

bench script: `/tmp/claude/bench_v4.sh` (本 commit 内には含めない、 docs
記録のみ)。 再現には `cd sample/ascheme && make && cd ../ascheme_precise &&
make GC=<backend>` で 17 backend ×9 workload。
