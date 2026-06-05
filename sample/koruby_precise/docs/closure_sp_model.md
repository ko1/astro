# koruby_precise 解説書 — 値スタック・closure(block/proc/lambda)・sp の全体像

対象: `sample/koruby_precise`。precise(moving copy)GC + 値スタック上で実行する
tree-walking インタプリタ。本書は **「値スタックと frame」「sp の 2 役割」「block/proc/
lambda の表現」「yield と Proc#call」「env の stack→heap 退避」「local の baking」「GC
contract」** を、実ソースと図で解説する。最終章で **「sp を引数 thread して `c->sp_top`
書き込みを `korb_alloc` だけにする」設計** がなぜ dispatch で難しいかを示す。

---

## 0. 地図(登場人物)

**3 つは別々の構造体**。`─→` は「ポインタ(その先を指す)」の意味。

```
struct CTX (c) ── インタプリタの実行コンテキスト(1 個)
 ├─ stack_base ──→ 値スタック(VALUE の線形配列, mmap)の下端
 ├─ sp_top         … GC scan の上端(high-water)。korb_alloc が alloc 直前に書く
 ├─ stack_end      … 値スタックの限界
 ├─ current_frame ──→ struct korb_frame(下)      … 今実行中の frame
 └─ current_block ──→ struct korb_proc(下) or NULL … 今の method に渡された block
```

```
struct korb_frame ── 「メソッド呼び出し 1 回分」の情報(値スタック上に積まれる)
 ├─ self                  … レシーバ(その frame での self)
 ├─ fp ──→ 値スタック上の locals 領域  (locals は fp[0], fp[1], … fp[locals_cnt-1])
 ├─ cref / current_class  … 定数解決・定義先 class の lexical scope
 ├─ locals_cnt
 ├─ block ──→ korb_proc   … この method に渡された block
 └─ last_line / last_match … $_ / $~
```

```
struct korb_proc ── block / Proc / lambda を表す(3 つとも この 1 つの型)
 ├─ body                  … 本体 AST(NODE*)
 ├─ env ──→ 捕捉した locals … block なら「定義元 method の fp」を指す(§3, §4)
 ├─ env_size              … env がカバーする slot 数(method locals + block locals)
 ├─ param_base            … env 内で block の引数が始まる slot 番号
 ├─ params_cnt / rest_slot / kwh_save_slot / block_slot …
 ├─ self
 ├─ is_lambda             … true なら lambda(§3)
 ├─ creates_proc          … body が proc/lambda を生むか(§6)
 ├─ enclosing_block       … 自分の body 内 yield の飛び先
 ├─ enclosing_frame_id    … 定義元 method が return 済みかの検出用
 └─ cref
```

2 系統の GC root:
1. **値スタック** `[stack_base, sp_top)` — staging slot を線形 scan
2. **frame chain** `current_frame->{self,fp,…}` — frame ごとの root

---

## 1. 値スタックと frame

各 method/block body は値スタック上に **frame** を持つ。`fp`(frame pointer)が locals の
先頭、locals は `fp[0..locals_cnt)`。その上(`fp+locals_cnt` 以降)が **部分式 staging**
領域(式評価の一時値・次の呼び出しの引数積み)。

```
値スタック(下=stack_base, 上=sp_top 方向)

  stack_base
  │
  ▼
  ┌────────────── method A の frame ──────────────┐┌─ A の staging ─┐
  │ fpA[0] fpA[1] … fpA[localsA-1]                ││ x  y  …        │
  └───────────────────────────────────────────────┘└────────────────┘
                                                     ▲
                                          ここが「A の sp」(= fpA + localsA)
                                          = A が次の呼び出しで引数を積む起点
```

**呼び出し規約(cfunc)**: 呼ぶ側が `sp[0]=recv, sp[1..argc]=args` を積み、
`func(c, argc, sp + 1 + argc)` で呼ぶ。呼ばれた cfunc は自分の引数を
**負 index** `sp[-argc-1 .. -1]`(self は `sp[-argc-1]`)で見る。

```
   caller の sp ─┐
                 ▼
  … caller live … [recv][arg0][arg1] │  ← callee はここ(sp+1+argc)を受け取る
                  └─ callee から見て sp[-3] sp[-2] sp[-1] ─┘
```

---

## 2. sp の 2 つの役割 — ここが全ての難所

`sp_transition_analysis.md` が言う通り、`sp` には **別物の 2 役** がある。

| | 役割 A: **body sp**(frame top) | 役割 B: **GC high-water** |
|---|---|---|
| 値 | `frame_base + scope_size` | live data 全体の上端 |
| 用途 | locals(`sp[負]`)+ 自 body の部分式 staging | 新 callee を積む場所 / GC scan 上端 |
| 持ち方 | **threaded 引数**(node body が受け取る) | **field `c->sp_top`** |

**method body / top-level では A == B**(上に live data 無し)。
**block body では A < B**(後述。block は iterator の frame より下で走る)。

```
   method body (A == B):

   [ method frame ][ staging ]
                    ▲
              sp == c->sp_top   ← 一致

   block body (A != B):

   [ method(closure)env ][ env staging ][ iterator の frame/staging ]
    ▲                     ▲                                          ▲
    blk->env              sp = env+env_size (役割A)         c->sp_top (役割B, 上)
                          └──── ここが iterator の下 ────────────────┘
```

この **A < B の divergence が「block を呼ぶと dispatch が壊れる」現象の根**。

---

## 3. korb_proc — block / proc / lambda の統一表現

Ruby では block(非オブジェクト)・Proc・lambda は別概念だが、koruby は **全部
`struct korb_proc`** で表す(object.h:225)。

```c
struct korb_proc {
    struct RBasic basic;        // T_PROC タグ + klass(Proc クラス)
    struct Node *body;          // 本体 AST(NULL = Symbol#to_proc 等の shim)
    uint64_t enclosing_frame_id;// 定義元 method の frame ID(escape 検出用)
    VALUE *env;                 // ★捕捉した locals(= 定義元 method の fp)
    uint32_t env_size;          // env がカバーする総 slot(method locals + block locals)
    uint32_t params_cnt;        // 位置引数の総数(required+optional)
    uint32_t opt_cnt;           // うち optional 数
    uint32_t param_base;        // ★env 内で block params が始まる絶対 slot
    int rest_slot, kwh_save_slot, block_slot;  // env 内の絶対 slot(or -1)
    uint32_t post_cnt;          // *rest の後の required(def f(*r,b,c))
    struct korb_proc *enclosing_block;  // 自 body 内 yield の飛び先(lexical)
    VALUE self;
    bool is_lambda;             // ★lambda 判別
    bool implicit_rest;         // `|a,|` 用 arity flag
    bool creates_proc;          // ★body が proc/lambda を生む → yield で fresh-env
    struct korb_cref *cref;     // 定数解決の lexical scope
    …
};
```

**区別の付け方:**

| 概念 | 表現 | 見分け方 |
|---|---|---|
| **block**(`m { }` / `do…end`) | `korb_proc`。`c->current_block` に置かれる。`env` = **生きている呼出元 fp** を指す(in-place 共有) | `c->current_block` 経由でアクセス。heap には出ない限り stack-env |
| **Proc**(`proc{}`/`Proc.new`/`&blk` 捕捉) | `korb_proc`(T_PROC heap obj)。`is_lambda=false` | `BUILTIN_TYPE(v)==T_PROC && !is_lambda` |
| **lambda**(`lambda{}`/`->(){}`) | `korb_proc`(T_PROC heap obj)。`is_lambda=true` | `is_lambda==true` |

→ **block と Proc の本質差は「型」ではなく「lifetime(env がどこを指すか)」**:
- **block**: 呼出元の **生きた stack frame** を `env` に共有(copy しない、速い)。method が
  return すると env(stack slot)は無効になる。
- **Proc**: escape し得るので、method が return する**前に env を heap へ snapshot**(§5)。

`lambda` 生成は flag を立てるだけ(node.def:1178 `((struct korb_proc *)p)->is_lambda = true;`)。
arity 厳格化と `return` の意味(lambda 内 return は lambda から、proc/block 内 return は
enclosing method から)に効く。

block literal の生成は **その時の fp を env として捕捉するだけ**:
```c
// node.def:1052  node_block_literal
VALUE p = korb_proc_new_with_cref(c, c->sp_top, body,
            c->current_frame->fp,        // ★env = 今の method の fp(参照, copy 無し)
            env_size, params_cnt, param_base, c->current_frame->self, …);
```
```c
// object.c:2273  korb_proc_new — env はポインタを持つだけ
p->env = fp;                 // ★stack を指す(まだ heap ではない)
p->enclosing_block = c->current_block;
koruby_register_libc_obj(&p->basic);
```

---

## 4. block の実行 = yield(in-place env 共有)

`yield` は `c->current_block`(= 渡された block の korb_proc)を **その場の env で実行**する。

```c
// object.h:988  korb_yield(fast path)
static inline RESULT korb_yield(CTX *c, VALUE *sp, uint32_t argc, VALUE *argv) {
    c->sp_top = sp;                         // ★iterator の top を publish(役割B)
    struct korb_proc *blk = c->current_block;
    …
    VALUE *bfp = blk->env;                   // ★block の env(= 定義元 method の fp)
    bfp[blk->param_base] = arg;              // ★block param をその場の env に直接書く
    c->current_frame->self = blk->self;
    c->current_frame->fp   = bfp;            // ★fp を block の env に差し替え(in-place)
    c->current_block = blk->enclosing_block; // 自 body 内 yield は enclosing へ
    _br = EVAL(c, blk->body, bfp + blk->env_size);   // ★body の sp = env + env_size
    …
}
```

図(`[1,2,3].each { |x| … }` の yield 中):

```
  stack_base
  ▼
  [ each を呼んだ method の frame ]
  [ blk->env = その method の fp ───────────┐  ← block の locals はここに同居
  │   env[0..]=method locals  env[param_base..]=block params/locals │
  └──────────────────────────────────────────┘
   ▲                                          ▲
   bfp = blk->env                  body の sp = bfp + env_size(役割A)
  [ each(cfunc) の frame/staging … ]          ← iterator は ここで上に積む
   ▲
   c->sp_top(役割B, iterator の top, body sp より上)
```

**body の sp(`bfp+env_size`)は `c->sp_top` より下**。それでも安全な理由:
- iterator(`each`)の self/locals は **frame chain**(`current_frame->self/fp`)で root 化
  され、値スタック range とは別に scan される(§8)。block が alloc で `c->sp_top` を下げても
  iterator の self は生存。
- iterator は yield 中、自分の sp より上に「値スタック経由でしか届かない live 値」を
  置かない規約(per-iter 値は C-local 再読み、self は `sp[-argc-1]` で frame chain 管理)。

---

## 5. Proc の実行 = Proc#call(escape 対応, fresh frame + env 退避)

block が `&blk`/`proc{}` で **オブジェクト化**されると、定義元 method が return した後も
呼ばれ得る(escape)。よって:

### 5a. method return 時に env を stack→heap へ snapshot
```c
// object.c:2227  method/class/singleton の戻りで呼ばれる
void korb_proc_snapshot_env_if_in_frame(VALUE v, VALUE *fp_lo, VALUE *fp_hi) {
    // v(または v の ivar)が T_PROC で、その env が [fp_lo, fp_hi] の stack 範囲を
    // 指していたら、env を heap array に copy して p->env を付け替える。
    // → method の stack slot が再利用されても Proc の captured locals が生き残る。
}
// node.def:842/911/973  method/class body の戻りで:
korb_proc_snapshot_env_if_in_frame(fr.self, fp_lo, fp_hi + 1024);
```
これが **block(stack-env)→ Proc(heap-env)への昇格**。`FL_HAS_PROC_IVARS` flag で
「proc を ivar に持つ class」だけ walk(最適化)。

### 5b. Proc#call は fresh frame を積んで env を(必要なら)clone
```c
// builtins/proc.c:57  proc_call
RESULT proc_call(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    struct korb_proc *p = (struct korb_proc *)sp[-argc-1];
    // lambda なら arity 厳格 check(§3)
    VALUE *new_fp = p->env;                  // 既定は captured env を直接使う
    //  ── slot 衝突 / method-overlap / fiber 跨ぎ なら fresh env に clone:
    bool method_overlaps_env = (prev_fp && prev_fp != new_fp && prev_fp > new_fp);
    bool env_outside_stack   = (new_fp < c->stack_base || new_fp >= c->stack_end);
    // clone する場合: c->sp_top の上に env_size+SLACK を確保 → memcpy →
    //   body 実行後、closure-captured slot [0, param_base) を元 env へ writeback
    //   (block 内の `r = …` が外側 scope に伝わるように)
    …
}
```

**block(yield)と Proc(call)の実行差まとめ:**

| | block / yield | Proc#call |
|---|---|---|
| env | 生きた stack(in-place 共有) | heap or stack。衝突時 clone+writeback |
| frame | 既存 frame の fp を差し替え | fresh frame を push |
| arity | 緩い(autosplat) | lambda は厳格、proc は緩い |
| `return` | 非局所(enclosing method へ) | lambda は局所、proc は非局所 |
| sp 位置 | `env+env_size`(< high-water) | fresh = c->sp_top の上(== high-water) |

---

## 6. env を clone する 2 ケース(in-place が壊れる時)

```c
// object.c:2371- korb_yield_slow
bool fresh_env_path = blk->creates_proc;            // (1)
bool method_overlaps_env_y =
     (c->current_frame->fp && c->current_frame->fp != blk->env
      && c->current_frame->fp > blk->env);          // (2)
if (method_overlaps_env_y) fresh_env_path = true;
enum { FRESH_ENV_SLACK = 512 };                     // clone 先の余裕(body 内 method 呼び frame 用)
```

1. **`creates_proc`**(block body 内で `proc{}`/lambda を生む): 各 iteration が
   **独立した env** を要する。共有のままだと `(1..3).each { |i| procs << proc { i } }` で
   全 proc が同じ env memory を alias し、最後の i しか見えない。→ iteration ごとに
   fresh env を clone し、戻りで closure slot を writeback。
2. **method-overlaps-env**(yield する method の frame が block の env より**上**=深い):
   `def my_fi(&blk); obj.each { blk.call }; end` で、`each` は `my_fi` の上で走るが
   inner block の env は `my_fi` の fp。in-place 実行すると active method の locals を
   破壊する。→ fresh env に clone して virgin memory で走らせる。

---

## 7. local 変数の baking(parse 時に sp 相対へ焼く)

local アクセスは **bake walker** が parse 時に **`idx - scope_size`(負の sp offset)** へ
焼く(node.c:91-98)。

```
// node.c:91
// `index` は patched by koruby_bake_sp_offsets:
//   negative → sp-relative offset (= original_idx - scope_size).
//   `sp` は frame TOP(= fp + scope_size)を指すので sp[off] が fp[index] を読む。
```

- method body: `scope_size = locals_cnt`。`sp[idx - locals_cnt] = fp[idx]`。
- block body: `scope_size = env_size`。`sp[idx - env_size] = env[idx]`。
  env は method locals + block locals を連続で持つので、**captured method-local も
  block-local も同じ `sp[負]` で一様アクセス**(koruby closure の肝)。

```
   block body の sp(= env + env_size)
   ▼
   [ env[0] … env[param_base-1] │ env[param_base] … env[env_size-1] ]
     └─ 外側 method の locals ──┘ └─ block の params + 自前 locals ─┘
     sp[-env_size] … sp[-(env_size-param_base+1)]   sp[-(env_size-param_base)] … sp[-1]
```

---

## 8. GC contract — なぜ `c->sp_top` が要るのか

precise moving GC は **alloc ごと(STRESS 時)に発火**し、root を 2 系統で scan する:

```c
// koruby_runtime.c:236  値スタックの線形 scan(c->sp_top を memory から読む)
if (c->stack_base && c->sp_top)
    for (VALUE *p = c->stack_base; p < c->sp_top; p++)
        visit_value_slot(ctx, fn, p);
// その後 frame chain: current_frame->self / fp / … を別途 scan
```

alloc helper は **alloc 直前に sp を publish**:
```c
// object.c:195  korb_alloc — ★これが c->sp_top write の「正当な例外」
static inline VALUE korb_alloc(CTX *c, VALUE *sp, size_t size) {
    c->sp_top = sp;                  // threaded sp を GC が読める memory へ
    return aro_gc_alloc(c, size);    // ここで GC が起こり得る → [base, sp) を scan
}
```

→ **GC は非同期(alloc 内で発火)で、現在の top を memory から読む**。threaded register の
`sp` は GC から見えない。だから **alloc 直前に sp を memory へ publish する**必要があり、
これが `korb_alloc` の `c->sp_top = sp`。**この 1 箇所だけが正当**(「GC を起こす直前の保存」)。

---

## 9. 設計目標と dispatch が難所な理由

### 目標
**`c->sp_top` への write を `korb_alloc` の 1 箇所だけにする。** それ以外の write
(frame setup の publish, sp-less helper 周りの park, dispatch bridge, restore)は全廃。
方法は「**渡ってきた sp が top。`sp[0..]` に積み、次の関数へ `sp+used` を渡す。locals は
`sp[-parsed_frame_idx + local_idx]`**」。

### builtins は達成済み(808/928 撤去)
leaf cfunc は sp を引数で受け、alloc helper に渡すだけ。`korb_alloc` が publish するので
明示 `c->sp_top=` は不要になった。

### dispatch(method 呼び出し)が壊れる理由 = §2 の A != B
`korb_dispatch_to_method` の AST 枝は、新しい callee の frame を **`c->sp_top`(役割B,
high-water)の上**に積む:
```c
// object.c(現状)
VALUE *prev_sp = c->sp_top;            // read B
VALUE *new_fp  = c->sp_top + 1;        // read B  ← callee frame を high-water の上に
{ for (p=c->sp_top; p<new_sp; p++) *p=Qnil;  c->sp_top = new_sp; }   // ★frame publish(write)
… EVAL(body, new_fp + locals) …
c->sp_top = prev_sp;                   // ★restore(write)
```

この **read B(`new_fp = c->sp_top+1`)を「渡ってきた sp(役割A)」に置換すると、block
body から呼んだ時に壊れる**:
- block body の sp(役割A)= `env+env_size` は **iterator の high-water より下**。
- そこに callee frame を積むと、**iterator が値スタック経由で握る live staging を上書き**
  し、GC が `[base, 下げた sp)` しか見ず root を見失う(= test_alias/test_class が落ちた現象)。

```
   ✗ 失敗版: callee を node-sp(役割A, 下)の上に積む

   [ block env ][ block staging │ callee frame ←ここ ][ iterator の live staging ←潰す! ]
                                                        ▲ ここが scan されなくなる
```

### では read B も消すには(§ user 設計)
**役割 B(high-water)を `c->sp_top` field から「引数 thread」へ**移す必要がある。
high-water は「直近 alloc/yield で publish された sp」=「最内 cfunc の staging top」。
これを **yield/proc_call/dispatch に引数で運べば** read B も消え、`c->sp_top` は
`korb_alloc` の write 1 つだけになる。

ただし block の closure は §4 の通り **「method の env を in-place 共有, sp = env+env_size」**
なので、ここで:
- **block 自前 locals は `sp[-parsed_frame_idx + idx]`(top のすぐ下)**、
- **captured method-locals は `env`(blk->env)経由**

に分離するか、あるいは **役割 A(frame sp)と役割 B(high-water)を 2 値で thread** するか、
という **設計判断が keystone**。ここを決めないと §9「失敗版」を繰り返す。

### 進め方(案)
1. **block body の sp を「真の top」にする**(iterator が上を空ける規約 + frame-chain root を
   使い、yield が high-water を引数で運ぶ)。closure 変数アクセスを env 経由に分離。
2. dispatch の frame base を read B でなく **引数 high-water** にする。
3. sp-less helper(`korb_const_get`/`korb_ivar_set`/`korb_inspect_inner` 等)を sp 化し
   park を撤去。
4. 結果、`c->sp_top` write は `korb_alloc` だけになる。

各段で `tools/gc_harness.sh all`(DEFAULT 28/28, STRESS 27/28=test_fiber flake)を維持。

---

## 10. 目標 — 「sp 一本」モデル(現状 → 理想)

### 10.1 不変条件(理想)
1. 関数に渡る **`sp` = その文脈の value-stack top**(= 自分が積んだ分まで含む上端)。
2. `sp[0], sp[1], …` に自由に積む。**次の関数へは `sp + (使った数)`** を渡す
   (`sp[3]` まで書いたら `sp + 4`)。
3. **locals は `sp[-scope_size + local_idx]`**(scope_size = parse 時確定。現 bake walker と同じ)。
4. **`fp` 直読みは廃止**。frame は「底 `env`(= `sp - scope_size`)」と chain メタ
   (self/cref/method/last_line…)だけ持つ。
5. **`c->sp_top` write は `korb_alloc` の 1 行だけ**(GC を起こす直前の publish)。read は
   GC scan(`koruby_runtime.c:236`)だけ。**「global high-water」概念は消滅**し、各文脈が
   自分の `sp` を持つ。

### 10.2 現状との差(何を書き換えるか)

| 項目 | 現状 | 目標(sp 一本) |
|---|---|---|
| body 内 local | `sp[idx-scope_size]`(=`fp[idx]`) | 同左(不変) |
| 引数 scratch / 子 frame 配置 | `fp[arg_index+i]` / `fp+arg_index` | **`sp[arg_index+i-scope_size]` / `sp+(arg_index-scope_size)`** |
| cfunc-dispatch staging | `VALUE *sp = c->sp_top`(global high-water) | **渡ってきた `sp`(top)を使う** |
| frame setup の高さ | `new_fp = c->sp_top + 1`、`c->sp_top = new_sp` | **`new_fp = sp(+offset)`、publish 無し** |
| sp-less helper(const_get/ivar_set/inspect_inner) | 呼ぶ前に `c->sp_top = sp+N`(park) | **helper を `(…, VALUE *sp, …)` 化、park 撤去** |
| frame の record | `fp`(底) | `env`(底)= `sp - scope_size`(closure 捕捉用にだけ残す) |
| `c->sp_top` writer | 多数(frame setup/park/bridge/restore) | **`korb_alloc` だけ** |

### 10.3 各経路の理想形

**method dispatch:**
```
caller body(sp = top):
  各引数を EVAL → sp[0..argc) に積む(self は別途、argv = sp[0..])
  callee: 子 frame の底 = sp、body は sp + scope_size で実行
          → 子 locals = (sp+scope_size)[-scope_size+idx] = sp[idx]
             (= 積んだ引数がそのまま子の最初の locals)
  ※ c->sp_top read も write も無し。alloc は callee 側 korb_alloc が publish。
```

**block / yield:**
```
iterator cfunc(sp = 自分の top):
  korb_yield(c, sp, args)           ← sp は korb_yield 内の alloc publish 用
    block body の sp = blk->env + blk->env_size(= block 文脈の top)
    block locals = sp[-env_size + idx] = env[idx]
    block の alloc は korb_alloc(c, sp)  ← c->sp_top = env+env_size に下がるが
       iterator の self は frame chain で守られ、上の領域は dead(copy 済み引数)→ 安全
```

**closure 捕捉(block literal):**
```
blk->env = sp - scope_size          ← 「底」を保存(現状の fp と同じ場所、表現だけ env に)
```

### 10.4 難所と移行手順
最大の壁は §9 の「block から呼んだ dispatch」:現状 dispatch は callee を
`c->sp_top`(high-water)の上に積むが、これを **渡ってきた sp** に置換すると、**値スタック上に
live staging を残したまま dispatch する例外パス**(class/alias body 評価中の定義 dispatch 等)で
上書きが起きる(test_alias/test_class が落ちた現象)。

手順(各段 `tools/gc_harness.sh all` で DEFAULT 28/28 維持):
1. **「dispatch 時点で値スタック上に live staging を残す」パスを洗い出し**、staging を
   `sp[0..]`(自分の top)に寄せる(主に cfunc-dispatch と class/alias body)。
2. **dispatch の frame base を `c->sp_top` read → 渡ってきた `sp`** に置換。frame setup の
   `c->sp_top = new_sp` publish と restore を撤去(alloc が publish)。
3. **`fp[arg_index]` 系を `sp[arg_index-scope_size]` に置換**、`fp` field を `env`(closure 捕捉用)に純化。
4. **sp-less helper を sp 化**(const_get/ivar_set/inspect_inner/glob_walk …)、park 撤去。
5. 結果、**`c->sp_top` write は `korb_alloc` だけ**になる。

→ 1 を先にやらないと 2 で必ず壊れる(3 度の失敗の教訓)。

---

## 付録: 主要 source 索引

| 概念 | 場所 |
|---|---|
| 値スタック scan(GC contract) | `koruby_runtime.c:236` |
| `korb_alloc`(唯一の正当 write) | `object.c:195` |
| `struct korb_proc` | `object.h:225` |
| `struct korb_frame` | `context.h:440` |
| block literal 生成 | `node.def:1050` `node_block_literal` |
| `korb_proc_new`(env=fp 捕捉) | `object.c:2273` |
| yield fast | `object.h:988` `korb_yield` |
| yield slow(env clone) | `object.c:2371` `korb_yield_slow` |
| Proc#call(fresh frame) | `builtins/proc.c:57` `proc_call` |
| env stack→heap snapshot | `object.c:2227` `korb_proc_snapshot_env_if_in_frame` |
| local baking(idx-scope_size) | `node.c:91` |
| dispatch AST frame setup | `object.c:4401` `korb_dispatch_to_method` |
| sp 役割分析(前回監査) | `docs/sp_transition_analysis.md` |
