# naruby 性能メモ

ASTro 論文評価対象として **「Plain / AOT / Profile-Guided / JIT を 1
バイナリで切り替え可能な最小の Ruby 系」** という位置づけ。絶対性能を
尖らせる方向ではなく、**4 モードの差を比較する** こと、および **CRuby
(MRI / yjit) や C (gcc) と同 source/同 work で並べる** ことに焦点。

## ⚠️ 重要: CRuby / yjit との比較はフェアではない

このドキュメントの数値表 (`ruby` / `yjit` 列) は **言語処理系として同
スペックの実装を比較したものではない**。naruby は ASTro framework の
評価のために Ruby から **大量の機能を削ぎ落とした最小サブセット** で
あり (詳細は [spec.md](spec.md) — 整数 (signed 64bit) のみ、object /
GC / 例外 / block / Float / String / 配列等すべてなし)、CRuby /
yjit が払っているコストの大半を「実装していないから払わない」という
形で回避している。**naruby/lto-c が yjit より速い** という結果は、
ほぼ自明な帰結 (= 削った機能のコスト) であって、ASTro の framework
が yjit を上回る JIT を生んでいることを意味しない。

したがって `n/lto-c` が `yjit` を 1.0-14× 上回る数値は、
「naruby JIT が yjit より優れている」ことを示すのではなく、
**「Ruby の言語機能 (tagging / type check / method dispatch / 例外
チェック / GC barrier 等) が CRuby/yjit に課しているコストの大きさ」**
を示すデータとして読むべきである。

公平性のあるベンチで naruby を評価したい場合の比較対象は:

- **同じ source / 同じ work を C で書いた `gcc-O3` 列** — これが言語
  実装としての naruby の「上限」(可能な最良コード) の参考値。
- **naruby の `plain` と `aot/pg/lto` の比 (10-25×)** — ASTro framework
  そのものの効果を見る軸。これは naruby 内部での比較なので
  「機能差」のバイアスがかからない。

> 一方で、`yjit` 列を完全に削除しないのは、Ruby を読む人に「同じ
> ソースを CRuby で動かしたらどのくらいか」のスケール感を与えたいため。
> 数値は **「Ruby の構文の式が、Ruby の意味論の重みを払うとここまで
> 重い」** という参照点として読んでほしい。

## naruby vs C: 仕様比較

`gcc-O3` 列との比較は値型を `int64_t` に揃えるなど可能な限り公平を
期しているが、**naruby は Ruby サブセットなので C にない意味論が
残っている**。残コストの正体を明らかにしておく。

### 同じもの (公平な部分)

- 値型は両方 `int64_t` 1 種類のみ (overflow は未定義)。
- GC なし、object なし、メソッドなし、例外なし、block なし、
  Float / String / Array 等もなし — 両方フラットな整数演算 + 関数呼び出し。
- 算術 `+ - * / %` は両方そのまま整数演算に下る (naruby は `Integer#+` 等
  のメソッドディスパッチを経由しない)。
- 比較も同じ。
- 制御構造は `if` / `while` / `return` のみ。

### 違い (naruby だけが払うコスト)

#### 1. **late binding (関数の動的再定義)** ★ 主要因

naruby は Ruby のセマンティクスとして `def f` をいつでも再定義できる
([spec.md](spec.md) §再定義):

```ruby
def f(x) = x + 1
p(f(10))         # => 11
def f(x) = x * 2
p(f(10))         # => 20
```

これを支えるため呼び出し側に **inline cache (callcache)** が乗る:

- 呼び出しのたびに `cc->serial == c->serial` を 1 cmp + 1 jcc でチェック
- ミス時はスローパスで関数テーブルを引き、cc を更新
- `def` 走行ごとに `c->serial++` で全 cc を一括無効化

C の `f(x)` はリンク時に固定のシンボル解決 (`addr32 call <f>`) で済み、
**実行時の検査ゼロ**。chain 系で naruby/lto-c が gcc-O3 に 1.6-3.7×
負ける主因はこの cc check の積み重ね (深度 N で N 個分)。

機構の詳細は [runtime.md §5.3](runtime.md) (callcache + serial + sp_body link) 参照。

#### 2. **dispatcher 抽象**

ASTro は AST traversal を一般化するため各 NODE が `head.dispatcher`
の関数ポインタで分岐する。AOT/PG モードでは specialize された関数
ポインタに差し替わるが、LTO で貫通 inline するために
`-Wl,-Bsymbolic` + `-fno-semantic-interposition` で「intra-.so で
interpose されない」と gcc に教える必要がある。C ではそもそも
dispatcher 抽象がないので発生しないコスト。

#### 3. **フレームのスロット表現**

naruby はローカル変数を `VALUE *fp` のスロット配列で持ち、各 NODE
dispatch に 3rd 引数で register passing。C はローカルがスタック変数
で、レジスタアロケータが直接最適化できる。

ASTro は `node_lget` / `node_lset` を 1 ロード/ストアまで縮め、LTO 後
は SROA がスロットをレジスタに昇格させる、という形で C との差を
埋めている。

### まとめ

naruby と gcc-O3 の差は概ね **(1) late binding + (2) dispatcher 抽象**
に由来し、ASTro framework の評価軸は「この 2 つを PG / LTO でどこまで
削れるか」に集約される。

実ワーク系 (gcd / collatz / early_return / prime_count / compose) で
naruby/lto-c が **gcc-O3 と ≤ 1.1× の同等水準** に達するのは、
ループ内側で関数呼び出し回数が少なく、cc check のコストが分散される
ため。逆に深い chain 系で 1.6-3.7× 負けるのは cc check が線形に
積み上がるため (= late binding を保ったまま動的再定義可能性を残した
代償)。

## 計測環境

- WSL2 + Linux 6.8、host binary は `gcc-13 -O3 -ggdb3 -march=native`。
- code_store SD/PGSD は `runtime/astro_code_store.c` の既定 CFLAGS
  `-O3 -fPIC -fno-plt -fno-semantic-interposition -march=native` で
  `make` ビルド (内側 cc は `CC` env、デフォルト `gcc-13`)。
- **`CCACHE_DISABLE=1` 全体** (1st run の compile 時間が ccache で
  artificially 速くならないように)。
- 各セルは median (cached 系は 5 回、cold 系は 3 回の中央値)。
- Ruby は CRuby 4.0.2、`RUBY_THREAD_VM_STACK_SIZE=33554432`
  (ackermann が default thread VM stack を超えるため)。
- gcc 列は `bench/<name>.c` の手書き C 版を `gcc-13 -O0..-O3` でビルド。
  値型は **`int64_t` 統一** (naruby 側の `VALUE = int64_t` と幅を揃える)。
  シンセティック chain 系 (call/chain20/chain40/chain_add/compose/
  branch_dom/deep_const/prime_count) は `__attribute__((noinline,noipa))`
  で IPA / DCE を抑制。実ワークロード系 (fib/ackermann/tak/gcd/loop/
  collatz/early_return) は再帰ないしループ自体が work なので noinline 不要。
- 全ベンチ末尾で `p result` で結果を出力 (DCE 防止)。詳細は
  [bench/*.na.rb](../bench/) と [bench/*.c](../bench/)。

## ベンチ題材 (15 種、グループ別)

| グループ | ベンチ | 内容 | 期待値 |
|---|---|---|---|
| 再帰         | **fib**          | naïve `fib(40)` | 165580141 |
| 再帰         | **ackermann**    | `ack(3, 11)` | 16381 |
| 再帰         | **tak**          | `tak(30, 22, 12)` | 13 |
| 再帰         | **gcd**          | `gcd(2³¹−1, 2³⁰−1)` を 5×10⁷ 回累積 | ~ |
| ループのみ   | **loop**         | 純 while 10⁸ 回 (関数呼び出しなし) | 10⁸ |
| chain (pass) | **call**         | 10 段尾呼出 `f0→…→f9` を 10⁸ 回累積 | 4.2×10⁹ |
| chain (pass) | **chain20**      | 20 段尾呼出を 5×10⁷ 回累積 | 2.1×10⁹ |
| chain (pass) | **chain40**      | 40 段尾呼出を 2.5×10⁷ 回累積 | 1.05×10⁹ |
| chain (算術) | **chain_add**    | 10 段で各 `n+1` を 10⁷ 回累積 | 10⁸ |
| chain (合成) | **compose**      | 3 段合成 `f(g(h(x)))` を 3×10⁷ 回 | 5.7×10⁸ |
| chain (定数) | **deep_const**   | 0-arg 定数 chain `def a=1;def b=a;…` を 10⁸ 回 | 10⁸ |
| 投機分岐     | **branch_dom**   | `if (n<10⁹) n+1 else n*2` を 5×10⁷ 回 | 2.15×10⁹ |
| 制御フロー   | **collatz**      | `collatz_steps(i)` を i=1..10⁶ で累積 | sum |
| 制御フロー   | **early_return** | `find_first_factor(n)` を n=2..2×10⁶ で累積 | sum |
| 制御フロー   | **prime_count**  | π(10⁵)=9592 を 100 回累積 | 959200 |

### `call` vs `chain*` vs `deep_const` の違い

すべて「N 段の関数チェイン」だが、性質が違う:

- **call / chain20 / chain40**: pass-through (各段 `def fk(n) = fk+1(n)`、
  最終段 `n` を返す)。引数 42 を最後まで通す。深度違いで call=10、
  chain20=20、chain40=40。
- **chain_add**: 各段が `n+1` を計算する 10 段。LTO で `+1` × 10 の
  constant folding が `mov $0xa, %ecx` (= 10) に畳み込まれるかを見る。
- **compose**: 3 段で異なる演算 `f = g − 3, g = h × 2, h = n+1`。`x=10` で
  最終値 `(10+1)×2−3 = 19`。op 種類混合の inlining テスト。
- **deep_const**: 0-arg `def a = 1; def b = a; …` の 10 段。引数なしで
  定数 1 を返す chain。完全 const-fold が期待される。

`call` は深度の違うチェイン系の 10 段版。前は accumulate なしだったが、
chain20/chain40 と同じ shape (acc += f0(42)) に統一した。

## 結果 (フルマトリクス)

15 ベンチ × 13 列、単位は秒。中央値 (cached 系は 5 回、cold 系は 3 回)。

| bench        |  ruby |  yjit | n/plain | n/aot-1st | n/aot-c | n/pg-1st | n/pg-c | n/lto-1st | n/lto-c | gcc-O0 | gcc-O1 | gcc-O2 | gcc-O3 |
|--------------|------:|------:|--------:|----------:|--------:|---------:|-------:|----------:|--------:|-------:|-------:|-------:|-------:|
| fib          |  9.12 |  1.12 |    5.36 |      5.60 |    0.99 |     4.62 |   0.57 |      4.52 |    0.56 |   0.46 |   0.40 |   0.12 |   0.14 |
| ackermann    |  6.29 |  0.73 |    7.84 |      7.64 |    1.25 |     5.66 |   0.74 |      5.86 |    0.71 |   0.53 |   0.31 |   0.05 |   0.06 |
| tak          |  1.20 |  0.20 |    1.20 |      1.29 |    0.16 |     0.68 |   0.11 |      0.75 |    0.11 |   0.06 |   0.05 |   0.04 |   0.02 |
| gcd          |  4.79 |  2.09 |    3.71 |      3.80 |    0.18 |     3.39 |   0.17 |      3.58 |    0.16 |   0.22 |   0.15 |   0.15 |   0.15 |
| loop         |  0.88 |  0.88 |    0.82 |      0.89 |    0.00 |     0.91 |   0.00 |      0.92 |    0.00 |   0.04 |   0.02 |   0.00 |   0.00 |
| call         | 18.44 |  5.34 |    9.70 |      9.94 |    2.34 |     6.25 |   1.19 |      6.78 |    0.58 |   1.09 |   1.07 |   0.31 |   0.32 |
| chain20      | 19.81 |  3.97 |    9.75 |      9.92 |    3.01 |     6.08 |   1.27 |      6.39 |    0.71 |   1.27 |   0.97 |   0.27 |   0.27 |
| chain40      | 20.80 |  3.83 |    8.89 |     14.55 |    3.91 |     7.29 |   3.01 |      7.60 |    0.92 |   2.49 |   2.24 |   0.25 |   0.25 |
| chain_add    |  2.11 |  0.65 |    1.29 |      1.52 |    0.25 |     1.13 |   0.11 |      1.23 |    0.07 |   0.11 |   0.10 |   0.03 |   0.03 |
| compose      |  2.37 |  1.26 |    1.55 |      1.38 |    0.20 |     1.14 |   0.12 |      1.22 |    0.09 |   0.10 |   0.09 |   0.09 |   0.09 |
| branch_dom   |  2.25 |  1.72 |    1.49 |      1.52 |    0.15 |     1.37 |   0.14 |      1.45 |    0.14 |   0.07 |   0.07 |   0.07 |   0.07 |
| deep_const   | 17.52 |  5.18 |    3.79 |      4.07 |    1.84 |     4.79 |   1.26 |      4.77 |    0.63 |   1.01 |   0.99 |   0.32 |   0.32 |
| collatz      |  5.38 |  0.87 |    3.67 |      3.74 |    0.16 |     3.74 |   0.15 |      3.53 |    0.15 |   0.34 |   0.15 |   0.14 |   0.14 |
| early_return |  5.77 |  0.70 |    4.25 |      4.39 |    0.28 |     4.34 |   0.29 |      4.04 |    0.28 |   0.30 |   0.28 |   0.28 |   0.28 |
| prime_count  |  9.30 |  3.46 |    6.95 |      7.05 |    0.45 |     7.17 |   0.45 |      7.44 |    0.44 |   0.49 |   0.43 |   0.43 |   0.43 |

### "cold" / "cached" の定義

**cold** = 以下を毎 run の前に実施した状態:
- `rm -rf code_store/` (naruby の SD/PGSD キャッシュ ディレクトリを完全削除、
  `c/` `o/` `all.so` `hopt_index.txt` `version` 全部消える)
- `CCACHE_DISABLE=1` (ccache の compile 結果キャッシュを無効化)
- naruby host binary 自体は事前に `make` 済 (= host binary の compile は
  測定外、SD/PGSD compile のみが新規)

→ naruby は SD/PGSD .c を生成 → make で gcc compile → all.so にリンク
→ dlopen までを毎回頭から実行する。

**cached** = `cold` 1 回走った後、`code_store/` がそのまま残っている
状態で `-b` flag (= bench mode、bake skip) で run を計測。
- `code_store/all.so` を `dlopen` して既存の SD/PGSD を load
- gcc compile / make は走らない (cs_compile が dedup でファイル既存を
  検知 → skip。さらに `-b` で build_code_store 自体スキップ)

### 列の意味

| 列 | 内容 |
|---|---|
| `ruby` | CRuby 4.0.2 で `ruby bench/$B.na.rb` |
| `yjit` | CRuby 4.0.2 で `ruby --yjit bench/$B.na.rb` |
| `n/plain` | naruby `-i` (interpreter only、SD load も bake もしない) |
| `n/aot-1st` | cold 状態で `./naruby $B.na.rb` (interpret 実行 + 終了時に AOT mode で SD bake)。**run 時間 + SD compile 時間込み** |
| `n/aot-c` | aot-1st 後の cached 状態で `./naruby -b $B.na.rb` (cache を load して実行のみ。compile なし) |
| `n/pg-1st` | cold 状態で `./naruby -p $B.na.rb` (PG mode で interpret 実行 + 終了時に SD + PGSD bake)。**run 時間 + SD/PGSD compile 時間込み** |
| `n/pg-c` | pg-1st 後の cached 状態で `./naruby -p -b $B.na.rb` |
| `n/lto-1st` | cold 状態 + LTO env (`ASTRO_EXTRA_CFLAGS=-flto`、`ASTRO_EXTRA_LDFLAGS="-Wl,-Bsymbolic -flto"`) で `./naruby -p $B.na.rb`。LTO は SD compile / link を遅くするので n/pg-1st より遅め |
| `n/lto-c` | lto-1st 後の cached 状態 (LTO baked all.so) で `./naruby -p -b` |
| `gcc-O0..-O3` | C 版を gcc-13 で `-O0`/`-O1`/`-O2`/`-O3` ビルドして実行 (host binary 自体は cold/cached の対象外、 ccache は global の `CCACHE_DISABLE=1` で無効) |

## 主要観察

### naruby/lto-c vs gcc-O3

cached run の比較 (naruby は最終形 lto-c、gcc は最高設定 -O3)。

| bench | n/lto-c | gcc-O3 | n vs gcc-O3 |
|-------|------:|------:|------------:|
| fib          | 0.56 | 0.13 | 4.3× (gcc 勝) |
| ackermann    | 0.71 | 0.06 | 11.8× (gcc) |
| tak          | 0.11 | 0.02 | 5.5× (gcc) |
| gcd          | 0.16 | 0.15 | **1.07× ≈ tie** |
| loop         | 0.00 | 0.00 | DCE で比較不能 |
| call         | 0.58 | 0.33 | 1.76× (gcc) |
| chain20      | 0.71 | 0.27 | 2.6× (gcc) |
| chain40      | 0.92 | 0.25 | 3.7× (gcc) |
| chain_add    | 0.07 | 0.03 | 2.3× (gcc) |
| compose      | 0.09 | 0.09 | **1.0× tie** |
| branch_dom   | 0.14 | 0.07 | 2.0× (gcc) |
| deep_const   | 0.63 | 0.32 | 2.0× (gcc) |
| collatz      | 0.15 | 0.14 | **1.07× ≈ tie** |
| early_return | 0.28 | 0.28 | **1.0× tie** |
| prime_count  | 0.44 | 0.43 | **1.02× ≈ tie** |

★ **gcc-O3 と同等 (≤ 1.1×)** が gcd / compose / collatz / early_return /
  prime_count の 5 種、つまり **実ワークロード系で gcc-O3 と並ぶ**。
★ chain 系 / call / branch_dom / deep_const は gcc-O3 に 1.6-3.7× 負けるが、
  これは callcache check (1 cmp + 1 jcc per inline 段) の固定オーバーヘッド。
★ 再帰 3 種 (fib/ackermann/tak) は gcc-O3 に 4-12× 負ける。recursive な
  IPA-SRA / frame elimination が gcc 側でしかできないため。

### plain → aot → pg → lto の段階差

各ベンチで cached run どうしの段階比 (倍率):

| bench | plain | →aot-c | →pg-c | →lto-c | total (plain → lto-c) |
|-------|-----:|------:|------:|------:|----------------------:|
| fib          | 5.36  | 5.4×  | 1.7× | 1.0× | **9.6×** |
| ackermann    | 7.84  | 6.3×  | 1.7× | 1.0× | **11.0×** |
| tak          | 1.20  | 7.5×  | 1.5× | 1.0× | **10.9×** |
| gcd          | 3.71  | 20.6× | 1.1× | 1.1× | **23.2×** |
| loop         | 0.82  | ∞ (DCE) | — | — | (DCE) |
| call         | 9.70  | 4.1×  | 2.0× | 2.1× | **16.7×** |
| chain20      | 9.75  | 3.2×  | 2.4× | 1.8× | **13.7×** |
| chain40      | 8.89  | 2.3×  | 1.3× | 3.3× | **9.7×** |
| chain_add    | 1.29  | 5.2×  | 2.3× | 1.6× | **18.4×** |
| compose      | 1.55  | 7.8×  | 1.7× | 1.3× | **17.2×** |
| branch_dom   | 1.49  | 9.9×  | 1.1× | 1.0× | **10.6×** |
| deep_const   | 3.79  | 2.1×  | 1.5× | 2.0× | **6.0×** |
| collatz      | 3.67  | 22.9× | 1.1× | 1.0× | **24.5×** |
| early_return | 4.25  | 15.2× | 1.0× | 1.0× | **15.2×** |
| prime_count  | 6.95  | 15.4× | 1.0× | 1.0× | **15.8×** |

★ **aot-c で 2-23×**、pg-c で更に **1-2×**、lto-c で更に **1-3×**。総じて
plain から最終形 (lto-c) で **6-25 倍速**。

### 1st run コスト (compile 込み) と cached run の差

`*-1st` は `code_store/` 空 + ccache off の状態から始まり、
インタプリタ実行 + SD/PGSD bake (= make + gcc + dlopen) を全部含む。

| bench | n/aot-1st | n/aot-c | 比 (1st/c) | n/lto-1st | n/lto-c | 比 |
|-------|------:|------:|----:|------:|------:|----:|
| fib       | 5.60 | 0.99 | 5.7×  | 4.52 | 0.56 | 8.1× |
| ackermann | 7.64 | 1.25 | 6.1×  | 5.86 | 0.71 | 8.3× |
| call      | 9.94 | 2.34 | 4.2×  | 6.78 | 0.58 | 11.7× |
| chain20   | 9.92 | 3.01 | 3.3×  | 6.39 | 0.71 | 9.0× |
| chain40   | 14.55| 3.91 | 3.7×  | 7.60 | 0.92 | 8.3× |
| collatz   | 3.74 | 0.16 | 23.4× | 3.53 | 0.15 | 23.5× |
| prime_count| 7.05| 0.45 | 15.7× | 7.44 | 0.44 | 16.9× |

aot-1st は 1 度の interpret 走行 (≈ plain に近い) + bake で 4-15 秒。bake
コストは 1〜数秒、interpret 走行が支配的。
lto-1st は LTO link が遅いので bake コストが +0.5〜2 秒上乗せ。

実用上は **2 回目以降の起動が速くなる仕組み**。1st run は不可避の cold
コスト (= compile time) を含む。

## 推奨ビルド設定

| 用途 | 設定 |
|---|---|
| 通常 (gcc-13 default) | `-Wl,-Bsymbolic -flto` |
| gcc-16 が使える | `CC=gcc-16` + 上記 (recursive で +20-50%) |
| 極限性能 (loop 中心) | `CC=gcc-16` + LTO + 2 段 PGO (gcd で 81% 追加改善) |

## 高速化の仕組み

### `n/plain` (interpreter-only)

- 名前 `"foo"` で `c->func_set` を線形検索 (`node_call`) + callcache 設定
- 各 NODE の dispatch は `head.dispatcher` の indirect call
- 関数引数は `lset` で slot に書き、`node_scope` で fp 移動
- すべてのオーバーヘッドが指数スケール (再帰時) に乗る

### `n/aot-c` (cached AOT)

- `code_store/all.so` を `dlopen` し、各 NODE の `head.dispatcher` を
  `SD_<HORG>` 関数ポインタに差し替え
- 子 NODE への dispatch は **direct call** (linker が `addr32 call`
  に解決)
- 算術ノードは inline 展開 (`node_add` の `lhs + rhs` 等)

### `n/pg-c` (Profile-Guided cached)

PG mode は `node_call` を `node_pg_call_<N>` に置き換える:

- call site が `sp_body` (= 本体 NODE) を parse 時に link
- SD は `sp_body_dispatcher = SD_<HORG(sp_body)>` を C 定数として bake
  → ベイクされた all.so 内では `addr32 call <SD_addr>` の direct call
- `cc->serial == c->serial` を 1 cmp + 1 jcc で fast path 判定
- 再定義は `node_pg_call_n_slowpath` で sentinel 化

### `n/lto-c` (PGSD chain + LTO)

ASTro framework の HOPT/PGSD 機構で、各 call site の sp_body を **HOPT
(profile-aware hash) に含めて baked**。chain 各レベルが下流の構造を
符号化したユニーク PGSD シンボルになる:

```
PGSD_<top>     ─inline→ PGSD_<HOPT(f0_body)>
                           ─inline→ PGSD_<HOPT(f1_body)>
                                       ─inline→ ...
                                                  ─inline→ literal
```

LTO 併用時 gcc/clang inliner が `static inline` を直接呼ぶ前提で
クロス TU で chain を貫通 inline できる。**結果として ループ本体全体が
1 つの関数体にまとめられる** (詳細は [runtime.md](runtime.md) §5.3.1)。

#### asm 確認

**call (10 段尾呼出、accumulate 化後)**: gcc-13 + LTO で生成された
top SD (loop body) 内 (`objdump -d code_store/all.so` 抜粋):

```asm
1f00: cmp 0x58(%rax),%rdx        ; ★ cc check #1 (top → f0)
1f04: mov 0x68(%rax),%rsi        ; sp_body 読み込み
1f0b: movq $0x2a,-0x38(%rbp)     ; ★ 42 を slot に即値ストア
1f13: jne 2000 <deopt>
1f1d: call 1a70 <SD_c59c762de…>  ; chain body へ direct call
1f22: mov %rax,%rcx              ; 結果
1f28: mov 0x8(%rbx),%rax         ; i 読み込み
1f2c: add %rcx,%r12              ; acc += result
1f34: inc %rax                   ; i++
1f41: cmp $0x5f5e0ff,%rax        ; i < 100M
1f47: jg 1fc0 <exit>
```

呼び出し先 `SD_c59c762de…` (chain f0→f9 全体):

```asm
1a90: cmp 0x58(%rsi),%rax        ; cc check #2
1aa2: cmp 0x58(%r10),%rax        ; cc check #3
1ab3: cmp 0x58(%rsi),%rax        ; cc check #4
1ac4: cmp 0x58(%r10),%rax        ; cc check #5
1ad5: cmp 0x58(%r11),%rax        ; cc check #6
1ae6: cmp 0x58(%rsi),%rax        ; cc check #7
1af7: cmp 0x58(%r11),%rax        ; cc check #8
1b00: cmp 0x58(%rsi),%rax        ; cc check #9
1b3b: mov %rcx,%rax              ; return arg = 42
```

合計 **10 個の cc 検査 (top SD に 2、chain SD に 8 inline)** + 即値 42 +
SIMD pack の accumulate。深度 10 を **2 個の SD body** に焼き込み、
本体には **indirect call が 1 つもない**。

**chain_add (10 段で各 +1)**: 各 fN が `n+1` を計算するチェイン。
Naruby の Fixnum tagging は `(n<<1)|1` で、tagged 加算 `n+1` は `add $2`
(`((n+1)<<1)|1 = (n<<1)|1 + 2`) になる。10 段は **2 つの SD body** に
分散して baked される (top SD → SD_f9b… → SD_b98…):

```asm
; SD_f9b… (chain levels 1-5): 5 個の cc check + 5 段分の +1 を畳み込み
1f10/1f21/1f32/1f43/1f51: cmp 0x58(%rN),%rax  ; cc check #1-5
1f4d: add  $0x5,%rdx              ; ★ 5 段分の +1 を tagged で畳み込み
1f6d: call 1590 <SD_b98…>         ; 残り 5 段へ
```

```asm
; SD_b98… (chain levels 6-10): 4 個の cc check + さらに +5
15ae/15bb/15cc/15d9: cmp 0x58(%rN),%rdx  ; cc check #6-9 (#10 は leaf)
15e3: add  $0x5,%rax              ; ★ 5 段分の +1 を tagged で畳み込み
160e: ret                          ; 結果 = 0 + 10 (tagged)
```

★ 10 段の `+1` が **gcc constant folding によって 2 個の `add $0x5`**
にまで畳み込まれる (cross-SD では folding できないので `+5 + +5`)。
`p f0(0)` の最終結果は tagged で `0 + 10`、unwrap 後 10。

### 再帰系で chain と同じ効果が出ない理由

`fib_body → pg_call(fib) → sp_body=fib_body` は cycle。HOPT 計算は
`is_hashing` フラグでサイクル検出して低情報量の値で打ち切るので、
`HOPT(fib_body)` は `HORG(fib_body)` と本質的に同じ識別性しか持たない。
結果として `PGSD_<HOPT>` ≈ `SD_<HORG>` で、LTO inline できる範囲も同じ。

ただし gcc-16 では plain な再帰関数でも IPA-SRA + frame elimination が
効いて、call 境界のオーバーヘッドが大きく減少する (fib 0.56→0.48、
ackermann 0.74→0.46)。これは PGSD 機構と独立した cc 側の改善。

## 主な高速化テクニック

`runtime/astro_code_store.c` のデフォルト + naruby 側の sp_body bake で
構成される PG モードの主要技 (適用順):

1. **`-Wl,-Bsymbolic`** — intra-.so の SD 間参照が GOT を経由せず
   `addr32 call <SD_addr>` の direct call に。
2. **`-fno-semantic-interposition`** (`runtime/astro_code_store.c` の
   default CFLAGS) — gcc が intra-.so 関数を interpose 不可と仮定して
   inline / 直接呼びの最適化を有効化。
3. **`callcache *cc@ref`** — call site の callcache が NODE union に
   inline 化され、`cc->serial` の load が 1 段。
4. **sp_body の direct call ベイク** — PG モードで SD が
   `SD_<HORG(sp_body)>` を C 定数としてベイク。
5. **HOPT/PGSD 投機チェイン** — sp_body を HOPT に含めることで
   chain 各レベルが固有の PGSD シンボルになり、LTO で貫通 inline 可能。
6. **`VALUE *fp` register-pass** — フレームポインタを dispatcher の第 3
   引数としてレジスタ渡し。`node_lget` / `node_lset` が `fp[idx]` 1 ロード。
7. **`--param=early-inlining-insns=100`** — gcc の early-inliner 予算を
   14 → 100 に上げる。EVAL_node_call2 / call_body の中規模関数連鎖が SD
   に inline され、SROA がフレームスロットをレジスタに昇格。

## 残課題

- **HOPT cycle break の識別性**: 単に低情報量の値で打ち切るので、再帰
  関数の `HOPT` が `HORG` と区別できない。Tarjan SCC ベースの hash で
  改善余地あり。
- **PGSD/SD 両形 bake の容量**: `code_store/all.so` のサイズが約 2 倍に
  なる。HOPT が AOT でしか使われないケースは PGC bake を skip 可能。
- **profile データの活用**: 現状は parse-time の `code_repo` last-wins を
  speculation default として使う。実 runtime 観測を反映する仕組みは
  未実装。
- **再帰関数 vs gcc -O3**: fib/ackermann/tak で 4-12× 遅れる。register
  passing / frame elimination がまだ追いついてない。

## 計測の取り方 (再現手順)

```sh
make clean
CCACHE_DISABLE=1 make

# 単一ベンチを 5 回 median で測る (cached)
B=fib
rm -rf code_store
./naruby -q -p bench/$B.na.rb >/dev/null   # warmup (build SDs + load)
for i in 1 2 3 4 5; do
    /usr/bin/time -f "%e" ./naruby -q -p -b bench/$B.na.rb >/dev/null
done

# LTO 込みで cached
rm -rf code_store
ASTRO_EXTRA_CFLAGS="-flto" ASTRO_EXTRA_LDFLAGS="-Wl,-Bsymbolic -flto" \
    ./naruby -q -p bench/$B.na.rb >/dev/null
for i in 1 2 3 4 5; do
    /usr/bin/time -f "%e" ./naruby -q -p -b bench/$B.na.rb >/dev/null
done

# 1st run (cold cache + ccache off)
rm -rf code_store
CCACHE_DISABLE=1 /usr/bin/time -f "%e" ./naruby $B.na.rb >/dev/null

# gcc 比較
gcc-13 -O3 -o /tmp/g3 bench/$B.c
/usr/bin/time -f "%e" /tmp/g3 >/dev/null

# Ruby / yjit
RUBY_THREAD_VM_STACK_SIZE=33554432 \
    /usr/bin/time -f "%e" ruby bench/$B.na.rb >/dev/null
RUBY_THREAD_VM_STACK_SIZE=33554432 \
    /usr/bin/time -f "%e" ruby --yjit bench/$B.na.rb >/dev/null

# 2 段 PGO (gcc-16 + LTO 前提)
PGO=/tmp/pgo_$B
rm -rf $PGO; mkdir -p $PGO
rm -rf code_store
ASTRO_EXTRA_CFLAGS="-flto -fprofile-generate=$PGO" \
ASTRO_EXTRA_LDFLAGS="-Wl,-Bsymbolic -flto -fprofile-generate=$PGO" \
CC=gcc-16 ./naruby -q -p bench/$B.na.rb >/dev/null
./naruby -q -p -b bench/$B.na.rb >/dev/null   # collect profile
rm -rf code_store
ASTRO_EXTRA_CFLAGS="-flto -fprofile-use=$PGO -fprofile-correction" \
ASTRO_EXTRA_LDFLAGS="-Wl,-Bsymbolic -flto -fprofile-use=$PGO -fprofile-correction" \
CC=gcc-16 ./naruby -q -p bench/$B.na.rb >/dev/null
# ここから timed loop
```

bench 数値が大きく動いたとき:

1. `make clean && CCACHE_DISABLE=1 make` で再生成漏れ確認。
2. `code_store/c/SD_*.c` `code_store/c/PGSD_*.c` を全消しして再ベイクが
   走っているか確認。
3. `code_store/hopt_index.txt` を見て期待した PGSD が baked されているか確認。
4. C 版が DCE されてないか: シンセティック chain 系は
   `__attribute__((noinline,noipa))` 必須 + accumulate 必須。
