# naruby 性能メモ

ASTro 論文評価対象として **「Plain / AOT / Profile-Guided / JIT を
1 バイナリで切り替え可能な最小の Ruby 系」** という位置づけ
なので、絶対性能を尖らせる方向ではなく、**4 モードの差を比較すること**
に焦点を当てている。

## 計測環境

- マシン依存。本ドキュメントの数値は WSL2 + Linux 6.8 + gcc 13、
  オプション `-O3 -ggdb3 -march=native` (host binary)、SD は
  `runtime/astro_code_store.c` 既定の `-O3 -fPIC -fno-plt -march=native`。
- 各セルは `ruby bench/bench.rb <bench>` を 1 回流した値 (median ではない)。
- 単位は秒 (`real` time)。
- 全ベンチは末尾で `p result` で結果を出力する。最適化系が dead-code
  elimination で計算ごと削れないようにするため (詳細は §公平性)。
- `naruby/aot` は `bench.rb` が `./naruby -c` で `code_store/all.so` を
  事前ベイクしてから、`./naruby -b` で計測。
- `naruby/pg` は `./naruby -p` で暖機 (実行 + bake) してから
  `./naruby -p -b` で計測。
- `ruby` / `ruby/yjit` は CRuby (4.0.2) を `RUBY_THREAD_VM_STACK_SIZE=32MiB`
  で実行 (ackermann が default thread VM stack を超えるため)。
- `spinel` は [matz/spinel](https://github.com/matz/spinel) Ruby AOT
  コンパイラ。`SPINEL_DIR=/path/to/spinel` を渡すと bench.rb が
  `spinel <bench>.na.rb -O 3 -o b_spinel` で事前コンパイルし、
  生成バイナリを実行。
- gcc 列は `bench/<name>.c` を `-O0`/`-O1`/`-O2`/`-O3` でビルドしたバイナリ。
  C 比較用に手書きした参照実装。
- ccache を `~/.cache/ccache` に書ける環境でない場合は `CCACHE_DISABLE=1`
  を環境変数で渡す (sandbox 環境ではこれが必須)。

## ベンチ題材一覧

| ベンチ | 内容 | スケール (plain ≥ 1 s) |
|---|---|---|
| fib | naïve `fib(40)` (再帰) | fib(40) → 165580141 |
| ackermann | `ack(3, 11)` (深い再帰、~16K stack) | → 16381 |
| tak | `tak(30, 22, 12)` (Takeuchi 関数、深い再帰) | → 13 |
| gcd | `gcd(2³¹−1, 2³⁰−1)` を 5×10⁷ 回ループ + 累積 | mod ベースのタイトループ |
| collatz | `collatz_steps(i)` を 1..10⁶ で総和 | if/else 分岐ループ |
| early_return | `find_first_factor(n)` を 2..2×10⁶ で総和 | `return` inside `while`+`if` |
| loop | 単純 `while i<10⁸; i+=1; end` | ループだけ |
| zero | `zero` を 10⁹ 回呼ぶ | call overhead 計測 |
| call | 10 段の自己尾呼出 `f0→f1→...→f9` を 10⁸ 回 | 深さ 10 の call chain |
| call0 | `zero(0) = one(0) = 1` を 10⁹ 回 | call overhead 計測 (2 段) |
| prime_count | エラトステネス相当 `prime_count(10⁵)` を 100 回 | mixed loop + 分岐 |

## 結果一覧

10 行 × 11 ベンチ。単位は秒。

| bench | ruby | yjit | spinel | n/plain | n/aot | **n/pg** | -O0 | -O1 | -O2 | -O3 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fib (40) | 11.4 | 1.32 | 0.16 | 6.95 | 1.21 | **0.91** | 0.57 | 0.48 | 0.13 | 0.16 |
| ackermann (3,11) | 7.46 | 0.91 | 0.072 | 8.94 | 1.53 | **1.13** | 0.66 | 0.37 | 0.071 | 0.075 |
| tak (30,22,12) | 1.38 | 0.21 | 0.028 | 1.29 | 0.19 | **0.17** | 0.081 | 0.067 | 0.053 | 0.029 |
| gcd | 5.62 | 2.37 | 0.19 | 4.44 | 0.21 | **0.21** | 0.25 | 0.20 | 0.20 | 0.20 |
| collatz | 6.09 | 1.05 | 0.19 | 4.44 | 0.18 | **0.19** | 0.41 | 0.18 | 0.18 | 0.18 |
| early_return | 6.95 | 0.82 | 0.33 | 5.52 | 0.34 | **0.34** | 0.37 | 0.33 | 0.33 | 0.34 |
| loop | 1.02 | 1.04 | 0.001¹ | 1.14 | 0.001 | 0.002 | 0.049 | 0.060 | 0.002 | 0.001¹ |
| zero | 37.0 | 10.9 | 0.002¹ | 21.7 | 3.67 | **0.69** | 1.84 | 0.30 | 0.001¹ | 0.001¹ |
| call | 22.5 | 6.38 | 0.001¹ | 12.0 | 2.78 | **1.69** | 1.43 | 1.31 | 0.43 | 0.42 |
| call0 | 55.7 | 33.3 | 0.002¹ | 30.1 | 4.71 | **3.38** | 1.74 | 0.28 | 0.001¹ | 0.001¹ |
| prime_count | 10.8 | 3.93 | 0.007¹ | 8.32 | 0.53 | **0.53** | 0.58 | 0.007¹ | 0.007¹ | 0.007¹ |

¹ 出力は最終 `p result` の 1 値のみ。ループ本体に observable な
side effect がないと spinel / gcc -O2/-O3 が **whole-loop DCE**
してしまう (1 回の最後の値だけ計算してループは飛ばす)。これは
最適化系として正しい挙動なので bench は意図的に書き換えていない。
インタプリタ系 (ruby / yjit / naruby) はループを実行するので
ここで桁違いに遅く見える。

## 公平性 — 結果出力 (`p result`) について

最初は bench/`*.c` 側で `if (argc > 1) fprintf(...)` ガードを使い、
gcc に値を生かさせつつ普段は出力しない、という構成だった。一方で
spinel が出す C は Ruby セマンティクス通り「式文末は値を捨てる」を
直訳して `sp_fib(40);` のように戻り値を完全に捨てる。結果として gcc
は spinel 経由のソースのほうを **戻り値チェーンの DCE で軽くできて
しまい**、「spinel が gcc -O3 を 1.5× 上回る」ように見えていた。

この非対称はベンチ側の取り回しの問題なので、全 11 ベンチで末尾に
`p result` (naruby は `p`、C は `printf`) を入れて結果を必ず観測
する形に揃えた。今の表は spinel ≒ gcc -O3 にぴったり並ぶ:

| bench | spinel | gcc -O3 | spinel/-O3 |
|---|---:|---:|---:|
| fib | 0.164 | 0.157 | 1.04× |
| ackermann | 0.072 | 0.075 | 0.96× |
| tak | 0.028 | 0.029 | 0.97× |
| gcd | 0.192 | 0.195 | 0.98× |
| collatz | 0.191 | 0.176 | 1.08× |
| early_return | 0.328 | 0.336 | 0.98× |

これは妥当で、spinel のパイプラインは「型推論 + 等価な C 出力 + cc」
なので、最終速度は **gcc -O3 そのもの**に収束する。spinel 行は
表のなかで「Ruby 意味論で書き直したときの gcc -O3 の代理値」として
読める。

## 観測ポイント

### naruby/pg vs gcc -O3

実計算系で見たとき、naruby/pg が gcc -O3 と並ぶか負けるか:

| bench | naruby/pg | gcc -O3 | 比 |
|---|---:|---:|---:|
| **gcd** | 0.21 | 0.20 | 1.05× ✅ 並ぶ |
| **collatz** | 0.19 | 0.18 | 1.06× ✅ 並ぶ |
| **early_return** | 0.34 | 0.34 | 1.00× ✅ 並ぶ |
| fib | 0.91 | 0.16 | 5.7× ❌ 再帰オーバーヘッド |
| ackermann | 1.13 | 0.075 | 15× ❌ |
| tak | 0.17 | 0.029 | 5.9× ❌ |

**ループ + 算術中心の bench では gcc -O3 と並ぶ**。再帰中心では関数
呼び出し境界 (frame setup + call cache check + RESULT 伝播) が
hot loop 内に残るので gcc -O3 にはまだ届いていない。

### naruby/pg vs ruby/yjit

| bench | naruby/pg | yjit | 比 |
|---|---:|---:|---:|
| fib | 0.91 | 1.32 | 0.69× (naruby 勝) |
| ackermann | 1.13 | 0.91 | 1.24× (yjit 勝) |
| tak | 0.17 | 0.21 | 0.81× (naruby 勝) |
| gcd | 0.21 | 2.37 | 0.09× (naruby 11× 勝) |
| collatz | 0.19 | 1.05 | 0.18× |
| early_return | 0.34 | 0.82 | 0.41× |
| zero | 0.69 | 10.9 | 0.06× |
| call | 1.69 | 6.38 | 0.26× |
| call0 | 3.38 | 33.3 | 0.10× |
| prime_count | 0.53 | 3.93 | 0.13× |

naruby/pg はほぼ全ベンチで yjit を上回る。yjit が勝つのは
ackermann のみで、深い再帰における yjit のインラインキャッシュ +
side-exit 戦略が効いている。

### Why is plain mode so slow?

`fib(40)` は再帰呼び出しが約 3.3 億回。各呼び出しで:

1. 名前 `"fib"` で `c->func_set` を線形検索 (`node_call`)
2. callcache が hit すれば body を取得
3. arg を slot に書く (lset)
4. fp 移動 (`node_scope`)
5. body 内の `if` (lt 比較 + 条件分岐)
6. recurse 2 回 + add

各ステップが indirect call (関数ポインタ越し)。指数スケールでこの
オーバーヘッドが乗るので 7 秒レンジ。

### Why does AOT speed it up so much?

AOT load 後は `head.dispatcher` が `SD_<hash>` に差し替わっている。
SD 関数の中では:

- 子 NODE への dispatch が **direct call** になる (gcc が `static inline`
  したり `__attribute__((no_stack_protector))` で素直に呼んだり)
- 整数演算 (`node_add` の `lhs + rhs`) はインライン展開される
- メモリアクセスが固まり、レジスタアロケーションが効く

結果として fib の再帰呼び出しはほぼ「fp 移動 + if + 2 回再帰 + add」の
コードになり、indirect call の山が消える。

### Why is `naruby/pg` faster than `naruby/aot`?

PG mode (`-p`) emits `node_call2` instead of `node_call` for call sites,
and naruby's parser links the function body into `node_call2.sp_body`
at parse time (callsite_resolve, naruby_parse.c).  Three consequences:

- The hot-path check shrinks from `cc->serial == c->serial` followed
  by `call_body(c, idx, cc->body)` (load `cc`, then `cc->serial`,
  then `cc->body`, then `body->head.dispatcher`, then indirect call)
  to `cc->serial == c->serial` then a direct call (load `cc.serial`
  inline, then direct call to baked SD constant).
- The SD baked for the call site bakes `SD_<HASH(sp_body)>` as a C
  compile-time constant.  The linker (with `-Wl,-Bsymbolic`) resolves
  the cross-SD reference to a direct PC-relative `addr32 call`, no
  GOT indirection.
- `cc` is `@ref` so it lives inline in the NODE union — `cc->serial`
  is a single load on `n->u.node_call2.cc.serial`, not a pointer
  chase through a separately-malloc'd cache cell.

Method redefinition is handled by `node_call2_slowpath` demoting the
call node's dispatcher back to the interpreter (`DISPATCH_node_call2`)
when it observes a real cache miss (`cc->serial != 0`).  That site
then runs through the indirect path forever, accepting the slowdown
in exchange for not having to re-bake or rewrite SDs at runtime.

### Why doesn't naruby/pg beat gcc -O3 on recursive benches?

fib / ackermann / tak で 5–15× 遅れる原因:

1. 関数呼び出し境界の **frame setup** が SD 内に残る (`fp + arg_index`
   への lset、push_frame、cc->serial check)。gcc -O3 は単純な再帰
   関数を register passing のみで回し、メモリアクセスゼロにできる。
2. **RESULT envelope** (16 byte / rax+rdx) が末端 leaf でも propagate
   される。インライン化されれば DCE 可能だが、再帰呼び出し越境で
   DCE できない値が残る。
3. gcc -O3 は fib のような再帰関数に対して **interprocedural
   specialization** (`fib.isra.0`) や末尾呼び出しの一部を畳み込む
   ことがある。ASTro の partial evaluation はノード単位の
   specialize で、call boundary を超える specialize はしていない。

ループ + 算術中心のベンチ (gcd / collatz / early_return) では
これらの境界コストが少ないので gcc -O3 と並ぶ。

主な naruby 側の高速化テクニック (適用順):

1. **`-Wl,-Bsymbolic`** — intra-.so の SD 間参照が GOT を経由せず
   直接アドレス解決され、`addr32 call <SD_addr>` の direct call に
   落ちる (vs `call *0x...(%rip)` の indirect)。
2. **`callcache *cc@ref`** — call site の callcache が NODE union
   に inline 化され、`cc->serial` の load が 2 段から 1 段に。
3. **sp_body の direct call ベイク** — PG モードで SD が
   `SD_<HASH(sp_body)>` を C の定数としてベイク。dispatcher の
   ロードと indirect call が消える。再定義は slowpath での
   dispatcher 降格で対応。
4. **`VALUE *fp` register-pass** — フレームポインタを `c->fp` 経由の
   メモリ往復ではなく dispatcher の第 3 引数としてレジスタ渡し。
   `node_lget` / `node_lset` が `fp[idx]` 1 ロードで済む。
5. **`--param=early-inlining-insns=100`** — gcc の early-inliner
   予算を 14 → 100 に上げる。EVAL_node_call2 / call_body の中規模
   関数連鎖が SD に inline され、SROA がフレームスロットをレジスタに
   昇格できる。

## 過去の主要マイルストーン

> 履歴は git log (`sample/naruby/`) を参照。代表的なものを抜粋:

- 2026-05-02: ベンチ 6 種追加 (`ackermann` / `tak` / `gcd` / `collatz` /
  `early_return`) + `ruby` / `ruby/yjit` / `spinel` 行を bench.rb に追加。
  全ベンチを `p result` で結果出力に統一して DCE 非対称を解消。
- 2026-05-02: PG モードの sp_body direct-call ベイク + fp register-pass
  + RESULT 型導入で `return` 対応。fib(40) PG 0.85s → 0.74s。
- 2026-05-02: `node_specialized.c` 静的 AOT モデルを `runtime/astro_code_store.c`
  の `code_store/all.so` 動的ロードモデルに置換。`sc_repo` を撤去。
- 2026-05-02: `node.c` の重複した hash / HASH / DUMP / alloc_dispatcher_name
  を `runtime/astro_node.c` の include に置き換え。`hash_builtin_func` のみ
  naruby 側に残した。
- (initial) ASTro 論文向けに 4 モード切り替えを統合。

## 計測の取り方 (再現手順)

```sh
# クリーンビルド + ccache 経由しないこと
make clean
make

# ベンチランナー (ベンチ題材は bench/<name>.na.rb / bench/<name>.c)
CCACHE_DISABLE=1 make bench BITEM=fib

# spinel 行も含める場合
SPINEL_DIR=/path/to/spinel CCACHE_DISABLE=1 make bench BITEM=fib

# ruby / yjit / gcc 行をスキップしたいときは bench.rb の env で:
#   NORUBY=1   ruby / yjit を skip
#   NOYJIT=1   yjit のみ skip
#   NOGCC=1    gcc -O0..-O3 を skip

# 個別の確認
CCACHE_DISABLE=1 ./naruby -c bench/fib.na.rb         # AOT bake
CCACHE_DISABLE=1 time -p ./naruby -b bench/fib.na.rb # warm load 実行

# インタプリタのみ
CCACHE_DISABLE=1 time -p ./naruby -i bench/fib.na.rb
```

bench 数値が大きく動いたときは:

1. `make clean && make` で再生成漏れを確認
2. `code_store/c/SD_*.c` を全消しして再ベイクが本当に走っているか確認
3. 同じ題材で `CCACHE_DISABLE=1 ./naruby --dump` で AST に変化がないか確認
   (`--dump` は未実装だが、`DUMP(stdout, ast, true)` を main.c に
   足せばすぐ吐ける)
