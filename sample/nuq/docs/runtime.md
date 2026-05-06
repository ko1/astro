# runtime.md — nuq の実装詳解

nuq は ASTro 上に乗せた **jq 互換ツリーウォーカー**。各 NODE_DEF が
`EMIT { items, count }` を返す形式で、items は CTX 上の flat な VALUE
pool に切られたスライス。これにより per-emit GC alloc を消しつつ、
SD specializer による AOT inlining と相性の良い構造を取る。

ファイル構成:

| ファイル | 内容 |
|---|---|
| `node.def`  | AST ノード定義 (フィルタ言語 + 全 builtin) |
| `node.h`    | NodeHead + EMIT pool helper (`nuq_pool_push` / `nuq_emit_one` / `nuq_emit_slice`) |
| `context.h` | VALUE / nuq_obj / CTX / 公開 API + `nuq_op_*` / `nuq_eq` / `nuq_cmp` の `static inline` fast path |
| `node.c`    | アロケータ + ASTroGen 生成ファイルの `#include` |
| `value.c`   | VALUE 構築 / 等価 / 順序 / `+ - * / %` の slow path |
| `json.c`    | JSON parser + pretty-printer |
| `runtime.c` | tree-eval helpers (object_eval / interp / format / user_call / def_table 走査ほか) |
| `filter.c`  | jq lexer + recursive-descent parser + AST fusion + module loader |
| `builtin.c` | builtin の VALUE-level 実装 (sort / unique / fromjson / `add` の type-dispatch kernel ほか) |
| `main.c`    | CLI driver |

## 1. 値モデル

```
xxxx_xxx1 → 62-bit signed fixnum (左 1 シフト + 1)
xxxx_xxx0 → ヒープオブジェクト (`struct nuq_obj *`、8-byte aligned)
```

`null` / `true` / `false` は **静的に確保された singleton `nuq_obj`** で、
`NUQ_NULL` / `NUQ_TRUE` / `NUQ_FALSE` というアドレス VALUE をそのまま
配る。

```c
struct nuq_obj {
    enum nuq_type type;
    union {
        bool b;
        double dbl;
        struct { char *bytes; size_t len; } str;
        struct {
            VALUE *items; size_t len; size_t capa;
            VALUE  inline_buf[NUQ_ARR_INLINE];   /* 4-slot inline */
        } arr;
        struct {
            VALUE *keys; VALUE *vals; size_t len; size_t capa;
            uint32_t *idx; uint32_t idx_mask;    /* lazy hash idx */
        } obj;
    };
};
```

- 数値整数は fixnum、`__builtin_*_overflow` で失敗時のみ heap double に
  昇格。bignum なし (decnum 未対応 — `done.md` 参照)。
- 配列は **挿入順 + 4 slot inline buffer**。`nuq_make_array(N)` で
  N ≤ 4 なら `inline_buf` を使い alloc を節約。
- オブジェクトは **挿入順 parallel array `keys[]` / `vals[]`**。`len`
  が `NUQ_OBJ_HASH_MIN` (=16) を超えると lazy で `idx[]`
  (open-addressing FNV-1a, load factor ≤ 0.5) を build。lookup が
  O(1) になり、jq 互換の挿入順イテレーションは parallel array 側で
  そのまま。

## 2. eval プロトコル

**各 NODE_DEF は `EMIT` 構造体を return する** (16 byte、`{ items,
count, flags }`)。`items` は **CTX の `pool[]` 上のスライスへの
ポインタ**、`count` はそのスライスの emit 個数。

```c
NODE_DEF
node_b_length(CTX *c, NODE *n)
{
    return nuq_emit_one(c, nuq_length(c->input));
}
```

`nuq_emit_one(c, v)` は内部的に `nuq_pool_push(c, v)` で 1 個積み、
`nuq_emit_slice` で範囲を取り出す。0 emit を返したいときは
`EMIT_EMPTY`。

呼び出し側の責任で **pool top0 を保存し、必要なら巻き戻す**:

```c
NODE_DEF
node_array(CTX *c, NODE *n, NODE *body)
{
    size_t top0 = c->pool_top;
    EMIT bo = EVAL_ARG(c, body);                 /* SD inline */
    if (UNLIKELY(c->error != NUQ_NULL)) return EMIT_EMPTY;
    VALUE arr = nuq_make_array(bo.count);
    for (uint32_t i = 0; i < bo.count; i++) nuq_array_push(arr, bo.items[i]);
    c->pool_top = top0;                           /* slice 解放 */
    return nuq_emit_one(c, arr);
}
```

エラーは `c->error` (NUQ_NULL = OK) で伝搬、break は `c->break_label`
(0 = 無)。実値は EMIT 経由のみ。`pool_top = top0` の巻き戻しは
stack-discipline で sub-expr の emits を解放する hot pattern。

これにより SD specializer は `EVAL_ARG(c, child)` を見つけて child
dispatcher の本体を親 SD に inline できる。pipe / map / select / array
ctor などすべてこの形。

### Partial 出力 + error
pipe / iter / setpath などの中で error が発生した場合、すでに
`pool` に push されたスライスは **slice として return** する (jq 互換
の partial output 規則)。`nuq_run` が pool スライスを stdout に出した
あとで `c->error` を stderr に flush する。

## 3. value 演算 fast path

`context.h` に `static inline` で fixnum 高速路を置き、slow case を
`_slow` 接尾辞付き関数として `value.c` に残す。

```c
static inline VALUE
nuq_op_add(VALUE a, VALUE b) {
    if (LIKELY(NUQ_IS_FIX(a) && NUQ_IS_FIX(b))) {
        int64_t la = NUQ_FIX_VAL(a), lb = NUQ_FIX_VAL(b), r;
        if (LIKELY(!__builtin_add_overflow(la, lb, &r))) return nuq_make_int(r);
    }
    return nuq_op_add_slow(a, b);
}
```

`node_add` などが `nuq_op_add` を呼ぶと、fixnum-fixnum の場合は inline
展開で関数 call が消える。`nuq_op_div / mod` は jq 仕様で常に double
演算 (`5/2 == 2.5`) なので fast path がうま味なく、slow に直行。

inline 対象: `nuq_op_add` / `sub` / `mul` / `neg`、`nuq_eq`、`nuq_cmp`、
`nuq_truthy`、`nuq_make_int`。

## 4. CTX

```c
typedef struct CTX_struct {
    VALUE                 input;          /* 現 `.` (pipe が per-emit に設定) */

    /* EMIT pool — flat VALUE buffer。各 NODE_DEF が pool_top を起点に
     * push してスライスを return、呼出側が `c->pool_top = top0` で
     * 巻き戻す stack-discipline。startup で 4096 entries pre-grow、
     * 以降の realloc は UNLIKELY 経路。 */
    VALUE                *pool;
    size_t                pool_top, pool_capa;

    struct nuq_var_slot  *var_stack;      /* `as $x` 束縛 */
    size_t                var_top, var_capa;

    struct nuq_func_def **funcs;           /* `def` 定義のスタック */
    size_t                func_cnt, func_capa;
    size_t                func_skip_start; /* 一時的に skip する範囲 */
    size_t                func_skip_end;

    VALUE                 error;          /* NUQ_NULL = no error */
    uint32_t              break_label;    /* 0 = no break */
    bool                  path_drop_pending;  /* select 経由の drop signal */
} CTX;
```

CTX は **`GC_malloc` で確保** する。`pool` / `var_stack` / `funcs` 等の
内部ポインタが GC ルートとして見える必要がある — `calloc` だと
Boehm の保守的 scanner から live と認識されず、内側ブロックが回収
されて `$x undefined` などの謎挙動を起こす。

## 5. 主要ノードの意味論

`./nuq --dump-ast` で実 AST を確認できる。代表例:

### `.users[] | .name`
```
node_pipe
├── lhs: pipe(field("users"), iter)
└── rhs: field("name")
```

### `[.users[] | select(.age > 30)] | length`
```
node_pipe
├── lhs: array
│         └── body: pipe(pipe(field("users"), iter),
│                        b_select(body=gt(field("age"), int(30))))
└── rhs: b_length
```

SD specializer が AOT で work すると、この全体が **1 つの SD 関数**
に折り畳まれる。lhs の array_ctor → 内側 pipe → users access → iter
ループ → select の cartesian → length の単一 emit、まで全部 inline。

なお AST fusion の `[X] | length` ルールが先に発火して
`emit_count(body)` 1 ノードになる場合もある — fusion は parser 内で
意味保存のまま行われる。

### `map(.name)` (= `[.[] | .name]`)
```c
NODE_DEF
node_b_map(CTX *c, NODE *n, NODE *body)
{
    /* ...input 配列を取り出して... */
    VALUE result = nuq_make_array(o->arr.len);
    for (size_t i = 0; i < o->arr.len; i++) {
        c->input = o->arr.items[i];
        size_t top0 = c->pool_top;
        EMIT bo = EVAL_ARG(c, body);    /* body SD inline */
        for (uint32_t j = 0; j < bo.count; j++) nuq_array_push(result, bo.items[j]);
        c->pool_top = top0;             /* slice 解放 */
    }
    return nuq_emit_one(c, result);
}
```

### `if cond then T else E end`
```
node_if
├── cond: ...
├── thn:  ...
└── els:  ... (省略時 parser が node_identity を default に)
```

generated dispatcher が operand を unconditionally deref するため、
NULL は不可。`f?` の `try` も handler に `node_empty` の sentinel を
置く。

### `def f(g; h): body;` の call
ユーザ定義 `def` は side-table に lower、call サイトは
`node_call(name_id, arity, args)`。`runtime.c` の `nuq_user_call` が
- value-arg (`$`-prefix) は eager 評価、cartesian 展開
- filter-arg (no-prefix) は **call-by-name closure** (下記)

## 6. Call-by-name closure

`def f(x): ...` の `x` (no-prefix) は call site で **値ではなく式 AST と
caller scope を保存** する。f の body から `x` を参照するたびに caller
scope で式を再評価する。

実装: `struct nuq_func_def` に `var_snap` (var stack snapshot) と
`var_snap_cnt` を持たせる。

```c
struct nuq_func_def {
    uint32_t   name_id;
    int        arity;
    uint32_t  *param_ids;
    bool      *param_is_value;
    struct Node *body;
    size_t     scope_top;          /* 関数 scope の lexical boundary */
    struct nuq_var_slot *var_snap; /* call-by-name 用の var stack snap */
    size_t     var_snap_cnt;
};
```

f が call されるとき、各 filter-arg `g` について:
- 0-arity の `pfd` (param-def) を作って `body = arg AST`
- `pfd->scope_top = c->func_cnt - 1` (f 自身を skip した caller scope)
- `pfd->var_snap = clone(c->var_stack)`、`pfd->var_snap_cnt = c->var_top`

f の body から `x` を call すると `nuq_user_call(x_pfd)` が呼ばれ:
- 既存の var stack を退避 (`saved_stack`, `saved_top`, `saved_capa`)
- snap を fresh 配列にクローンして c->var_stack に swap
- body を eval (snap clone の上で `as $y` などで push しても破壊しない)
- swap を戻す

func スコープも `func_skip_start` / `func_skip_end` で `pfd->scope_top`
までに制限し、f 自身や f が定義した内側 def を見えなくする。

これにより:
```
2000 as $x | def f(x): 1 as $x | [$x, x, x];
def g(x): 100 as $x | f($x, $x+x);
g($x)
```
で f の body の `x` 参照が g scope で `$x, $x+x` を評価し、g scope
での `$x = 100`、g の x = root scope の `$x = 2000` を解決して
`100, 2100` の 2 emit を生成、配列 `[1, 100, 2100, 100, 2100]` が
できる (jq 1.7 と一致)。

## 7. SD specialization と AST fusion

### SD specialization

ASTro の SD specializer は `EVAL_ARG(c, child)` を見つけると child の
dispatcher を constant-fold して、child の body を親 SD に inline する。
nuq では:

- **runtime helper を経由しない**: 多くの NODE 本体は node.def 直接
  展開で `EVAL_ARG` を子 operand に対して使う
- **builtin が個別 NODE**: `length` / `map` / `select` / `range` など
  60+ の builtin はすべて parser が直接対応 NODE を生成 — runtime の
  linear builtin table は無い
- **再帰 def 本体を独立 entry に登録**: `nuq_user_call` 内の
  `EVAL(c, fd->body)` は runtime resolved dispatcher なので top-level
  filter SD からは inline できない。`nuq_compile_all_def_bodies` が
  parse 時に集めた `def_tab` を walk して各 body を別 entry として
  `astro_cs_compile` に渡す (usage.md "Entry nodes")。`upto` / `ack` の
  AOT が伸びるのはこの仕組み。

これで `[range(N)] | map(. * 2) | add` のような典型 chain が **1 SD
関数**に焼き上がり、tight loop は GC alloc を除いて純 C と区別が
つかない速度になる。

長 helper は runtime.c に残っている (object_eval / interp / user_call /
ほか) — complex な cartesian や string 処理で実装が長く、inline すると
per-bench SD が肥大化する / chain hot path に出にくいケース。

### AST fusion (parse-time peephole)

`filter.c` の `nuq_make_pipe(lhs, rhs)` が parse 時に意味保存の書き
換えを適用:

- `map(F) | map(G)` → `map(F | G)` (中間配列消去)
- `select(F) | select(G)` → `select(F and G)` (短絡保存)
- `[body] | length` → `node_emit_count(body)` (専用ノード)
- `[body] | add` → `node_emit_fold_add(body)` (`add` の type-dispatch
  kernel `nuq_add_fold_items` を共有)
- **右辺エッジ fusion**: parse は左結合なので `f | g | h` は
  `pipe(pipe(f, g), h)` になる。`nuq_make_pipe` で lhs が pipe なら
  その rhs と新 rhs を `nuq_try_fuse_pair` に投げ、成功なら splice
  戻す。`f | sel(a) | sel(b) | sel(c)` のような任意長 chain が
  左から順に折り畳まる

意味保存は jq 公式テスト + ローカル差分テストで常時チェック。

## 8. Path-mode walk

`walk_path(c, n, v, fn, ud)` は AST `n` を path として辿り、leaf に
`fn` を適用しつつ container を rebuild。代入 `=` `|=` / `del` /
`setpath` の実装基盤。

サポートする path 構成要素:
- `.` (identity) — leaf
- `.foo` (field) — descend、auto-vivify object
- `.[expr]` (index) — int → array、string → object、auto-vivify
- `.[]` (iter) — for each child
- `.[a:b]` (slice)
- `pipe(a, b)` — recurse a with `nested_apply(b, fn, ud)` as leaf
- `select(cond)` — cond truthy なら fn 適用、否なら v 不変
- `as $x | body` — bind して body へ
- `..` (recurse) — bottom-up rebuild、各 sub-tree に fn 適用
- `getpath([keys...])` — path 配列で descend

`..` は test #432 の `(.. | select(P) | .b) |= F` 形をサポート:
```c
if (n->head.kind == &kind_node_recurse) {
    /* descend first (post-order) */
    VALUE updated = v;
    if (NUQ_IS_PTR(v)) {
        /* rebuild children with walk_path(c, n /* recurse again */, child, fn, ud) */
        ...
    }
    /* then apply fn at this level */
    return fn(updated, ud, &dropped);
}
```

これで「全マッチを bottom-up に 1 度に更新」が動く。

## 9. Lazy stream eval

`limit(N; gen)` / `first(gen)` / `last(gen)` / `nth(N; gen)` /
`any(gen; cond)` / `all(gen; cond)` / `isempty(gen)` は
`nuq_stream_eval(c, body, cb, ud)` ヘルパが gen を遅延展開:

- `body` が `node_comma(lhs, rhs)` なら `stream_eval(lhs)` → 続けて
  `stream_eval(rhs)`
- `body` が `node_pipe(lhs, rhs)` なら `stream_eval(lhs, pipe_inner_cb)`
  で各 lhs emit に対し `c->input` を設定して `stream_eval(rhs)`
- それ以外は普通の `EVAL`、各 emit について `cb(c, v, ud)` を呼ぶ
- `cb` が false を return すると stream を打切る

これで `limit(1; 1, error)` が `1` で停止、`error` を評価しない。

## 10. Module loader

`filter.c` 末尾の section に実装:

- `struct nuq_module`: cache キーは canonical abs path、各 module に
  ユニークな `ns_id` を割り当て
- `loaded_defs[]`: 全 module の def を flat に集めた配列 — 名前は
  `<ns_id>::<original>` で qualify
- `parse_directives`: top-level / module file head の `module {meta};`、
  `import "X" as foo;`、`include "X";` を recursive に処理
- `prescan_local_defs`: module body を full parse する前に lex-scan で
  def 名 + arity を集める。これにより body 内の bare 名 call を
  `<my_ns>::<name>` に rewrite できる
- 探索: `{search: "..."}` import meta → `-L` パス → CWD の順で
  `<dir>/<rel>.jq` と `<dir>/<rel>/<rel>.jq` を試す
- データ import (`as $var`) は `nuq_user_arg_add_value` 経由で
  `$var` と `$var::var` 両方に bind
- `include` は後勝ち shadow — alias / include 配列を末尾から逆順で
  検索する
- 循環 import は cache に pre-register することで安全に終結
- `modulemeta` builtin は別経路で軽量 lex-scan のみ — defs を実際に
  load しない

## 11. JSON I/O

`json.c` に手書き再帰下降パーサ + pretty-printer。

- パーサは `(src, len, *endp, *errmsg) → VALUE`。複数 value のストリーム
  入力を while ループで消費可
- 深さ 10001 以上で `"Exceeds depth limit for parsing"` を返す
- jq 互換の `Infinity` / `-Infinity` / `NaN` / `nan` リテラル accept
- エラーメッセージは jq 互換の `at line L, column C (while parsing
  '<src>')` 形式 (`fmt_err_loc`)
- pretty-printer は jq 互換の数値整形:
  - 整数値の double を整数表記
  - 通常は最短 round-trip (`%.15g` から `%.17g` まで増やして strtod
    一致確認)
  - 整数値で `>2^53` の大きい double は `%.17g` のマンティッサ +
    末尾ゼロパッディングで fixed-point 化 (jq の decnum-flavoured
    error メッセージと一致)
  - 深さ 10001 以上で `"<skipped: too deep>"` プレースホルダ
- 文字列は ASCII printable をそのまま、制御コードは `\uXXXX` で escape
- サロゲートペアは UTF-8 byte 列に decode

## 12. ASTro / Code Store

`INIT()` で `astro_cs_init("code_store", ".", 0)`。`main.c` は parser
出力 AST に対して `astro_cs_compile` → `astro_cs_build` →
`astro_cs_reload` → `astro_cs_load` で SD を生成 dlopen → dispatcher
に patch する (`--no-compile` で skip)。

`astro_cs_build` の `make` が ccache 経由で落ちる環境では
`CCACHE_DISABLE=1` を設定 (project memory: `feedback_ccache_disable`)。

再帰 def の AST を hot loop に載せるために、`nuq_compile_all_def_bodies`
が `def_tab` を walk して全 def body を独立 entry として登録、
`nuq_load_all_def_bodies` が dlopen 後に dispatcher を patch する。

## 13. 設計上の妥協

- emit は **CTX 上の flat VALUE pool** からのスライス。pros: per-emit
  GC alloc ゼロ、SD inlining 容易。cons: pool 巻き戻し忘れがバグる
- pipe は **lhs を一旦配列に集めて iterate**。streaming にはなって
  いないが、実用 JSON サイズでは問題にならない
- object は **挿入順 parallel array + lazy hash idx**。jq 互換のため
  keys は順序保持
- 値表現は IEEE-754 double + 62-bit fixnum。decnum 未対応 — jq 公式
  テストの残り 2 件は decnum 必須なので原理的に通せない
