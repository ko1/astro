# runtime.md — nuq のランタイム解説

nuq は ASTro 上に乗せた **jq サブセットのツリーウォーカー**。各
NODE_DEF が `EMIT { items, count }` 構造体を return する形式で、
items は CTX 上の flat な VALUE pool に切られたスライス。これにより
per-emit GC alloc を消しつつ、SD specializer による AOT inlining
と相性の良い構造を取る。

ファイル構成:

| ファイル | 内容 |
|---|---|
| `node.def`  | AST ノード定義 (114 種、フィルタ言語 + 全 builtin) |
| `node.h`    | NodeHead + EMIT pool helper (`nuq_pool_push` / `nuq_emit_one` / `nuq_emit_slice`) |
| `context.h` | VALUE / nuq_obj / CTX / 公開 API + `nuq_op_*` / `nuq_eq` / `nuq_cmp` の `static inline` fast path |
| `node.c`    | アロケータ + ASTroGen 生成ファイルの `#include` |
| `value.c`   | VALUE 構築 / 等価 / 順序 / `+ - * / %` の slow path |
| `json.c`    | JSON parser + pretty-printer |
| `runtime.c` | tree-eval helpers (object_eval / interp / format / user_call / def_table 走査ほか) |
| `filter.c`  | jq フィルタ言語の lexer + recursive-descent parser + AST fusion peephole |
| `builtin.c` | builtin の VALUE-level 実装 (sort / unique / fromjson / `add` の type-dispatch kernel ほか) |
| `main.c`    | CLI driver |

## 1. 値モデル (VALUE)

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

- 数値整数は fixnum、`__builtin_*_overflow` で失敗時のみ heap double
  に昇格。bignum なし。
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

## 2.5 value 演算 fast path

`context.h` に `static inline` で `nuq_op_add / sub / mul / neg`、
`nuq_eq`、`nuq_cmp`、`nuq_truthy`、`nuq_make_int` の fixnum 高速路
を置き、slow case を `_slow` 接尾辞付き関数として `value.c` に残す。

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

`node_add` などが `nuq_op_add` を呼ぶと、fixnum-fixnum の場合は
inline 展開で関数 call が消える (gcc が分岐予測どおりに通す)。
`nuq_op_div / mod` は jq 仕様で常に double 演算 (5/2 == 2.5) なので
fast path がうま味なし、slow に直行。

## 3. CTX

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

    VALUE                 error;          /* NUQ_NULL = no error */
    uint32_t              break_label;    /* 0 = no break */
} CTX;
```

CTX は **`GC_malloc` で確保** する (重要 — `pool` / `var_stack` 等の
中身ポインタが GC ルートとして見える必要がある。`calloc` だと
Boehm の保守的 scanner から live と認識されず、内側ブロックが
回収されて `$x undefined` 等の謎挙動を起こす)。

## 4. 主要ノードの意味論と AST 例

`./nuq --dump-ast` で実 AST を確認できる。代表例:

### `.foo.bar` — フィールド連鎖

```
node_pipe
├── lhs: node_pipe
│       ├── lhs: node_field("foo")
│       └── rhs: node_field("bar")  -- 実際は parser が左右を組み立てる
└── rhs: ...
```

実際は parser が `parse_postfix` で `.foo.bar` を `pipe(pipe(identity?,
field(foo)), field(bar))` 風に作る (詳細は次の `.users[]` の例)。

### `.users[] | .name` — 反復 + フィールド

```
node_pipe
├── lhs: node_pipe(node_field("users"), node_iter)
└── rhs: node_field("name")
```

### `[.users[] | .name]` — 配列構築

```
node_array
└── body: pipe(pipe(field("users"), iter), field("name"))
```

`[...]` は body の emit を **1 配列にまとめて 1 個 emit** する。

### `[.users[] | select(.age > 30)] | length` — 典型的な集計

```
node_pipe
├── lhs: node_array
│       └── body: pipe(
│             pipe(field("users"), iter),
│             b_select(body=gt(field("age"), int(30))))
└── rhs: b_length
```

SD specializer が AOT で work すると、この全体が **1 つの SD 関数**
に折り畳まれる。lhs の array_ctor → 内側 pipe → users access → iter
ループ → select の cartesian → length の単一 emit、まで全部 inline。

### `map(.name)` — `[.[] | .name]` の糖衣

```
node_b_map
└── body: node_field("name")
```

`b_map` の本体は node.def 内に展開済み:

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

### `group_by(.country) | map({country: .[0].country, count: length})`

```
node_pipe
├── lhs: node_b_group_by
│         └── body: field("country")
└── rhs: node_b_map
          └── body: node_object(entries={
                "country": pipe(index(int(0)), field("country")),
                "count":   b_length})
```

### `if .age > 18 then "adult" else "minor" end`

```
node_if
├── cond: node_gt(field("age"), int(18))
├── thn:  node_str("adult")
└── els:  node_str("minor")
```

`else` 省略時は parser が `els = node_identity()` を入れる (生成 dispatcher
が operand pointer を unconditionally deref するため、NULL は不可)。

### `def square: . * .; [range(10)] | map(square) | add`

```
node_defs
├── defs_id: [(square, body=mul(identity, identity))]
└── body: pipe(
            pipe(array(b_range1(int(10))), b_map(call(square))),
            b_add)
```

ユーザ定義 `def` は side-table に lower されて、call サイトは
`node_call(name_id, arity)` になる。builtin (length / map / range / etc.)
は parser が直接対応 NODE に解決するので table lookup なし。

### `reduce range(10) as $i (0; . + $i)`

```
node_reduce
├── src:    b_range1(int(10))
├── var_id: $i
├── init:   int(0)
└── update: add(identity, var($i))
```

reduce の本体は node.def 内に展開済み — src を eval して emit 列を
取り、各 emit について `$i` を bind して update を eval、最終 acc を
1 個 emit。

### `try .foo catch "default"`

```
node_try
├── body:    field("foo")
└── handler: str("default")
```

`f?` (= `try f`) は parser が handler に **`node_empty` の sentinel** を
置く (NULL は generated dispatcher が deref して segfault する)。

## 5. SD specialization と AST fusion

### SD specialization

ASTro の SD specializer は `EVAL_ARG(c, child)` を見つけると child の
dispatcher を constant-fold して、child の body を親 SD に inline する。
この性質を活かすために、nuq では:

- **runtime helper を経由しない**: 多くの NODE 本体は node.def 直接
  展開で `EVAL_ARG` を子 operand に対して使う。
- **builtin が個別 NODE**: `length` / `map` / `select` / `range` など 60+
  の builtin はすべて parser が直接対応 NODE を生成 (`node_b_length`
  等)。runtime の linear builtin table は無い。
- **再帰 def 本体を独立 entry に登録**: `nuq_user_call` 内の
  `EVAL(c, fd->body)` は runtime resolved dispatcher なので top-level
  filter SD からは inline できない。`nuq_compile_all_def_bodies` が
  parse 時に集めた `def_tab` を walk して各 body を別 entry として
  `astro_cs_compile` に渡す (usage.md "Entry nodes")。upto / ack の
  AOT が伸びるのはこの仕組み。

これで `[range(N)] | map(. * 2) | add` のような典型 chain が **1 SD
関数** に焼き上がり、tight loop は GC alloc を除いて純 C と区別が
つかない速度になる。

長 helper は runtime.c に残っている (object_eval / interp / user_call
/ ほか):
- complex な cartesian や string 処理で実装が長く、inline すると
  per-bench SD が肥大化する
- chain hot path に出にくい / 既に O(n) 以上の本来コストがある

### AST fusion (parse-time peephole)

`filter.c` の `nuq_make_pipe(lhs, rhs)` が parse 時に意味保存の書き
換えを適用 (詳細は `perf.md`):

- `map(F) | map(G)` → `map(F | G)`
- `select(F) | select(G)` → `select(F and G)`
- `[body] | length` → `node_emit_count(body)` (専用ノード)
- `[body] | add` → `node_emit_fold_add(body)` (`add` の type-dispatch
  kernel `nuq_add_fold_items` を共有)
- 右辺エッジ fusion: `f | sel(a) | sel(b) | sel(c)` のような左結合
  chain も任意長で折り畳み

これにより SD specialize より前の段階で AST が短縮されるので、
interp / AOT 両方が恩恵を受ける。

### 残る伸びしろ

- **PGO 的な型 feedback**: ASTro framework に `swap_dispatcher` /
  `HOPT(n)` / `--pg-compile` などの部品はあるが nuq には未配線。
  jq は集合演算が重いので AST fusion ほどの効果は期待しにくい
  (詳細は perf.md)。
- pipe の CPS 化 (todo B-1) — alloc pattern の改善、AOT vs interp
  の差にはほぼ効かない。
- pool top の register 常駐化 (todo B-4) — pyramid に少し効くかも。

## 6. JSON I/O

`json.c` に手書き再帰下降パーサ + pretty-printer。

- パーサは `(src, len, *endp, *errmsg) → VALUE`。複数 value のストリーム
  入力を while ループで消費可。
- pretty-printer は jq 互換の数値整形 (整数値の double を整数表記、最短
  round-trip)。
- 文字列は ASCII printable をそのまま、制御コードは `\uXXXX` で escape。
- サロゲートペアは UTF-8 byte 列に decode。

## 7. ASTro / Code Store

`INIT()` で `astro_cs_init("code_store", ".", 0)`。`main.c` は parser
出力 AST に対して `astro_cs_compile` → `astro_cs_build` →
`astro_cs_reload` → `astro_cs_load` で SD を生成 dlopen → dispatcher
に patch する (`--no-compile` で skip)。

`astro_cs_build` の `make` が ccache 経由で落ちる環境では
`CCACHE_DISABLE=1` を設定 (project memory: `feedback_ccache_disable`)。
