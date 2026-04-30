# castro 性能最適化メモ

ASTro 上の C サブセットインタプリタとして、AOT cached モードで
gcc -O0 を超え、tight inner loop では gcc -O3 を上回るところまで
削った経緯と手法を記録します。

ベンチは `bench.rb`、`BENCH_RUNS=15` median、parse.rb 起動時間
(~110ms) を除いた純粋な実行時間。

## 最終ベンチ結果 (gcc -O0/-O1/-O3 比較)

| bench | castro AOT cached | gcc -O0 | gcc -O1 | gcc -O3 | castro / -O1 |
|---|---:|---:|---:|---:|---:|
| fib_big (fib 35)   |    **47** |   46 |   42 |   15 | 1.12× |
| fib_d              |     **4** |    3 |    2 |    1 | 2× |
| tak (18,12,6)      |     **2** |    0 |    0 |    0 | -- |
| ackermann (3,8)    |    **17** |    8 |    4 |    1 | 4× |
| loop_sum           | 🟢 **6** | 🔴 8 |    0 |    0 | -- |
| mandelbrot_count   |     **3** |    2 |    1 |    1 | 3× |
| sieve              |    **11** |    7 |    3 |    2 | 3.7× |
| nqueens            | 🟢**17** | 🔴 29|   15 |   12 | 1.13× |
| quicksort          | 🟢**56** | 🔴 80|   20 |   20 | 2.8× |
| crc32              | 🟢**20** |   46 |🟰 18 |   18 | **1.11×** |
| matmul             |     **3** |    2 |    1 |    0 | 3× |

🟢 / 🔴 は castro が gcc に勝っている / 負けている関係、🟰 はほぼタイ。

**castro が gcc -O0 を超えるケース** (3件):
- `loop_sum` — 6ms vs gcc -O0 8ms (1.3× 速い)
- `nqueens` — 17ms vs gcc -O0 29ms (1.7× 速い)
- `quicksort` — 56ms vs gcc -O0 80ms (1.4× 速い)

**castro が gcc -O1 にほぼタイ**:
- `crc32` — 20ms vs gcc -O1 18ms (1.11× だけ後ろ)

**残ギャップが構造的なケース**:
- `sieve` — 11 vs 3ms (3.7×)。inner mark loop は disasm 上 gcc -O1
  と**完全同形** (3 命令 branchless、レジスタ保持) を達成済。残ギャップは
  perf stat で見ると: cycles 4×、L1 loads **3×**、IPC 1.31 vs 3.94。
  L1 miss rate は逆に低い (14% vs 26.6%) — 実物 sieve は 1.6MB
  で最初から L1 越えてるので両者とも L2/L3 アクセスし、**castro が
  3× 多くの memory operation を発行している**ことが本質。inner loop
  自体は最適でも、outer mark loop の `i*i` 計算や count loop の
  `prime[i]` read 等で SD chain 経由の load が散発的に gcc より多く出る。
  詳細は §13。

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

### 11. leaf-helper インライン化 (`node_call_static`)

非自己再帰の呼び出しを framework のナチュラル specializer 経由で
インライン化する道を castro 側で開通させた。

**仕掛け**: 呼び先 body NODE * を子オペランドとして持つ
`node_call_static(NODE *callee, ...)` を新設。framework は NODE *
operand を「子」と見て SPECIALIZE が再帰的に walk → callee body の
SD chain を caller と同じ TU に `static inline` で展開 → gcc -O3 が
inline する (gcc -O1 で `-finline-small-functions` が host source 上で
やってる事の AST 評価器版)。

**parse 時の振り分け**:
- 自己 / 相互再帰の SCC に属する callee → 旧 `:call` (uint32_t func_idx
  + extern direct call SD)。`:call_static` を recursive callee に当てる
  と body 内の `:call` が emit する `extern SD_<self>` 宣言と framework
  が同 TU に出す `static inline SD_<self>` def が衝突する。
- それ以外 (= leaf 群) → `:call_static`。

実装:
- 全 call を optimistic に `:call_static` で emit
- compile 完了後に Tarjan SCC を call graph に走らせ、recursive function
  集合を計算
- recursive 集合に含まれる callee への `:call_static` を `:call` に
  ダウングレード

**body NODE * の patch**: parse 時には callee body はまだ build されて
いないこと多々 (前方参照)。`node_call_static.callee` は phase-2 SX
load で NULL のまま ALLOC、main.c の `call_patch_record` で
(NODE, func_idx) を side-table、phase-3 で
`callee = c->func_bodies[idx]` を書き戻し。

**効果**:
- bench は baseline とほぼ同等。fib_d AOT cached が 9 → 4ms (2×) に
  下がった以外は誤差レベル (±1ms)。
- 期待していた nqueens の `safe()` インライン化は **bench 上で見えず**。
  実装上は `solve` の TU 内に safe の SD chain が `static inline` で
  展開され、disasm でも inline 済みなことは確認できる。が hot path 全体
  の中で safe の call overhead 比率が想定より低かった模様。
- 「機構を入れた」コミット。leaf 大量の benchmark 追加時に効く可能性は
  残してある。

### 12. typed pointer による TBAA 改善

`node_load_*` / `node_store_*` の実装を `VALUE *` 経由から
typed pointer (`int64_t *` / `double *` / `void **`) 経由に切替。

**前**:
```c
node_store_i(...) {
    void *pv = UNWRAP(EVAL_ARG(c, p)).p;
    int64_t vv = UNWRAP(EVAL_ARG(c, v)).i;
    ((VALUE *)pv)->i = vv;
}
```

**後**:
```c
node_store_i(...) {
    int64_t *ip = (int64_t *)UNWRAP(EVAL_ARG(c, p)).p;
    int64_t vv = UNWRAP(EVAL_ARG(c, v)).i;
    *ip = vv;
}
```

VALUE union 経由だと gcc の TBAA は他の VALUE * (= fp[]) と
aliasing 可能性ありと判定してしまう。typed pointer 化すると
strict-aliasing で「`int64_t *` への store は `VALUE *` の任意
member とは alias しない」と判定でき、`prime[j] = 0` のような
global store の後に `fp[2]` を再 load しなくて済むようになる
ケースが増える。

**効果**: quicksort 80 → 58ms (-27%)。sieve / loop_sum は変わらず
(下記 §13 で別の理由で停滞)。

### 13. caller-allocated stack VLA frame (wastro パターン採用)

最大の構造変化。`node_call` / `node_call_static` が呼び出し時に
**callee 用の frame を caller stack 上の VLA `VALUE F[local_cnt]`
として確保**するよう変更。それまで callee の frame は呼び出し元の
heap frame (`fp + arg_index`) を使い回していた。

```c
// 前: heap frame 流用
node_call(... uint32_t func_idx, ...) {
    NODE *body = c->func_bodies[func_idx];
    RESULT r = (*body->head.dispatcher)(c, body, fp + arg_index);
    return RESULT_OK(r.value);
}

// 後: 呼び出し時に stack VLA を確保
node_call(... uint32_t func_idx, uint32_t local_cnt, ...) {
    NODE *body = c->func_bodies[func_idx];
    VALUE F[local_cnt];
    for (i = 0; i < arg_count; i++) F[i] = fp[arg_index + i];
    for (; i < local_cnt; i++) F[i].i = 0;
    RESULT r = (*body->head.dispatcher)(c, body, F);
    return RESULT_OK(r.value);
}
```

これは wastro が前から使ってる pattern (wastro `node.def:1672`)。
SD chain がフルで `static inline always_inline` になっている時、
`&F` は escape しないので **gcc の SROA が F[i] を個別スカラに
分解 → register に昇格** する。

そのまま `VALUE F[local_cnt]` (VLA) だと gcc の SROA は VLA
扱いを保守的にして部分的にしか走らないので、`SPECIALIZE_node_call` /
`SPECIALIZE_node_call_static` の override で **`local_cnt` を baked
literal として `VALUE F[6]` の形に展開**するようにした (override
は castro_gen.rb)。固定サイズ配列だと SROA がフルで走り、loop
induction variable まで register 化される。

**実装**:
- `local_cnt` を call ノードの operand に追加
- parse.rb は post-pass で callee の `fn.max_local` を patch (compile
  中は値が確定しないため)。`Func` クラスに `max_local` の高水位線
  追跡を追加 (arg staging で `next_local` を一時的に bump するので、
  bump 後の最大値が真のフレームサイズ)
- main.c の SX loader が `(call FUNC_IDX nargs argidx local_cnt)` を
  読む

**効果** (sieve の inner mark loop disasm で確認):

```
# Before VLA:
1290: movq $0, (%rcx,%rax,8)   ; prime[j] = 0
1298: mov 0x10(%rdx),%rax       ; i = fp[2]      ← reload
129c: add 0x18(%rdx),%rax       ; j_old = fp[3]  ← reload
12a0: mov %rax, 0x18(%rdx)      ; fp[3] = j (new)
12a4: cmp (%rdx),%rax            ; n = fp[0]      ← reload
12a7: jl 1290

# After VLA + literal-baked size (override):
1170: movq $0, (%rsi,%rax,8)   ; prime[j] = 0
1178: add %rcx, %rax              ; j += i  (i in %rcx register!)
117b: cmp %rdi, %rax              ; j < n   (n in %rdi register!)
117e: jl 1170
```

**5 命令 / 4 メモリ → 3 命令 / 0 メモリ**。`i` と `n` が完全に
レジスタ化され、`j` も memory writeback が消えた (SROA + DSE)。
これは gcc -O1 と完全に同形のコード。

bench:
- sieve: 13 → 11ms
- quicksort: 73 → 55ms (-25%)
- nqueens: 24 → 17ms (-29%)
- crc32: 22 → 20ms (gcc -O1 とタイ)
- loop_sum / nqueens / quicksort で gcc -O0 越え

### 14. AVX-vectorized array fill helper

parse.rb の peephole として
```c
for (int i = S; i < E; i++) array[i] = K;  // K は int 定数
```
を 1 つの `node_array_fill_i(base, S, E, K)` に畳み込み、`-O3 -O3
__restrict__` で別途コンパイルされた `castro_fill_i` という外部
ヘルパー関数を呼ぶ。gcc は単独関数の自動ベクトル化に強いので、
AVX-256 の `movups` (2 int64 stores per inst) + 4× unroll に
最適化される。

inlined SD chain だと restrict 情報が保守化されてベクトル化が
trigger しないが、外部関数化することで gcc の自動ベクトル化を
発火させられる。

sieve の `for (i = 0; i < n; i++) prime[i] = 1;` がターゲット。
sieve は init phase が全体の ~25% なので体感は誤差レベル
(11ms 維持) だが、機構自体は将来効くベンチマーク用に保存。

### 15. 試したが効かなかった: count loop の branchless 化

`if (prime[i]) count++;` の sieve count loop で gcc -O1 は
`cmpl $1, ...; sbb $-1, %edx` の branchless 4 命令に
最適化する。これを castro でも実現すべく parse.rb に
`if (cond) lset N (add N K) else nop` → `lset N (add N
(neq_i cond 0) * K)` の rewrite を peephole で入れた。

結果: gcc が**完全に同じ sbb 形に最適化してくれる** (`cmpq $1,
...; sbb $-1, %rcx` の 4 命令ループ)。期待どおりの命令列。

**しかし bench は逆に遅くなった** (sieve 11 → 14ms、L1-fit case
で 70 → 110ms)。

**理由**: `cmpq mem` (L1/L2 load) → `sbb $-1, %rcx` の
チェーンが毎 iter loop-carry で、`%rcx` が iter 間で読み書きされる。
load latency が loop の臨界経路に乗る。

branchy 版 (`if (...) inc count`) は sparse な prime 分布 (90%
non-prime) で `count` 更新が稀 → loop carry が短い → OoO エンジンが
iter N+1 の `cmpq` load を iter N 終了前に発行できる。

教訓: **branchless は always 速いわけではない**。loop carry が
重要なベンチでは sparse 分岐 + 良い branch predictor のほうが OoO
を効かせやすい。disasm が gcc -O1 と完全同形でも、ベンチ実測でしか
確認できないことがある。

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
