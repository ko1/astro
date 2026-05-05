# runtime.md — nuq のランタイム解説

nuq は ASTro 上に乗せた **jq サブセットのツリーウォーカー**。各
NODE_DEF が VALUE (= emit 列を表す nuq_array) を return する形式で、
SD specializer による AOT inlining と相性のよい構造を取る。

ファイル構成:

| ファイル | 内容 |
|---|---|
| `node.def`  | AST ノード定義 (~75 種、フィルタ言語 + ほぼ全 builtin) |
| `node.h`    | NodeHead + binop / cmpop の inline 適用 helper |
| `context.h` | VALUE / nuq_obj / CTX / 公開 API |
| `node.c`    | アロケータ + ASTroGen 生成ファイルの `#include` |
| `value.c`   | VALUE 構築 / 等価 / 順序 / `+ - * / %` |
| `json.c`    | JSON parser + pretty-printer |
| `runtime.c` | tree-eval helpers (object_eval / interp / format / def-call ほか) |
| `filter.c`  | jq フィルタ言語の lexer + recursive-descent parser |
| `builtin.c` | builtin の VALUE-level 実装 (sort / unique / fromjson 等) |
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
        struct { VALUE *items; size_t len; size_t capa; } arr;
        struct { VALUE *keys; VALUE *vals; size_t len; size_t capa; } obj;
    };
};
```

数値整数は fixnum、`__builtin_*_overflow` で失敗時のみ heap double に
昇格。bignum なし。オブジェクトは挿入順を保つ flat parallel array
(`keys[]` / `vals[]`)、lookup は線形 O(n) — 小規模 JSON 用途では十分。

## 2. eval プロトコル

**各 NODE_DEF は VALUE を return する**。その VALUE は emit 列を表す
nuq_array。`EVAL_ARG(c, child)` は child の emit 配列を直接返す。

```c
NODE_DEF
node_b_length(CTX *c, NODE *n)
{
    VALUE r = nuq_make_array(1);
    nuq_array_push(r, nuq_length(c->input));
    return r;
}
```

エラーは `c->error` (NUQ_NULL = OK)、break は `c->break_label` (0 = 無)
で伝搬。実値はあくまで戻り値経由で、副チャネル (古い `emit_buf`) は
廃止。

これにより SD specializer が pipe / map / select 等の sub-expression
operand を **EVAL_ARG 経由で inline** できる:

```c
NODE_DEF
node_pipe(CTX *c, NODE *n, NODE *lhs, NODE *rhs)
{
    VALUE l = EVAL_ARG(c, lhs);          /* SD inline */
    if (UNLIKELY(c->error != NUQ_NULL)) return l;
    struct nuq_obj *lo = NUQ_PTR(l);
    VALUE r = nuq_make_array(0);
    VALUE saved = c->input;
    for (size_t i = 0; i < lo->arr.len; i++) {
        c->input = lo->arr.items[i];
        VALUE rv = EVAL_ARG(c, rhs);     /* SD inline */
        if (UNLIKELY(c->error != NUQ_NULL)) { c->input = saved; return r; }
        struct nuq_obj *ro = NUQ_PTR(rv);
        for (size_t j = 0; j < ro->arr.len; j++) nuq_array_push(r, ro->arr.items[j]);
    }
    c->input = saved;
    return r;
}
```

## 3. CTX

```c
typedef struct CTX_struct {
    VALUE                 input;          /* 現 `.` (pipe が per-emit に設定) */

    struct nuq_var_slot  *var_stack;      /* `as $x` 束縛 */
    size_t                var_top, var_capa;

    struct nuq_func_def **funcs;           /* `def` 定義のスタック */
    size_t                func_cnt, func_capa;

    VALUE                 error;          /* NUQ_NULL = no error */
    uint32_t              break_label;    /* 0 = no break */
} CTX;
```

CTX は **`GC_malloc` で確保** する (重要 — `var_stack` 等の中身ポインタ
が GC ルートとして見える必要がある)。

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
    for (size_t i = 0; i < o->arr.len; i++) {
        c->input = o->arr.items[i];
        VALUE bo = EVAL_ARG(c, body);   /* body inline */
        ...
    }
    ...
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

## 5. SD specialization の効き方

ASTro の SD specializer は `EVAL_ARG(c, child)` を見つけると child の
dispatcher を constant-fold して、child の body を親 SD に inline する。
この性質を活かすために、nuq では:

- **runtime helper を経由しない**: 多くの NODE 本体は node.def 直接
  展開で `EVAL_ARG` を子 operand に対して使う。
- **builtin が個別 NODE**: `length`、`map`、`select`、`range` などは
  parser が直接対応 NODE を生成 (`node_b_length` 等)。table lookup や
  関数呼び出しを介さず、本体は node.def に inline。

これで `[range(N)] | map(. * 2) | add` のような典型 chain が **1 SD
関数** に焼き上がり、tight loop は GC alloc を除いて純 C と区別が
つかない速度になる。

長 helper は runtime.c に残っている (object_ctor / interp / format /
user_call / split / join / *_by 等):
- complex な cartesian や string 処理で実装が長く、inline すると
  per-bench SD が肥大化する
- これらは「SD inline できなくても困らない」もの (chain hot path に
  ほぼ出ない or 既に O(n) 以上の本来コストがある)

将来の改善余地としては:
- inline 化されていない helper も node.def に展開する (コード量と inline
  の trade-off)
- pipe を CPS 化して emit_buf を完全に消す (allocator pressure 削減)
- per-call `alloca` ベースの emit buffer (todo B-4)

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
