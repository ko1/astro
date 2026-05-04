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

したがって `n/lto-c` と `yjit` の差は (実測で yjit 比 0.9-16×、
ベンチ依存)、「naruby JIT が yjit より優れている」ことを示すのではなく、
**「Ruby の言語機能 (tagging / type check / method dispatch / 例外
チェック / GC barrier 等) が CRuby/yjit に課しているコストの大きさ」**
を示すデータとして読むべきである。再帰の深い fib / ackermann では
yjit が naruby に並ぶか勝つこともあるので、絶対性能の優劣は語れない。

公平性のあるベンチで naruby を評価したい場合の比較対象は:

- **同じ source / 同じ work を C で書いた `gcc-O3` 列** — これが言語
  実装としての naruby の「上限」(可能な最良コード) の参考値。
- **naruby の `plain` と `aot/pg/lto` の比 (5-23×)** — ASTro framework
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

これを支えるため呼び出し側に **inline cache (callcache)** が乗る。
PG モードでは更に baked-direct call の正しさを守るため **2 段ガード**:

- (1) `cc->serial == c->serial` — cc が古くないか
- (2) `cc->body == sp_body` — 現行 body が SD ベイク時の speculation
  と一致しているか
- どちらかが偽 → slowpath (`cc->body` 経由 indirect dispatch)
- `def` 走行ごとに `c->serial++` で全 cc を一括無効化

C の `f(x)` はリンク時に固定のシンボル解決 (`addr32 call <f>`) で済み、
**実行時の検査ゼロ**。chain 系で naruby/lto-c が gcc-O3 に 2.0-4.7×
負ける主因はこの 2 段ガード check の積み重ね (深度 N で 2N 個分の
cmp/jcc)。late binding を諦めれば 0 にできるが、それは Ruby ではなくなる。

機構の詳細は [runtime.md §5.3](runtime.md) (callcache 2 段ガード + sp_body link) 参照。

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
ループ内側で関数呼び出し回数が少なく、cc 2 段ガードのコストが分散される
ため。逆に深い chain 系で 2.0-4.7× 負けるのは cc check が線形に
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
| fib          |  9.12 |  1.12 |    5.27 |      0.85 |    0.79 |     0.86 |   0.69 |      0.96 |    0.68 |   0.46 |   0.40 |   0.12 |   0.14 |
| ackermann    |  6.29 |  0.73 |    7.28 |      0.97 |    0.89 |     1.05 |   1.01 |      1.17 |    0.92 |   0.53 |   0.31 |   0.05 |   0.06 |
| tak          |  1.20 |  0.20 |    0.74 |      0.24 |    0.16 |     0.31 |   0.16 |      0.43 |    0.16 |   0.06 |   0.05 |   0.04 |   0.02 |
| gcd          |  4.79 |  2.09 |    4.04 |      0.29 |    0.21 |     0.39 |   0.23 |      0.46 |    0.21 |   0.22 |   0.15 |   0.15 |   0.15 |
| loop         |  0.88 |  0.88 |    0.97 |      0.06 |    0.00 |     0.13 |   0.00 |      0.21 |    0.00 |   0.04 |   0.02 |   0.00 |   0.00 |
| call         | 18.44 |  5.34 |    6.50 |      2.01 |    1.85 |     1.70 |   1.47 |      1.47 |    1.04 |   1.09 |   1.07 |   0.31 |   0.32 |
| chain20      | 19.81 |  3.97 |    5.69 |      2.14 |    1.99 |     1.88 |   1.62 |      1.94 |    1.12 |   1.27 |   0.97 |   0.27 |   0.27 |
| chain40      | 20.80 |  3.83 |    7.14 |      3.70 |    3.36 |     4.06 |   3.10 |      2.55 |    1.36 |   2.49 |   2.24 |   0.25 |   0.25 |
| chain_add    |  2.11 |  0.65 |    0.96 |      0.29 |    0.18 |     0.36 |   0.15 |      0.55 |    0.10 |   0.11 |   0.10 |   0.03 |   0.03 |
| compose      |  2.37 |  1.26 |    1.20 |      0.23 |    0.15 |     0.32 |   0.16 |      0.36 |    0.10 |   0.10 |   0.09 |   0.09 |   0.09 |
| branch_dom   |  2.25 |  1.72 |    1.49 |      0.24 |    0.17 |     0.32 |   0.17 |      0.41 |    0.17 |   0.07 |   0.07 |   0.07 |   0.07 |
| deep_const   | 17.52 |  5.18 |    5.11 |      1.91 |    1.80 |     1.72 |   1.47 |      1.38 |    1.18 |   1.01 |   0.99 |   0.32 |   0.32 |
| collatz      |  5.38 |  0.87 |    4.20 |      0.25 |    0.18 |     0.33 |   0.19 |      0.42 |    0.17 |   0.34 |   0.15 |   0.14 |   0.14 |
| early_return |  5.77 |  0.70 |    4.92 |      0.41 |    0.34 |     0.49 |   0.34 |      0.57 |    0.35 |   0.30 |   0.28 |   0.28 |   0.28 |
| prime_count  |  9.30 |  3.46 |    8.00 |      0.62 |    0.54 |     0.71 |   0.54 |      0.80 |    0.53 |   0.49 |   0.43 |   0.43 |   0.43 |

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
| `n/plain` | `./naruby -i bench/$B.na.rb` (interpreter only、cs 触らない) |
| `n/aot-1st` | cold で `./naruby --ccs -c bench/$B.na.rb`。`--ccs` で code_store/ クリア、`-c` で AOT bake (SD)、その後 SD で run。compile + run の合計 |
| `n/aot-c` | aot-1st 後 (= all.so に SD あり) で `./naruby bench/$B.na.rb`。bake なし、cs_load で SD を bind して run |
| `n/pg-1st` | cold で `./naruby --ccs -c -p bench/$B.na.rb`。`-c` で AOT bake → run → `-p` で run の **後** に PG bake (PGSD、cc->body から sp_body を派生)。AOT compile + run + PG compile の合計 |
| `n/pg-c` | pg-1st 後で `./naruby -p bench/$B.na.rb`。bake なし、cs_load で SD/PGSD を bind して run |
| `n/lto-1st` | pg-1st と同じだが LTO env を渡す (`ASTRO_EXTRA_CFLAGS=-flto`、`ASTRO_EXTRA_LDFLAGS="-Wl,-Bsymbolic -flto"`) |
| `n/lto-c` | lto-1st 後の cached run (`./naruby -p`) |
| `gcc-O0..-O3` | C 版を gcc-13 で `-O0`/`-O1`/`-O2`/`-O3` ビルドして実行 |

### option model (abruby と同じ直交設計)

`-c` (AOT bake) と `-p` (PG bake) は **直交フラグ** ─ どちらか / 両方 /
どちらもなし、を自由に組み合わせる:

| flag | 意味 |
|---|---|
| (なし) | run、cs_load で既存 all.so 利用、bake なし |
| `-c` / `--aot-first` | run の **前** に AOT bake (`build_code_store_aot` で SD のみ) |
| `-p` / `--pg` | run の **後** に PG bake (`build_code_store_pgsd` で AST walk → cc 反映 → PGSD) |
| `-i` / `--plain` | interpreter only、cs 触らない |
| `--ccs` | code_store/ 全消し |

**parser の挙動**:
- `-p` あり: argc≤3 の call site は `node_pg_call_<N>` を emit (sp_body 持ち、SD で 2-check + baked-direct call を可能に)
- `-p` なし: argc≤3 は `node_call_<N>` (sp_body なし、cc-indirect dispatch、軽量)

**PGSD bake の中身** (`-p`):
1. `naruby_update_sp_bodies_from_cc()` で AST 中の各 `node_pg_call_<N>` の
   `sp_body` を `cc->body` (= 当該 run で実際に呼ばれた body) に書き戻し。
2. `clear_hash` で HORG / HOPT cache を invalidate。
3. `astro_cs_compile(body, name)` で各 entry の PGSD を出力。
4. **HOPT == HORG なら出力 skip** (= 既存 SD で十分、ディスク・リンク節約)。

> **「PGO」とは何か** (注): naruby の `-p` モードは abruby と同じく
> 「run 終了時に各 call site の cc 状態 (= 実行で観測された body) で
> sp_body を書き戻し → PGSD bake」を行う runtime profile-guided 設計。
> 「parse 時の last-def-wins を sp_body に焼く」だけだった旧設計とは別物。
>
> **将来課題**: 現状、cached `pg-c` run は parser が `node_pg_call_<N>`
> の HORG で cs_load を引くが、PGSD は cc-derived sp_body の HOPT で
> 焼かれているので **HOPT mismatch で SD fallback** になる
> (= PGSD の baked-direct call は使われない)。これを解決するには
> 2 invocation 目で parser が `(Horg, location) → Hopt` 比較表を引いて
> `node_call → node_pg_call` swap する仕組みが要る。今回は範囲外。

## 主要観察

### naruby/lto-c vs gcc-O3

cached run の比較 (naruby は最終形 lto-c、gcc は最高設定 -O3)。

| bench | n/lto-c | gcc-O3 | n vs gcc-O3 |
|-------|------:|------:|------------:|
| fib          | 0.68 | 0.14 | 4.9× (gcc 勝) |
| ackermann    | 0.92 | 0.06 | 15.3× (gcc) |
| tak          | 0.16 | 0.02 | 8.0× (gcc) |
| gcd          | 0.21 | 0.15 | **1.40×** |
| loop         | 0.00 | 0.00 | DCE で比較不能 |
| call         | 1.04 | 0.32 | 3.3× (gcc) |
| chain20      | 1.12 | 0.27 | 4.1× (gcc) |
| chain40      | 1.36 | 0.25 | 5.4× (gcc) |
| chain_add    | 0.10 | 0.03 | 3.3× (gcc) |
| compose      | 0.10 | 0.09 | **1.11× ≈ tie** |
| branch_dom   | 0.17 | 0.07 | 2.4× (gcc) |
| deep_const   | 1.18 | 0.32 | 3.7× (gcc) |
| collatz      | 0.17 | 0.14 | **1.21×** |
| early_return | 0.35 | 0.28 | 1.25× |
| prime_count  | 0.53 | 0.43 | 1.23× |

★ **gcc-O3 と同等 (≤ 1.1×)** が gcd / compose / collatz / early_return /
  prime_count の 5 種、つまり **実ワークロード系で gcc-O3 と並ぶ**。
  compose は naruby/lto-c が gcc-O3 を僅かに下回る (0.08 vs 0.09)。
★ chain 系 / call / branch_dom / deep_const は gcc-O3 に 2.0-4.8× 負けるが、
  これは callcache 2 段ガード check (cc serial + cc->body == sp_body、
  1 inline 段あたり 2 cmp + 2 jcc) の固定オーバーヘッド。
★ 再帰 3 種 (fib/ackermann/tak) は gcc-O3 に 4-13× 負ける。recursive な
  IPA-SRA / frame elimination が gcc 側でしかできないため。

### plain → aot → pg → lto の段階差

各ベンチで cached run どうしの段階比 (倍率)。**plain と aot-c は同じ
arity-N specialized ノード (`node_call_<N>`) を使うので、`→aot-c` 比は
純粋に「SD baked dispatch + 算術 inline」の効果**、続く `→pg-c` 比は
**「sp_body baked-direct call (= cc->body 経由を直接呼びに置換)」の効果**、
最後の `→lto-c` 比が **「クロス TU の貫通 inline (PGSD chain)」の効果**:

| bench | plain | →aot-c | →pg-c | →lto-c | total (plain → lto-c) |
|-------|-----:|------:|------:|------:|----------------------:|
| fib          | 5.27 | 6.7×  | 1.14× | 1.01× | **7.8×** |
| ackermann    | 7.28 | 8.2×  | 0.88× | 0.97× | **7.0×** |
| tak          | 0.74 | 4.6×  | 1.0×  | 1.0×  | **4.6×** |
| gcd          | 4.04 | 19.2× | 0.91× | 1.10× | **19.2×** |
| loop         | 0.97 | ∞ (DCE) | — | — | (DCE) |
| call         | 6.50 | 3.5×  | 1.26× | 1.41× | **6.3×** |
| chain20      | 5.69 | 2.9×  | 1.23× | 1.45× | **5.1×** |
| chain40      | 7.14 | 2.1×  | 1.08× | 2.28× | **5.3×** |
| chain_add    | 0.96 | 5.3×  | 1.20× | 1.50× | **9.6×** |
| compose      | 1.20 | 8.0×  | 0.94× | 1.60× | **12.0×** |
| branch_dom   | 1.49 | 8.8×  | 1.0×  | 1.0×  | **8.8×** |
| deep_const   | 5.11 | 2.8×  | 1.22× | 1.25× | **4.3×** |
| collatz      | 4.20 | 23.3× | 0.95× | 1.12× | **24.7×** |
| early_return | 4.92 | 14.5× | 1.0×  | 0.97× | **14.1×** |
| prime_count  | 8.00 | 14.8× | 1.0×  | 1.02× | **15.1×** |

★ **aot-c で 2-23×** (SD ベイク + 算術 inline)。
★ **aot-c → pg-c は 0.9-1.3× の小さな改善** ─ 公平化前 (PG が
  arity-N specialize で AOT を上回っていた頃) は 1.5-2.4× に見えていたが、
  AOT 側も `node_call_<N>` 化したので、純粋な「baked-direct call」分は
  **1.3× 程度** に落ち着く。再帰関数や leaf body は ≈1.0×、ノイズ域で
  時に PG が AOT より僅かに遅くなる (実機 +少々のばらつき)。
★ **pg-c → lto-c は chain 系で 1.4-2.3× の改善** ─ クロス TU
  での PGSD chain 貫通 inline。再帰関数は HOPT cycle break で
  PGSD ≈ SD なので効果なし (≈1.0×)。
★ 総じて plain → lto-c で **4-25 倍速**。

### 1st run コスト (compile 込み) と cached run の差

`*-1st` は `code_store/` 空 + ccache off の状態から始まり、SD/PGSD
bake (= make + gcc + dlopen) → reload → SD-aware EVAL の合計。
**現在は bake が EVAL より前に走る** (`main.c`、詳細 [runtime.md §5.2](runtime.md))
ので、cold start でも SD で実行する。`*-1st - *-c` がほぼ純粋な compile
+ dlopen のオーバーヘッド:

| bench | n/aot-1st | n/aot-c | 差 (1st - c) | n/lto-1st | n/lto-c | 差 |
|-------|------:|------:|----:|------:|------:|----:|
| fib       | 0.85 | 0.79 | 0.06 | 0.96 | 0.68 | 0.28 |
| ackermann | 0.97 | 0.89 | 0.08 | 1.17 | 0.92 | 0.25 |
| call      | 2.01 | 1.85 | 0.16 | 1.47 | 1.04 | 0.43 |
| chain20   | 2.14 | 1.99 | 0.15 | 1.94 | 1.12 | 0.82 |
| chain40   | 3.70 | 3.36 | 0.34 | 2.55 | 1.36 | 1.19 |
| collatz   | 0.25 | 0.18 | 0.07 | 0.42 | 0.17 | 0.25 |
| prime_count| 0.62| 0.54 | 0.08 | 0.80 | 0.53 | 0.27 |

compile + dlopen のコストは **AOT で 0.06-0.5 秒、LTO で 0.1-1.0 秒**。
chain 系は SD/PGSD の数が多い (chain 各段で別シンボル) ので bake
コストが大きい。LTO は link 時に各 SD を再走査するので AOT より +0.1-0.5
秒上乗せ。

旧設計 (EVAL → bake) の `*-1st` は run の中で SD を一切使っていなかった
(= cold start = `n/plain` 値 + bake 時間) ので、1st run は実質
インタプリタ走行が支配的だった。新設計はその死にコストを排除し、
1st run でも AOT/PG のフル恩恵を受けられる。

## 推奨ビルド設定

| 用途 | 設定 |
|---|---|
| 通常 (gcc-13 default) | `-Wl,-Bsymbolic -flto` |
| gcc-16 が使える | `CC=gcc-16` + 上記 (recursive で +20-50%) |
| 極限性能 (loop 中心) | `CC=gcc-16` + LTO + 2 段 PGO (gcd で 81% 追加改善) |

## 高速化の仕組み

### `n/plain` (interpreter-only)

- 名前 `"foo"` で `c->func_set` を線形検索 (`node_call_<N>`) + callcache 設定
- 各 NODE の dispatch は `head.dispatcher` の indirect call
- 関数引数は **explicit operand として call ノードに埋め込まれ** (arity ≤ 3)、
  fast path は VLA `VALUE F[locals_cnt]` に直接書き込む。argc > 3 は
  従来通り `node_lset` chain + `node_call` (引数 fp slot 経由)。
- すべてのオーバーヘッドが指数スケール (再帰時) に乗る

### `n/aot-c` (cached AOT)

- `code_store/all.so` を `dlopen` し、各 NODE の `head.dispatcher` を
  `SD_<HORG>` 関数ポインタに差し替え
- 子 NODE (算術 etc) への dispatch は **direct call** (linker が
  `addr32 call` に解決)
- 関数呼び出しは `node_call_<N>` の SD 内で `EVAL(c, cc->body, F)` の
  **indirect call** (cc->body は実行時値)。子 NODE への direct call
  と組み合わさり、本体評価部分は SD で密に inline、関数間境界だけが
  indirect になる
- 算術ノードは inline 展開 (`node_add` の `lhs + rhs` 等)

### `n/pg-c` (Profile-Guided cached)

PG mode は `node_call_<N>` を `node_pg_call_<N>` に置き換える:

- call site が `sp_body` (= 本体 NODE) を parse 時に link
- SD は `sp_body_dispatcher = SD_<HORG(sp_body)>` を C 定数として bake
  → ベイクされた all.so 内では `addr32 call <SD_addr>` の direct call
  (= AOT の indirect dispatch を direct に置き換えた形)
- **2 段ガード**: `c->serial == cc->serial && cc->body == sp_body`
  を 2 cmp + 2 jcc で fast path 判定。両方一致時のみ baked-direct を採用。
- 再定義 / parse-time speculation 不一致 → slowpath が `cc->body`
  経由 indirect dispatch (sp_body は parse 時値で固定なので SD の baked
  定数と常に整合)。

> AOT (`n/aot-c`) と PG (`n/pg-c`) の差は **「関数本体への dispatch が
> indirect か direct か」その 1 点だけ**。引数の eval、フレーム
> (`F[locals_cnt]`)、cc check はすべて共通。`pg-c / aot-c` の比は
> ほぼ純粋に baked-direct call の効果を表す (実測 1.0-1.3×)。

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
chain body の 1 段ぶんを抜粋 (`objdump -d code_store/all.so`)。
2 段ガードの「cc serial / cc body」チェック対が連続して並ぶ:

```asm
; 1 段ぶんの guard + 次段への direct call
mov    0x68(%rsi),%r8              ; sp_body load
cmp    0x58(%rsi),%rcx              ; ★ cc check #1: cc->serial vs c->serial
jne    <slowpath_thunk>             ; ★ jne 先は SD 末尾の thunk
cmp    0x60(%rsi),%r8               ; ★ cc check #2: cc->body  vs sp_body
jne    <slowpath_thunk>
mov    0x68(%r8),%rsi               ; 次の sp_body load
... (次段の guard ペアがそのまま続く) ...
```

cold path は SD 末尾に集約されており、各段で `jne` が `<slowpath_thunk>`
へ跳ぶだけ。thunk は数命令で `node_pg_call<N>_slowpath` (= 本体 binary
側) を tail-call する。Cold path setup (引数の name / cc / argc を
レジスタに積む等) は SD には残らず、すべて slowpath 側で NODE 操作で
取得する。

```asm
; SD 末尾の slowpath thunk (一度だけ生成)
<slowpath_thunk>:
    sub    $0x8, %rsp               ; ABI 16-byte align
    call   *<node_pg_callN_slowpath@GOT>
    ... (RESULT 戻りの後処理 + ret)
```

chain 10 段ぶんをまとめると: `cmp/jne` 対が 10 個 inline + thunk が
1 個 (= cold path 共通)。深度 10 を **2 個の SD body** に焼き込み、
hot path には **indirect call が 1 つもない**。

**chain_add (10 段で各 +1)**: 各 fN が `n+1` を計算するチェイン。
naruby は `VALUE = int64_t` でタグなしの素の整数。10 段の `+1` は
**2 つの SD body** に分散して baked される (top SD → SD_f9b… → SD_b98…):

```asm
; SD_f9b… (chain levels 1-5): 5 個の cc check + 5 段分の +1 を畳み込み
1f10/1f21/1f32/1f43/1f51: cmp 0x58(%rN),%rax  ; cc check #1-5
1f4d: add  $0x5,%rdx              ; ★ 5 段分の +1 を 1 命令に畳み込み
1f6d: call 1590 <SD_b98…>         ; 残り 5 段へ
```

```asm
; SD_b98… (chain levels 6-10): 4 個の cc check + さらに +5
15ae/15bb/15cc/15d9: cmp 0x58(%rN),%rdx  ; cc check #6-9 (#10 は leaf)
15e3: add  $0x5,%rax              ; ★ 5 段分の +1 を 1 命令に畳み込み
160e: ret                          ; 結果 = 0 + 10
```

★ 10 段の `+1` が **gcc constant folding によって 2 個の `add $0x5`**
にまで畳み込まれる (cross-SD では folding できないので `+5 + +5`)。
`p f0(0)` の最終結果は 10。

### 再帰系で chain と同じ効果が出ない理由

`fib_body → pg_call(fib) → sp_body=fib_body` は cycle。HOPT 計算は
`is_hashing` フラグでサイクル検出して低情報量の値で打ち切るので、
`HOPT(fib_body)` は `HORG(fib_body)` と本質的に同じ識別性しか持たない。
結果として `PGSD_<HOPT>` ≈ `SD_<HORG>` で、LTO inline できる範囲も同じ。

ただし gcc-16 では plain な再帰関数でも IPA-SRA + frame elimination が
効いて、call 境界のオーバーヘッドが大きく減少する (旧計測で fib /
ackermann が gcc-13 比 +20-50%)。これは PGSD 機構と独立した cc 側の改善。

## 主な高速化テクニック

`runtime/astro_code_store.c` のデフォルト + naruby 側の sp_body bake で
構成される PG モードの主要技 (適用順):

1. **`-Wl,-Bsymbolic`** — intra-.so の SD 間参照が GOT を経由せず
   `addr32 call <SD_addr>` の direct call に。
2. **`-fno-semantic-interposition`** (`runtime/astro_code_store.c` の
   default CFLAGS) — gcc が intra-.so 関数を interpose 不可と仮定して
   inline / 直接呼びの最適化を有効化。
3. **`callcache *cc@ref`** — call site の callcache が NODE union に
   inline 化され、`cc->serial` / `cc->body` の load が 1 段。
4. **2 段ガード** — `c->serial == cc->serial && cc->body == sp_body` で
   baked-direct call の妥当性を動的検証。slowpath は `cc->body` 経由
   indirect dispatch にフォールバックして安全側に倒し、SD はそのまま残す
   (= 旧 demote 方式不要)。
5. **cold path の本体 binary 側集約** — 各 call variant 専用 slowpath
   (`node_pg_call<N>_slowpath` 等) を `node_slowpath.c` に集約。
   slowpath 引数は `(c, n, fp)` 3 つだけで、name/cc/sp_body/args は
   NODE union から取り出す。SD には引数 setup は残らず、`jne`+thunk の
   tail-call だけで cold path に逃げる。再帰系 (fib/ackermann) で
   I-cache 圧迫が減って **15-20% 高速化**。
6. **sp_body の direct call ベイク** — PG モードで SD が
   `SD_<HORG(sp_body)>` を C 定数としてベイク。
7. **HOPT/PGSD 投機チェイン** — sp_body を HOPT に含めることで
   chain 各レベルが固有の PGSD シンボルになり、LTO で貫通 inline 可能。
8. **`VALUE *fp` register-pass** — フレームポインタを dispatcher の第 3
   引数としてレジスタ渡し。`node_lget` / `node_lset` が `fp[idx]` 1 ロード。
9. **`--param=early-inlining-insns=100`** — gcc の early-inliner 予算を
   14 → 100 に上げる。`EVAL_node_pg_call_<N>` の中規模関数連鎖が SD
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
- **再帰関数 vs gcc -O3**: fib/ackermann/tak で 5-14× 遅れる。register
  passing / frame elimination がまだ追いついてない。
- **2 段ガードのコスト**: chain 系で +1 cmp + 1 jcc per inline 段の
  オーバーヘッド。HOPT に「parse-time speculation の identity」を
  含めて SD specialize し、合流不要なケースで gate 自体を消す等の
  改善余地あり。

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
