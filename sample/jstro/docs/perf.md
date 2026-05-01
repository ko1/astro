# perf.md — 性能向上の記録

jstro が現在の性能水準に至るまでに試した最適化を、効いたもの/効かなかった
ものの両方を時系列で並べる。各項目に **bench** 列で fib(35) と nbody 100k
への影響を書く (s 単位、best-of-3 のおおよその値)。

ベースの測定環境: Linux x86_64, gcc -O3、SMI fast path 入り、IC 最初期版。
比較対照: `node` (V8 JIT)。

| 改善                                      | fib   | nbody | mandelbrot |
|------------------------------------------|-------|-------|------------|
| 出発点 (-O2, BR チェック有, 単純 IC)        | 0.74  | 0.49  | 0.94       |
| -O3 へ昇格                                | 0.74  | 0.47  | 0.90       |
| arity 特化 call ノード (call0/1/2/3)       | 0.69  | 0.46  | 0.89       |
| flonum-flonum 算術 fast path              | 0.69  | 0.42  | 0.74       |
| compiler flags (no-stack-* / no-plt 等)   | 0.59  | 0.45  | 0.85       |
| call IC + body inline (`jstro_inline_call`) | 0.54  | 0.43  | 0.81       |
| flat n-statement seq (`node_seqn`)        | 0.54  | 0.41  | 0.72       |
| transition IC for member_set (`pre_shape`) | 0.54  | 0.33  | 0.74       |
| **現在**                                  | **0.52** | **0.32** | **0.73** |
| 比較: node.js                              | 0.08  | 0.02  | 0.03       |

下記、各項目を **採用 (○) / 不採用 (✗)** に分けて詳述。

---

## ○ 採用された最適化

### コンパイラフラグ

#### `-fno-stack-protector` / `-fno-stack-clash-protection`

`js_call_func_direct` の `objdump` を見ると、`alloca` の前後で stack-clash
保護のための `sub $0x1000, %rsp; orq $0x0, 0xff8(%rsp)` ループが挿入されて
いた。これはプログラムが大きな alloca で kernel guard page を踏み抜くこと
を防ぐ機構だが、jstro の frame は通常 8〜32 バイト程度なので不要。
`-fno-stack-clash-protection` で除去。同時に `-fno-stack-protector` で
canary も削減。

→ fib 0.74 → 0.61 (-18%)。最大級の単一改善。

#### `-fno-plt`

ELF の手続きリンケージテーブルを経由しない呼び出しに切り替える。
動的ライブラリ呼び出し (libc, libm) が `call ptr@plt` から `call *libc__symbol@GOT(%rip)`
に変わり、PLT スタブの **jmp + jmp** をスキップ。

→ fib 0.61 → 0.59。小さいが確実。

#### `-fno-semantic-interposition`

LD_PRELOAD で実装関数を差し替えできる ELF の互換性を捨てる。gcc がプロセス内
で関数間の **直接参照** を有効化するので、関数ポインタ越しでない直接呼び出し
が `call <local>` に最適化される。

→ fib 0.59 → 0.54 (累計)。

### 値表現側

#### CRuby 風 SMI / inline flonum

最初から採用。fib のような整数演算では heap allocate を完全に回避でき、
gcc は `(v >> 1)` を sign-extending shift 1 命令に落とす。

#### flonum-flonum / SMI-flonum 混在 fast path

mandelbrot のホットループは double 同士の乗算 / 加算が支配的。最初は
`js_to_double` を経由していたが、これは out-of-line 関数呼び出しを伴う。
`node_add` / `node_sub` / `node_mul` / `node_div` / `node_lt` 系すべてに

```c
if (JV_IS_FLONUM(a) && JV_IS_FLONUM(b))
    return JV_DBL(JV_AS_DBL(a) + JV_AS_DBL(b));
if ((JV_IS_SMI(a) && JV_IS_FLONUM(b)) || (JV_IS_FLONUM(a) && JV_IS_SMI(b))) ...
```

を追加。`JV_AS_DBL` も `always_inline` なので gcc が SSE 命令まで畳み込む。

→ mandelbrot 0.85 → 0.74 (-13%)。

### ノード設計

#### arity 特化 call ノード `node_call0/1/2/3`

汎用 `node_call` は引数を `JSTRO_NODE_ARR` (側面配列) に書き出して
`alloca` で staging するが、ほとんどの call は引数 0〜3 個。`node_call1(fn, a0)`
のように **AST node 自身に NODE * 子供として埋め込む** ことで:

- side-array indirection が消える
- alloca buffer がリニア (常に 1 引数なら register に乗る)
- ASTroGen の specialization が child を直接見られる (将来 SD 化したとき)

→ fib 0.74 → 0.69 (-7%)。

#### Monomorphic call IC (`JsCallIC`)

各 call site の AST node に `(cached_fn, cached_body, cached_nlocals)`
の 3 ワード IC を埋め込み、ヒット時に **`js_call`/`js_call_func_direct` を
完全にスキップ** して body の EVAL を直接呼ぶ:

```c
if (JV_IS_PTR(fv) && (JsFunction*)fv == ic->cached_fn) {
    return jstro_inline_call(c, ic->cached_fn, ic->cached_body,
                             ic->cached_nlocals, JV_UNDEFINED, &av0, 1);
}
```

`jstro_inline_call` は `static inline always_inline` なので
dispatcher 関数本体に展開され、frame 確保 → param copy → EVAL → 復帰
までが一つの C 関数の中で完結する。fib のような monomorphic な再帰サイト
では type check + indirect call を完全に外せる。

→ fib 0.69 → 0.54 (-22%)。

#### Transition IC for `member_set` (pre_shape フィールド)

最大の発見。nbody の `function Body(x,y,...) { this.x = x; this.y = y; ... }`
コンストラクタを 5 回呼ぶと、各 `this.x = x` の AST ノードは
**EMPTY_SHAPE → SHAPE_X** の遷移を毎回発生させる。

普通の IC は post-state shape (= SHAPE_X) を覚える。次の呼び出しは
EMPTY_SHAPE で始まるので IC ミス → `js_shape_find_slot` の線形スキャン。

そこで `JsPropIC` に `pre_shape` フィールドを足し、ミス時に
`(pre_shape, shape, slot)` を覚える。次の呼び出しでは

```c
if (o->shape == ic->shape /* in-place */) ...
else if (o->shape == ic->pre_shape) {  // transition hit
    o->shape = (JsShape*)ic->shape;
    o->slots[ic->slot] = v;
}
```

→ nbody 0.43 → 0.33 (-23%)。`js_shape_find_slot` の cycles 9% → ほぼ 0%。

これが現状最もコストパフォーマンスのよい IC 改造。

#### Flat n-statement sequence `node_seqn`

`{ a; b; c; d; e; f; }` のようなブロックは元々

```
seq(seq(seq(seq(seq(a,b),c),d),e),f)
```

の 5 段ネスト。各 seq は dispatcher 1 回 + child 2 回の indirect call なので、
ブロック先頭から末尾まで毎ステートメント間で 1 indirect 余分。

3 文以上のブロックを `node_seqn(idx, count)` という flat 配列ノードに変更。
1 つの dispatcher の中で `for` ループで子を回すので indirect call は子の
分だけ。

→ mandelbrot 0.74 → 0.72。

#### longjmp ベースの throw

最初は `JSTRO_BR != JS_BR_NORMAL` を **すべての算術ノードと property access
ノード** で `EVAL_ARG` の後にチェックしていた:

```c
JsValue a = EVAL_ARG(c, l);
if (JSTRO_UNLIKELY(JSTRO_BR != JS_BR_NORMAL)) return a;
JsValue b = EVAL_ARG(c, r);
if (JSTRO_UNLIKELY(JSTRO_BR != JS_BR_NORMAL)) return b;
```

このチェックは throw 用だったが、(throw が極めて稀なので) ほぼ全パスで
分岐予測が効くにもかかわらず、メモリ load + compare の 2 命令が常に乗る。

`throw` を **`longjmp`** に切り替えれば、実際に throw が起きたときだけ
スタックを巻き戻して try-frame に戻れる。算術ノードからチェックを完全に
削除可能。`break`/`continue`/`return` は静的に loop / function body 境界に
閉じているので、これらは引き続き `JSTRO_BR` グローバルで伝播。

→ 効果は単独では微小だが、後の最適化と相互作用 (生成コードがコンパクト)。

#### 関数宣言ホイスティング (テキスト pre-scan)

仕様準拠の修正だが、これがないと相互再帰が動かない。`{ }` ブロック / 関数
本体の入口で **トークン列ではなくソーステキスト** を depth=0 走査して
`function NAME` パターンを集める。文字列リテラル / 行コメント / ブロック
コメント / テンプレート文字列を skip しつつバイト単位で進める。

トークン化を経由せずバイト走査にしたのは、lexer が状態 (cur, next, tpl_stack 等)
を持っているので一時的に save/restore する仕組みを書くのが面倒だったため。
バイト走査でも要求 (depth-0 の `function NAME` だけ拾う) は十分に達成可能。

ホイスト名を予約してから本パースに入るので、内部の参照が `RES_LOCAL`/
`RES_BOXED`/`RES_UPVAL` のいずれかに正しく解決される。

---

## ✗ 試したが効かなかった最適化

### `-flto` (Link-Time Optimization)

理論上、関数ポインタ越しの呼び出しを直接呼び出しに promote できる場合が
ある (devirtualization)。実測ではほぼ無効果かわずかに悪化。dispatcher
の関数ポインタが「`n->head.dispatcher` を indirect 呼び出し」という形
なので、LTO でも静的に解析しきれない。

→ fib 0.69 → 0.79 (悪化)。LTO は外した。

### `-march=native`

AVX-512 のアラインメント要求が `alloca` バッファに合わず、
prologue が増えた。

→ fib 0.69 → 0.78 (悪化)。`-march=native` も外した。

### PGO (`-fprofile-generate` → `-fprofile-use`)

意外と効かなかった (2-5%)。すでに `__builtin_expect` で hot/cold が明示
されており、indirect call の予測は CPU 側で十分。バイナリサイズと
ビルド時間の悪化を考えると割に合わない。

### `EVAL` をマクロ化

`EVAL(c, n, frame)` を `((*n->head.dispatcher)(c, n, frame))` のマクロに
すれば 1 関数呼び出し分 (関数ポインタ load + indirect call) を hot path
内にインライン化できる気がした。実際は gcc が既に `EVAL` を inline していた
(static inline 化していた等)、結果はノイズの範囲。

### `__attribute__((flatten))` を全 dispatcher に

dispatcher 関数を flatten すると child の `EVAL_*` も全部 inline されて
1 つの巨大関数になる。実装量は多い。フレームワークが既に always_inline で
EVAL_* を内側に展開しているので、flatten 追加では大して動かなかった。

### computed-goto threaded interpreter

「dispatcher を関数ポインタにせず、ラベルアドレスにして goto する」典型的
なバイトコード VM 最適化。AST ベースの jstro では各ノードが構造的に独立
した「subtree-evaluating function」なので素直には適用できない。
`EVAL` 全体を 1 つの大 switch 文 にする方向もあるが、ASTroGen の生成コード
を大幅に書き換える必要がある。

### js_call_func_direct を inline 化 (常に)

`js_call_func_direct` を `static inline always_inline` にして全呼び出しを
インライン化する案。`alloca` を含む関数を inline すると caller 関数が
大きく膨れて register pressure が上がり、結果としてホットループ全体が
遅くなった。**monomorphic call IC のヒット時のみ** inline する現在の方式
(`jstro_inline_call`) が最適だった。

### `arena` allocator

文字列連結 (`js_str_concat`) や Map のエントリ拡張など、短寿命の小さな
allocation をまとめて arena に流せばメモリ確保コストを削れる。実装の
複雑性に見合うほどの効果が出なかった。GC を入れるなら本質的にこの方向に
進むので、後回し。

### Specialized comparison ノード `node_smi_lt` 等

「parser の段階で SMI 比較と確信できるなら専用ノードを emit する」型推論。
parser 側に型推論が必要 + 失敗時に general path に戻す guard が必要 +
ASTro が既に SMI fast path を inline しているので、ノードを増やしても
gcc が同じコードに落ちる。

---

## 機能拡張ラウンド後の状態

ES2023+ サブセットの大幅な追加 (destructuring, class extends/super, getter/setter,
Promise/async, regex, Proxy/Reflect, Map/Set, modules, など) を行ったが、
ベンチマーク数値はほぼ変化なし:

| benchmark         | 機能拡張前 | 機能拡張後 | 変化 |
|-------------------|-----------|-----------|------|
| fib(35)           | 0.52 s    | 0.55 s    | +6%  |
| fact ×5M          | 1.30 s    | 1.40 s    | +8%  |
| sieve(1M)         | 0.08 s    | 0.09 s    | ~    |
| mandelbrot(500)   | 0.74 s    | 0.79 s    | +7%  |
| nbody 100k        | 0.32 s    | 0.38 s    | +19% |
| binary_trees(14)  | 0.47 s    | 0.49 s    | +4%  |

nbody が一番悪化したのは `js_set_member` / `js_object_set_with_ic` に
freeze / NOT_EXTENS / accessor のチェックが入ったため。これは fast path
には乗らないが、IC 1 段目 (in-place update) の前段で `gc.flags` をチェック
する追加のロード+分岐が定常時にも入る。完全 monomorphic ループでは
ほぼ予測できるので影響は限定的。

`node_member_get` / `node_member_set` の IC fast path は HOLE / accessor
の場合に slow path に落ちるよう変更したが、通常のオブジェクト操作 (data
property 一辺倒) では追加チェック 1 件で済む。

## 振り返り

最も効いた最適化は **シェイプ遷移 IC** と **monomorphic call IC + inline body**
の 2 つで、いずれも「**多形性が低いケース** にだけ最速パスを切る」という
発想。逆に「全ケースを一律に速くしようとした」最適化 (LTO、march=native、
flatten) はあまり効かなかった。

JS のような動的言語では、ホットスポットの **type 多形性は実は低い** という
V8 の発見が裏付けられている。jstro でも 80/20 の法則がきれいに当てはまる。

次に大きな gain が見込める順は次のとおり:

1. **ASTro 特化 (SD) モードを駆動する**。luastro 既存の機構を移植すれば、
   関数本体を 1 つの C 関数に畳めて 5-10× 期待。
2. **多形性 IC** — 同じサイトで 2-3 個の shape を見ても落ちないようにする。
3. **method call IC** — 現在は `member_get` の IC + `js_call` の IC を 2 段
   に分けて持つ。`obj.foo()` の AST node に `(shape, slot, cached_fn, cached_body)`
   を 1 セットで持てば直結できる。
