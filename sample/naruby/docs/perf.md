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

## HOPT / PGSD 投機チェイン (2026-05-03)

ASTro framework の HOPT (profile-aware hash) / PGSD (profile-guided
specialized dispatcher) 機構を有効化。**非再帰 call chain では
LTO 併用で 47–84% 高速化**。

### 仕組み

- **HORG (existing)**: `hash_node()` でキャッシュされる構造ハッシュ。
  pg_call の `sp_body` は除外 → 同名関数の call site identity として
  parse-time mutation や redef に対して安定。
- **HOPT (new)**: `hash_node_opt()` でキャッシュされる profile-aware
  ハッシュ。`sp_body` を含めて再帰計算 (cycle break 付き) → 投機
  対象の構造を fingerprint。再帰関数では cycle に入った時点で 0 を
  返すので、本質的に HORG と同じハッシュに落ちる。
- **SD_<HORG> / PGSD_<HOPT>**: 同じ body NODE が両方の名前で baked。
  SD 系は AOT (HORG-keyed)、PGSD 系は PGC (HOPT-keyed)。
- `cs_load(n, file)` の `file != NULL` 時は `hopt_index.txt` を引いて
  PGSD_<HOPT> を採用。`file == NULL` または見つからなければ
  SD_<HORG> へ fall-back。
- naruby 側では:
  - `node_def` 実行時に body を `cs_load(body, name)` して body の
    dispatcher を PGSD に
  - `main.c` で top-level AST を `cs_load(ast, source_file)` して top も
    PGSD に
  - body と top の両方で AOT(SD) と PGC(PGSD) の両形を bake
    (top-level SD は AOT 側に baked-direct-call を SD_<HORG(body)> で
    持つので、互換のため SD 形も残す)

### 実測 (2026-05-03)

各ベンチ 5 回 median、単位: 秒。`pg` は `astro_cs_compile(_, file)` で
PGSD bake (`-Wl,-Bsymbolic`)、`pg+lto` は それ + `-flto`。

#### gcc 13 (default `gcc`) ベース

| bench | plain | pg | pg+lto | pg → pg+lto |
|-------|-----:|----:|-------:|------------:|
| fib | 5.61 | 0.55 | 0.56 | 0% (再帰 cycle break) |
| ackermann | 7.54 | 0.79 | 0.75 | -5% |
| tak | 1.12 | 0.11 | 0.11 | 0% |
| gcd | 3.85 | 0.17 | 0.16 | -6% |
| **call** (10 段尾呼出) | 9.47 | 1.15 | **0.49** | **-57%** |
| **zero** (1B builtin call) | 15.89 | 2.68 | **0.42** | **-84%** |
| **chain20** (20 段尾呼出) | 9.67 | 1.23 | **0.65** | **-47%** |
| **chain_add** (10 段 +1) | 1.33 | 0.11 | **0.05** | **-55%** |
| **compose** (3 段 f(g(h(x)))) | 1.50 | 0.10 | **0.03** | **-70%** |
| **branch_dom** (固定分岐) | 1.24 | 0.07 | **0.02** | **-71%** |
| **deep_const** (10 段 getter) | 3.76 | 1.18 | **0.58** | **-51%** |

#### gcc 16 (`CC=gcc-16` 指定)

gcc-16 は LTO 周りと recursive 関数の inlining が大幅改善されてて、
非 LTO の段階で fib / ackermann / tak が gcc-13 + LTO を凌ぐ:

| bench | plain | pg(g16) | pg+lto(g16) | gcc-13 pg+lto との差 |
|-------|-----:|--------:|------------:|---------------------:|
| fib | 5.38 | **0.47** | 0.48 | -16% |
| ackermann | 7.44 | **0.46** | 0.47 | -38% (再帰の劇的改善) |
| tak | 1.13 | **0.05** | 0.05 | -55% |
| gcd | 3.78 | 0.17 | 0.17 | +6% (誤差) |
| call | 9.93 | 1.15 | 0.49 | 同 |
| zero | 16.18 | 0.67 | 0.69 | +64% (gcc-13 で偶然 0.42 だっただけ?) |
| chain20 | 9.39 | 1.17 | **0.60** | -8% |
| chain_add | 1.22 | 0.11 | **0.04** | -20% |
| compose | 1.51 | 0.12 | **0.04** | +33% (誤差) |
| branch_dom | 1.24 | 0.07 | **0.03** | +50% (誤差) |
| deep_const | 3.35 | 1.13 | **0.47** | -19% |

★ **gcc-16 の効果**: recursive benches では `pg(g16)` が `pg+lto(g16)` と
ほぼ同等に速い。LTO なしでも recursive 関数の SROA + frame elimination が
強くなり、call 境界のオーバーヘッドが大きく減少。

非再帰 chain 系では gcc-13 と gcc-16 で大差なし (LTO の chain inline は
gcc-13 でも完成済)。

#### gcc 16 + LTO + PGO (2 段)

`-fprofile-generate=DIR` で instrument → 1 回実行で `*.gcda` 収集 →
`-fprofile-use=DIR` で再ビルド。PGO の効果は **chain 系よりも
loop 系で顕著** (gcc が分岐確率を学習してループ unrolling / 命令再配置):

| bench | LTO only | +PGO | Δ |
|-------|---------:|-----:|---:|
| fib | 0.48 | 0.47 | -2% |
| ackermann | 0.47 | 0.44 | -6% |
| tak | 0.05 | 0.04 | -20% |
| **gcd** | 0.16 | **0.03** | **-81%** (PGO で loop 全展開) |
| call | 0.48 | 0.43 | -10% |
| zero | 0.67 | 0.67 | 0% |
| chain20 | 0.60 | 0.57 | -5% |
| chain_add | 0.05 | 0.04 | -20% |
| compose | 0.04 | 0.04 | 0% (already saturated) |
| branch_dom | 0.03 | 0.02 | -33% |
| deep_const | 0.49 | 0.41 | -16% |

★ **gcd の劇的改善**: PGO で loop counter (50M iterations) が hot と判明
→ gcc がループを unroll + 算術命令の長依存を独立並列化 → 1 cycle/iter
近くまで詰まる。chain 系は既に PGSD+LTO で saturated なので PGO 効果は
小さい。

詳細な compiler 比較は git log を参照。

### まとめ: 推奨ビルド設定

通常用途 (gcc 13 default): `-Wl,-Bsymbolic -flto`
gcc 16 ある場合: `CC=gcc-16` + 上記
極限性能: `CC=gcc-16` + LTO + 2 段 PGO (`-fprofile-generate` で profile
収集 → `-fprofile-use` で再ビルド)。loop 重視のベンチで最大 81% の
追加高速化。

### なぜ非再帰 chain で大きいか

PGSD chain は各 call site の sp_body を HOPT に含めて baked するので、
**chain 各レベルの SD が下流のすべての構造を符号化したユニークな
シンボル** になる。LTO 併用時 gcc/clang の inliner はこれらの
`static inline` を直接呼ぶ前提でクロス TU inline できる:

```
PGSD_<top>     ─inline→ PGSD_<HOPT(f0_body)>
                            ─inline→ PGSD_<HOPT(f1_body)>
                                        ─inline→ ...
                                                  ─inline→ PGSD_<HOPT(f9_body)>
                                                              = literal (e.g., n+1)
```

guard チェック (cc->serial == c->serial) も各レベルで残るが、
LTO のチェイン inline で hot path 全体が 1 つの関数体にまとめられ、
ループ本体で 0 〜 数 ns/call まで畳み込まれる。

### なぜ再帰 chain で効かないか

`fib_body → pg_call(fib) → sp_body=fib_body` は cycle。HOPT 計算は
`is_hashing` フラグでサイクル検出して 0 を返すので、HOPT(fib_body)
は実質的に HORG と同じ情報量しか持たない (sp_body の構造を識別
できない)。結果として `PGSD_<HOPT(fib_body)>` ≈ `SD_<HORG(fib_body)>`
で、LTO inline できる範囲も同じ。

### 実装ファイル

- `node.h`: `HOPT(n)` / `hash_node_opt(n)` 宣言、`HORG = HASH` macro
- `node.c`: `HOPT` 実装 (kind->hopt_func dispatch + has_hash_opt cache)
- `node.def`: `node_def` 実行時に `cs_load(body, name)` を追加
- `naruby_gen.rb`: `:hopt` gen_task 登録、`sp_body` の HORG/HOPT
  分岐 (`kind: :hopt` で `hash_node_opt`)、build_specializer の
  PGSD/SD prefix runtime 切替 (`astro_cs_use_hopt_name`)
- `naruby_parse.c`: `naruby_current_source_file` グローバル
- `main.c`: top-level の `cs_compile(ast, source_file)` + `cs_load(ast, source_file)`、
  bodies は AOT + PGC の両形 bake
- 生成: `node_hopt.c` (per-NODE_DEF `HOPT_<name>` 関数)

### 残課題

- HOPT 計算の cycle break は kind id 等を含めず単に 0 を返す → 再帰
  関数の HOPT が HORG と区別できない。本来は parent SCC を含む
  Tarjan ベースの hash で識別したい (将来課題)
- PGSD と SD を両 bake すると `code_store/all.so` のサイズが約 2 倍。
  HOPT が AOT bake にしか使われないケースでは PGC bake を skip する
  最適化余地あり。
- profile データ (= "実 runtime に観測された body") を反映する仕組みは
  未実装。現状は parse-time の `code_repo` last-wins を speculation
  default として使う (= 既存挙動)。redef の頻度が低ければこの
  default で十分機能する。

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
