# astocaml ランタイム解説

このドキュメントは astocaml が **どのように OCaml プログラムを動作させているか** をやや詳細に説明する。
特に「関数とメソッド呼び出し周り」 (環境キャプチャ・適用・末尾呼び出し・partial application・例外) に焦点を当てる。

## 1. 値表現 (`VALUE`)

```c
typedef int64_t VALUE;
```

最下位 1 bit でタグ付け:

| ビットパターン | 意味 |
|--|--|
| `xxxx_xxx1` | 62-bit 整数 (`OC_INT_VAL(v) = v >> 1`) |
| `xxxx_xxx0` | 8-byte 整列ヒープポインタ または シングルトン |

シングルトンはグローバル `static struct oobj` のアドレス: `OC_UNIT_OBJ`, `OC_TRUE_OBJ`, `OC_FALSE_OBJ`, `OC_NIL_OBJ`。
これらは `bit 0 == 0` で 8-byte aligned アドレスなので heap pointer と区別不要。

### ヒープオブジェクト

すべて単一構造 `struct oobj` の union 表現:

```c
struct oobj {
    int type;            // OOBJ_CONS / OOBJ_STRING / ...
    union {
        bool b;
        double dbl;                                   // OOBJ_FLOAT
        VALUE refval;                                 // OOBJ_REF
        struct { VALUE head, tail; } cons;
        struct { char *chars; size_t len; } str;
        struct {                                      // OOBJ_CLOSURE
            struct Node *body;
            struct oframe *env;
            int nparams;
        } closure;
        struct {                                      // OOBJ_PRIM
            oc_prim_fn fn;
            const char *name;
            int min_argc, max_argc;
        } prim;
        struct { int n; VALUE *items; } tup;          // OOBJ_TUPLE
        struct { int n; VALUE *items; } arr;          // OOBJ_ARRAY
        struct { const char *name; int n; VALUE *items; } var;       // OOBJ_VARIANT
        struct { int n; const char **fields; VALUE *items; } rec;    // OOBJ_RECORD
    };
};
```

`struct oobj` を type ごとに union で持つのは「サイズ ~80 byte 程度の頭でかい型を 1 種類に統一する」設計上の妥協。
プロダクション化するなら type ごとに別構造体に分けて cache pressure を下げるべき。

## 2. 環境 (`oframe` チェーン)

OCaml は静的スコープなので、クロージャは作成時の環境をキャプチャしておく必要がある。
astocaml では各レキシカルフレームを次の構造で表現する:

```c
struct oframe {
    struct oframe *parent;     // 1 つ外側のフレーム
    int nslots;                // このフレームのスロット数
    VALUE slots[];             // 値スロット
};
```

`CTX *c` は唯一のグローバル状態として現フレーム `c->env` を保持する。
変数参照は `(depth, slot)` のペアにコンパイルされ、`node_lref(c, n, depth, slot)` は
`oc_env_at(c, depth)->slots[slot]` を返す:

```c
static inline struct oframe *
oc_env_at(CTX *c, uint32_t depth)
{
    struct oframe *f = c->env;
    for (uint32_t i = 0; i < depth; i++) f = f->parent;
    return f;
}
```

`node_let` / `node_letrec` / `node_match_arm` / `node_let_pat` が新しいフレームを push する; いずれも

```c
struct oframe *f = oc_new_frame(c->env, nslots);
struct oframe *saved = c->env;
c->env = f;
VALUE r = EVAL_ARG(c, body);
c->env = saved;
return r;
```

の典型形を取る (例外: `node_apply` 内のフレーム push と例外時のフレーム巻き戻し)。

## 3. 関数呼び出し

`fun x y -> body` は `node_fun(2, body)` という単一ノードに desugar される。
`let f x y = body` は `let f = fun x y -> body` への糖衣で、同様の形になる。

### クロージャの作成 (`node_fun`)

```c
NODE_DEF @noinline
node_fun(CTX *c, NODE *n, uint32_t nparams, NODE *body)
{
    return oc_make_closure(body, c->env, (int)nparams);
}
```

実行時、現在の `c->env` をキャプチャした closure オブジェクトを heap に確保。

### 適用 (`oc_apply`)

`f a b c` は `node_app3(f, a, b, c)` → `oc_apply(c, f, 3, &av[0])`。

```c
VALUE
oc_apply(CTX *c, VALUE fn, int argc, VALUE *argv)
{
    if (OC_IS_PRIM(fn)) { /* 組み込み呼び出し */ ... }
    if (!OC_IS_CLOSURE(fn)) oc_raise(...);
    struct oobj *cl = OC_PTR(fn);
    int np = cl->closure.nparams;
    if (argc < np) {
        // 部分適用 — captured args を持つ "partial" を返す
        return /* partial sentinel */;
    }
    struct oframe *f = oc_new_frame(cl->closure.env, np);
    for (int i = 0; i < np; i++) f->slots[i] = argv[i];
    struct oframe *saved = c->env;
    c->env = f;
    VALUE r = EVAL(c, cl->closure.body);
    c->env = saved;
    if (argc == np) return r;
    return oc_apply(c, r, argc - np, argv + np);   // over-application
}
```

主な分岐:

1. **arity 一致** (`argc == np`): 通常呼び出し。新フレームを push し body を eval、戻す。
2. **過適用** (`argc > np`): 最初の np 引数で呼び出し、戻り値 (普通はクロージャ) に残り引数で再帰呼び出し。
3. **部分適用** (`argc < np`): captured args を持つラッパー型を返す。再呼び出し時に combined args で実行。
   現状の実装は OOBJ_PRIM のフィールドを流用したアドホックな形 (改善予定)。

### 末尾呼び出し最適化

**現状: 未実装**。`oc_apply` の末尾呼び出しは普通に C スタックに積まれていく。
深い再帰 (10000 step を超えるあたり) で `oc_apply` のスタックが伸びる。
TCO を入れるなら ascheme 同様に `c->next_body` / `c->tail_call_pending` フラグを使った trampoline 化が定石。

## 4. パターンマッチ

`match e with | p1 -> e1 | p2 -> e2 | ...` は次の形に desugar される (パーサで生成):

```
let __m = e in
   <chain>
```

ここで `<chain>` は arm 列を逆順に処理した nested expression:

```
fail = raise Match_failure
for arm in reverse(arms):
    test     = pat_gen_test(arm.pat, lref(0,0))   // bool
    extracts = pat_gen_extracts(arm.pat, ...)     // VALUE 評価式の配列
    if arm.guard:
        guarded_body = if arm.guard then arm.body else fail
        # 一旦パターン bind してからガード評価
        arm_node = node_match_arm(true, |extracts|, extracts_idx, guarded_body, fail)
        fail = if test then arm_node else fail
    else:
        fail = node_match_arm(test, |extracts|, extracts_idx, arm.body, fail)
```

各 arm は `node_match_arm` (test + arity + extracts + body + failure) として 1 つに集約される。
`node_match_arm` は **arm 内で導入される全変数を 1 つのフレームにまとめて push する** という重要な設計上の決定をしている:

```c
NODE_DEF @noinline
node_match_arm(CTX *c, NODE *n, NODE *test, uint32_t arity, uint32_t extracts_idx, NODE *body, NODE *failure)
{
    if (EVAL_ARG(c, test) != OC_TRUE) return EVAL_ARG(c, failure);
    if (arity == 0) return EVAL_ARG(c, body);          // ★ no frame push
    VALUE *vals = alloca(sizeof(VALUE) * arity);
    for (uint32_t i = 0; i < arity; i++) {
        NODE *e = OC_EXTRACT_NODES[extracts_idx + i];
        vals[i] = (*e->head.dispatcher)(c, e);          // OUTER env で評価
    }
    struct oframe *f = oc_new_frame(c->env, arity);
    for (uint32_t i = 0; i < arity; i++) f->slots[i] = vals[i];
    struct oframe *saved = c->env;
    c->env = f;                                          // ★ frame push
    VALUE r = EVAL_ARG(c, body);
    c->env = saved;
    return r;
}
```

★ 印の "arity が 0 ならフレーム push をスキップ" がパーサ側のスコープ管理と整合する必要があり、過去にここでバグが発生した。
パーサも `nv == 0` のときは `push_scope_n` を呼ばないよう揃えている。

### パターンコンパイラ (`pat_gen_test` / `pat_gen_extracts`)

パターンは再帰的構造体 `struct pat`:

```c
struct pat {
    enum pat_kind kind;
    union {
        const char *var_name;
        struct { struct pat *head, *tail; } cons;
        struct { int n; struct pat **items; } tup;
        struct { const char *name; struct pat *arg; } variant;
        ...
    };
    NODE *guard;
};
```

scrut 値は **scrut factory** という関数ポインタ抽象を経由してアクセスする:

```c
typedef NODE *(*scrut_fac_fn)(void *ctx);
```

末端の scrut (`__m`) は `scrut_fac_lref` が `lref(0, 0)` を毎回フレッシュ確保。
タプルやバリアントなどの sub-scrut は `scrut_fac_proj` で `cons_head` / `tuple_get` / `variant_get` などの NODE を被せる。

これにより、**毎回フレッシュな NODE を生成する** ので、ノード共有による親ポインタ衝突などの心配がない (代わりに少しメモリを消費する)。

## 5. 例外

`try body with | p1 -> h1 | ...` は `node_try(body, handler)` の形に desugar され、handler は match と同じ形。

```c
VALUE oc_run_try(CTX *c, NODE *body, NODE *handler)
{
    int top = ++c->handlers_top;
    struct oc_handler *h = &c->handlers[top];
    h->saved_env = c->env;
    if (setjmp(h->buf) == 0) {
        VALUE r = EVAL(c, body);
        c->handlers_top--;
        return r;
    }
    // longjmp で戻ってきた
    VALUE exn = h->exn;
    c->env = h->saved_env;
    c->handlers_top--;
    struct oframe *f = oc_new_frame(c->env, 1);
    f->slots[0] = exn;
    struct oframe *saved = c->env;
    c->env = f;
    VALUE r = EVAL(c, handler);
    c->env = saved;
    return r;
}
```

`raise e` は `oc_raise(c, exn_value)` を呼び出し、最も近い handler スロットに exn を書き込み `longjmp` で戻る。
未捕捉なら `c->err_jmp_active` フラグに従って top-level error trap で殺すか `exit(2)` する。

`oc_run_try` は **意図的に non-inline** にしている。ASTroGen が生成する `EVAL_node_try` は `__attribute__((always_inline))` 付きで、
gcc は **setjmp を含む always-inline 関数** を許さないため、`setjmp` 部分を別関数に追い出している。

## 6. グローバル

```c
struct gentry {
    const char *name;
    VALUE value;
};
```

シンプルな線形リスト (`globals[]`)。再定義や lookup は `strcmp` の線形探索。

`gref` ノードは parser で「現在のスコープに見つからない名前」のために生成され、実行時に毎回 lookup する:

```c
NODE_DEF
node_gref(CTX *c, NODE *n, const char *name)
{
    return oc_global_ref(c, name);
}
```

定数畳み込みやキャッシュは入れていない (将来的には `@ref` で inline cache を持たせるのが妥当)。

## 7. 主要なフロー

```
パース (main.c)
  ├─ next_token() でトークン化
  ├─ 再帰下降パーサ (parse_expr / parse_atom / etc.)
  │   ├─ 変数解決: scope stack を walk して lref(d, s) または gref(name)
  │   ├─ パターン: parse_pattern() で struct pat ツリー構築
  │   └─ match: build_match_arms() で node_match_arm のチェーンを構築
  │
  └─ 各 top-level let/expr について:
       EVAL(c, ast)
         └─ ノードの dispatcher を呼ぶ → EVAL_node_*  (always_inline)
             ├─ fun / app / let / match / try → 上記の通り
             └─ 算術 / 比較 / 等は短い体で完結 (最も hot)
```

## 8. なぜ多くのノードが `@noinline` なのか

ASTroGen はデフォルトで **EVAL_node_X を always-inline** にする (callsite が 1 つの DISPATCH/SD だけだから)。
が、いくつかのノードはこのインライン化を避けたい:

- `node_try`: setjmp を含むので always-inline 不可 (gcc エラー)
- `node_appn` / `node_tuple_n` / `node_variant_n` / `node_record_n` / `node_record_with` / `node_letrec_n` / `node_match_arm` / `node_let_pat`: alloca + ループを含み、コードサイズが大きい。inline すると register pressure / icache 圧迫で遅くなる
- `node_fun`: 単一で 1 alloc 程度しかしないが、パーサが書き出す closure 数だけ duplicate されるので outline 化が無難

それ以外の "hot path" ノード (lref / app1..4 / add / sub / mul / lt / cons / cons_head / cons_tail など) はインライン化対象。
