# abc 性能メモ

計測: `make bench` (= `ruby benchmark/run_bench.rb`)。各ワークロードを ~1 秒
スケールで `bc` (GNU bc 1.07.1) / `abc` インタプリタ / `abc` AOT の 3 通りで走らせ、
**出力一致を確認してから** best-of-3 の wall time を取る。

## 代表値 (1 コア, gcc -O2, GNU bc 1.07.1)

```
benchmark           bc(s)   abc-int(s)   abc-aot(s)     int/bc  aot/int
----------------------------------------------------------------------------
int_sum             0.669        0.324        0.345      0.48x    1.06x
fib_rec             0.695        0.208        0.216      0.30x    1.04x
factorial           8.373        0.198        0.205      0.02x    1.04x
div_scale           0.683        0.164        0.172      0.24x    1.05x
modpow_loop         0.420        0.095        0.109      0.23x    1.15x
sqrt_loop           5.342        0.216        0.215      0.04x    1.00x
pi_leibniz          0.941        0.245        0.247      0.26x    1.01x
collatz             1.692        0.377        0.396      0.22x    1.05x
----------------------------------------------------------------------------
geomean                                                  0.16x    1.05x
```

`int/bc < 1.0` = abc インタプリタが bc より速い。`aot/int < 1.0` = AOT が効いている。
**全ベンチで bc より速く、geomean で ~6 倍速い。**

## fixnum (即値小整数)

初期実装は「あらゆる値を `bcnum`(GMP + GC)」だったため、小整数ループで演算ごとに
確保が走り bc の ~2 倍遅かった。そこで **LSB タグの即値**を導入:

- `VALUE` (= `bcnum *`) の LSB=1 を **scale 0・62bit 整数の即値**として使う
  (値 = `(intptr_t)v >> 1`)。scale>0 や範囲外だけがヒープ `bcnum`。
- `+ - * / % == < …` は両辺が即値なら **native long 演算**(`__builtin_*_overflow`
  で桁あふれ検出)。あふれ/小数が絡むときだけ GMP にフォールバック。
- 即値はポインタを含まないので GC はスキャン対象の奇数ワードを単に無視する。

これで小整数稠密ループの確保がゼロになり、効果は劇的:

| bench | 導入前 int/bc | 導入後 int/bc |
|---|---|---|
| int_sum | 2.65x (遅い) | **0.48x** |
| fib_rec | 2.09x | **0.30x** |
| modpow_loop | 1.84x | **0.23x** |
| collatz | 2.05x | **0.22x** |

あわせて、`bcnum` は immutable なので **`bc_rescale` は scale 一致時にコピーせず
引数を共有**、関数呼び出しの引数も値コピーをやめて共有にした (確保削減)。

## 解釈

数値は計測の裏付けがある事実に限って書く (cf. ルート `docs/perf.md` 方針)。

### 1. 多倍長が効く処理は依然 bc を一桁以上引き離す

**factorial (~42x)**, **sqrt_loop (~25x)** は abc の値が GMP `mpz_t` なので、
大きな乗算 (Karatsuba/FFT) や整数平方根 (`mpz_sqrt`) で GMP のアルゴリズム的優位が
そのまま出る。

### 2. 小整数ループも bc より速くなった

fixnum 導入で int_sum / fib / modpow / collatz が ~2x 遅い → ~2–5x 速いに反転。
ホットパスが native long 演算 + ツリーウォークになり、bc の破壊的桁配列処理より速い。

### 3. AOT 特殊化はほぼ横ばい (geomean 1.05x)

- ASTro の部分評価は **dispatch チェーンの除去**で効くが、abc の算術は
  `bc_add` / `bc_mul` 等の **out-of-line helper 呼び出し**として残る (SD に inline
  されない)。即値化でノード本体は軽くなったが、helper 呼び出しが支配的なままなので
  SD 化しても削れる時間が小さい (±5%、計測誤差レベル)。
- **取り分を出すには** fixnum fast-path を node.def 内に直接展開する (helper を
  static-inline 化 / statement-expr macro 化して EVAL_ARG 経由で SD に取り込む) 必要が
  ある。cf. [[feedback_eval_arg_vs_eval]] / pystro の inline 知見。`todo.md` 参照。

## さらに速くするなら (未着手)

- **算術 helper の inline 化**: 上記。AOT で初めて dispatch 除去 + 即値演算の
  inline が効く見込み。
- **bcnum プール / アリーナ**: 大きな scale の中間値の確保削減。
