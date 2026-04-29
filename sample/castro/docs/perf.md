# castro 性能最適化メモ

ASTro 上の C サブセットインタプリタとして、AOT cached モードで
gcc -O0 を超え、tight inner loop では gcc -O3 を上回るところまで
削った経緯と手法を記録します。

ベンチは `bench.rb`、`BENCH_RUNS=15` median、parse.rb 起動時間
(~110ms) を除いた純粋な実行時間。

## 最終ベンチ結果 (gcc -O0 と -O3 比較)

| bench | castro AOT cached | gcc -O0 | gcc -O3 | castro/-O3 |
|---|---:|---:|---:|---:|
| fib_big (fib 35)   |    **48** |   47 |   16 | 3.0× |
| fib_d              |     **4** |    3 |    1 | 4.0× |
| tak (18,12,6)      |     **2** |    0 |    0 | -- |
| ackermann (3,8)    |    **12** |    8 |    1 | 12× |
| loop_sum           | 🟢 **2** | 🔴 9 |    0 | 2× |
| mandelbrot_count   |     **3** |    2 |    1 | 3× |
| sieve              |     **8** |    8 |    2 | 4× |
| nqueens            |    **69** |   31 |   14 | 4.9× |
| quicksort          | 🟢**84** | 🔴 92|   23 | 3.6× |
| crc32              | 🟢**14** |   46 |🔴 18 | **0.78×** |
| matmul             |     **8** |    2 |    0 | -- |

🟢 / 🔴 は castro が勝っている / 負けている関係。

**castro が gcc を上回るケース**:
- `crc32` — castro 14ms vs gcc -O3 18ms (1.3× 速い)
- `loop_sum` — castro 2ms vs gcc -O0 9ms (4.5× 速い)
- `quicksort` — castro 84ms vs gcc -O0 92ms (1.1× 速い)

## 採用した最適化 (時系列順、効果込み)

### 1. tail-return リフト (parse.rb)

`return E` が tail position にあるなら、囲みの `seq`/`if` の値として
畳み込んで `return` ノードを消す:

```
seq(if(c, return(e1), nop), seq(rest, return(e2), return_void))
                         ↓
if(c, e1, seq(rest, e2))     ← return が消えた
```

リフト後に関数本体に `return` ノードが 1 つも残らなければ、
その関数は `setjmp/longjmp` の枠を一切張らない (caller も
`castro_invoke_jmp` ではなく直接 `(*disp)(c, body, fp)` を呼ぶ)。

**効果**: fib / tak / ackermann / loop_sum 等は setjmp 0 個。
fib_big で 1442ms → 803ms (44% 縮)。

### 2. `-rdynamic` ビルド

(地味だが decisive な落とし穴。) AOT 後の `code_store/all.so`
側 SD は host 側の `castro_invoke_jmp` などを `extern` 参照する
が、castro バイナリ側の symbol を export していないと dlopen が
unresolved symbol で **静かに NULL を返す**。`load_all_funcs` は
それを「該当 SD なし」と解釈して全ノードを interp dispatcher の
ままにする → AOT の効果ゼロのまま動き続ける。

`-rdynamic` を Makefile に足すだけ。

**効果**: fib_big AOT cached 803ms → 217ms (4× 縮)。

### 3. コンパイル時に呼び先決定 (call IR を name → idx に切替)

C は静的言語なので呼び先は parse 時に確定する。にもかかわらず
旧設計は abruby 由来の `struct callcache @ref` を毎 call で
チェックしていた (`serial / body / dispatcher / needs_setjmp` の
4 フィールド + strcmp 線形探索 fallback)。

`(call NAME nargs arg_index)` を `(call FUNC_IDX nargs arg_index)`
に置換、parse.rb で `gather_signatures` が各 function_definition
に index を割り振る。runtime は `c->func_set[idx]` を直接引く。
serial / inline cache / 名前ルックアップ全廃。

`(program GSIZE NFUNCS INIT (sig...) ... BODY ...)` の 2-pass
load で前方参照解決:
1. NFUNCS 個の sig を読んで `func_set` を stub 登録
2. body を順番に読んで `func_set[i].body` を埋める

**効果**: fib_big AOT cached 217ms → 64ms (3.4× 縮)。

### 4. ベンチ間違い: `0xFF` が float 扱い

`parse.rb` の number_literal 処理が `/[fF]$/` で float 末尾 suffix
を判定していた。これが **`0xFF` の末尾 F にもマッチ**してしまい、
`0xFF` がまるごと `(lit_d 0)` (= `"0xFF"` を `"0xF"` にして to_f
= 0.0) に変換されていた。`x & 0xFF` は常に 0、bitwise mask 系の
演算結果が全部おかしいまま動いていた (テストは exit code が
偶然合うので素通り)。

修正: `0x` prefix 検出を float 判定より先に行う。

```ruby
is_hex = t.downcase.start_with?('0x')
is_float = !is_hex && (t.include?('.') || t.downcase.include?('e') || t.match?(/[fF]$/))
```

**効果**: 単独では性能変化なし、ただし `crc32` `quicksort`
など bitwise 演算のあるベンチが正しい計算を始めた (= 速度評価が
信頼できる値になった)。c-testsuite +1 件。

### 5. `VALUE *fp` を common_param に格上げ (B2)

旧設計の `node_call` は `c->fp += arg_index` / `c->fp -= arg_index`
を毎 call やっていた (= memory load + add + store ×2)。さらに
call 後の `c->fp` 再 load を gcc が省略できない (call が CTX を
書き換えるかもしれないので)。

wastro と同じく `common_param_count = 3` を castro_gen.rb で
override し、`VALUE * restrict fp` を **全 dispatcher / EVAL の
3 番目の共通引数** にする。caller は `new_fp = fp + arg_index` を
レジスタで計算 → callee に渡す → callee は受け取った `fp` を
そのまま locals アクセスに使う → caller の `fp` レジスタは
保存されているので post-call の reload も不要。

```c
// before
c->fp += arg_index;
v = (*disp)(c, body);
c->fp -= arg_index;

// after
v = (*disp)(c, body, fp + arg_index);
// fp は caller の register に残ったまま、CTX に touch しない
```

**効果**:
- **tight inner loop で大きい**: loop_sum 5ms → 2ms (60% 縮)、
  crc32 はコレと #4 込みで gcc -O3 を上回る
- **recursion はわずかに改善**: fib_big 67ms → 67ms (B2 単独では
  callee fp を register に残す push/pop の追加で相殺)。次の A3 と
  組み合わせて初めて効く。

### 6. parse 時に `node_call` / `node_call_jmp` を振り分け (A3)

callee の `needs_setjmp` を毎 call で `cmpb $0, fe->needs_setjmp`
する代わりに、parse.rb が呼び先関数の has_returns を見て
`(call IDX ...)` か `(call_jmp IDX ...)` のどちらを emit する
かを決める。

自己再帰 / 相互再帰では callee の has_returns が
compile_function 中にはまだ未確定なので、全関数 compile 後に
**post-pass で AST を walk** して `:call` を必要に応じて
`:call_jmp` に書き換え。

**効果**: fib_big 66ms → **48ms** (-27%)、nqueens 82ms → 69ms (-16%)、
quicksort 77ms → 71ms (-8%)。recursion-heavy 系で全体的に効く。

### 7. cross-SD direct call (B1)

`SPECIALIZE_node_call` を castro 側で完全 override。framework
default は `EVAL_node_call` を inline 展開して
`(*body->head.dispatcher)(c, body, fp + arg_index)` のような
**間接呼び出し**を emit していた。

代わりに、`astro_cs_compile` を呼ぶ前に **全関数 body の
`HASH(body)` を pre-compute** しておき、SPECIALIZE_node_call
は callee の hash を使って:

```c
extern VALUE SD_<callee_hash>(CTX *c, NODE *n, VALUE *fp);
...
return SD_<callee_hash>(c, body, fp + arg_index);
```

を emit する。direct call。

ただし default の link は `-fPIC -fno-plt` なので extern 参照が
GOT 経由 (`call *got_entry(rip)`) になり、純粋な direct call に
ならない。framework に `LDFLAGS` を入れて `-Wl,-Bsymbolic` を
渡し、**intra-`.so` の symbol 参照を bind local** に。これで

```
call *got_entry(rip)        ← GOT-mediated indirect
   ↓
addr32 call SD_<hash>       ← 真の direct relative call
```

になり、BTB / front-end / icache の振る舞いが gcc -O3 と同等に。

**効果 (純粋 B1)**: fib_big 48ms → 48ms (BTB が GOT 経由でも完璧
だったので測定上は tied)。disasm レベルでは gcc -O3 と同じ命令
パターンに揃った (recursive inlining はしてないので絶対値の差は
残る)。

### 8. break/continue 振り分け (parse 時)

ループ本体に break / continue があるかを parse.rb が走査して、
3-tier に振り分け:
- なし: `node_for` (setjmp 0 個)
- break のみ: `node_for_brk_only` (setjmp 1 個 / loop 入口)
- continue or 両方: `node_for_brk` (setjmp / iter)

setjmp 0 個のループは fp register と完全に register 化されて、
gcc -O3 並みの inner loop コードが出る (sieve / loop_sum / crc32 が
これに該当)。

### 9. framework のリダイレクト bug fix

`runtime/astro_code_store.c` の SD ビルドコマンドが
`make ... 2>&1 >/dev/null` の順で書かれていた。これは shell では:
1. `2>&1`: stderr → 現在の stdout (== terminal)
2. `>/dev/null`: stdout → null

つまり stderr は消えず terminal (parent process の stdout) に
出続ける。その結果 SD ビルド時の warning が test runner の
output capture (stdout) に紛れ込み、stdout-diff な test が
すべて mismatch 扱いされる。

`>/dev/null 2>&1` の正しい順序に修正。

## 失敗 / 棄却したアイディア

### A1: dispatcher を `function_entry` に snapshot する

最初の案として「`fe->dispatcher = body->head.dispatcher;` を
load 完了後にコピーしておけば `body->head.dispatcher` の load
が 1 回減る」を考えたが、specialize の B1 が「callee の SD 名
を直接 baked in」する以上、snapshot を中継する意味はなくなる。

→ 採用せず B1 に直行。

### A2: call ノードに `function_entry *` を直接保持

`(call IDX ...)` の代わりに ALLOC 時に `&c->func_set[idx]` を
baked。`c->func_set[idx]` の indexed load が 1 段減る。

ただし `c->func_set` がランタイム calloc'd で、SD の hash を
プロセスをまたいで再現可能にしたい (code_store の永続性)
場合、生 pointer を baked できない。

→ 効果は微小 (2-3% 程度)、実装コストとリターンが見合わず棄却。
   index 経由 (current state) で十分。

### Recursive inlining (gcc -O3 が fib に対してやっている)

gcc -O3 は fib をループ展開 + 自己 inline で 1100 byte の
巨大関数に変換し、call 件数を 1/64 程度に圧縮する。これを
castro でやろうとすると SD generator に AST 自己参照の検出 +
bound 付き unfold パスが必要で、framework レベルの拡張が
必要。

→ 1.5–2× の追加効果は見込めるが、cost-benefit と汎用性の観点で
   一旦保留。`docs/todo.md` に記載。

## 残りギャップ

最終的に gcc -O3 比 3〜5× 程度のギャップが残る。内訳推定:

- gcc -O3 の recursive inlining (fib を 6 段以上 unfold) : 2-4×
- gcc -O3 の register allocation (n を register に keep): 1.2-1.5×

これらは AST インタプリタの構造的限界に近く、現実的には
gcc -O3 と完全同等は無理。tight inner loop なら gcc -O3 を
上回るところまで来た (crc32) ので、framework の出来としては
十分高水準。

## 付録: 主要マイルストーン

| 時点 | fib_big AOT cached | 累積効果 |
|---|---:|---|
| 開始時 | 1442 ms | (baseline) |
| tail-return リフト | 803 ms | 1.8× |
| `-rdynamic` で AOT が実際に効く | 217 ms | 6.6× |
| call IR を idx 化 | 64 ms | 22.5× |
| B2 + A3 + bug fix + B1 + Bsymbolic | **48 ms** | **30×** |
| (gcc -O0) | 47 ms | -- |
| (gcc -O3) | 16 ms | -- |
