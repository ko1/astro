# runtime.md — nuq のランタイム解説

nuq は ASTro 上に乗せた jq サブセットの **tree-walking インタプリタ**。
v0 時点で動く範囲は [done.md](./done.md)、未実装は [todo.md](./todo.md)
を参照。

ファイル構成:

| ファイル | 内容 |
|---|---|
| `node.def`   | AST ノード定義 (~50 種、フィルタ言語の各構文) |
| `context.h`  | `VALUE` 表現、`nuq_obj`、`CTX`、option 構造体 |
| `node.h`     | `NodeHead` / 公開 API |
| `node.c`     | アロケータ + ASTroGen 生成ファイルの `#include` |
| `value.c`    | VALUE 構築 / 等価 / 順序 / JSON 算術 (`+ - * / %`) |
| `json.c`     | JSON parser + pretty-printer |
| `runtime.c`  | tree-eval helpers (binop / pipe / object ctor / try / interp) |
| `filter.c`   | jq フィルタ言語の lexer + recursive-descent parser |
| `builtin.c`  | 70+ の組み込み関数テーブル |
| `main.c`     | CLI driver (`-c -r -R -s -n` ほか) |

## 1. VALUE 表現

```
xxxx_xxx1 → fixnum (62-bit signed、左 1 シフト + 1)
xxxx_xxx0 → ヒープオブジェクト (`struct nuq_obj *`、8-byte aligned)
```

`null` / `true` / `false` は **静的に確保された singleton `nuq_obj`** で、
`NUQ_NULL` / `NUQ_TRUE` / `NUQ_FALSE` というアドレス VALUE をそのまま
配る。pystro / astr の null 表現と同じパターン。

ヒープ型は `enum nuq_type` の `NUQ_T_*`:
`NULL / BOOL / DOUBLE / STRING / ARRAY / OBJECT`。

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

double は **常にヒープ box**。NaN-boxing / inline flonum は v0 で入れて
いない (project memory `no_nan_boxing` 参照: 提案禁止)。整数は
`__builtin_*_overflow` チェックで fixnum 範囲を外れたら double に
昇格する。bignum は無いので `nuq_make_int` の overflow 経路で精度が
落ちる。

オブジェクトは挿入順を保つ flat な parallel array `keys[] / vals[]`。
ハッシュ表ではないので lookup は線形 O(n) — jq の典型的な n
(数〜数十) では問題なし。等価判定は両側のキー集合一致 + 値の
ペアワイズ等価 (順序は問わない)。

VALUE 比較は jq の型順序に従う:
`null < false < true < number < string < array < object`。
オブジェクト比較はキーをソートしてからペアワイズで再帰比較
(`nuq_keys(v, true)` でソート済みキーを取る)。

## 2. 実行コンテキスト (`CTX`)

```c
typedef struct CTX_struct {
    VALUE                 input;       /* 現 `.` */
    VALUE                 emit_buf;    /* 現出力先 (nuq array VALUE) */

    struct nuq_var_slot  *var_stack;   /* `as $x` 束縛 (id, value) ペア */
    size_t                var_top, var_capa;

    struct nuq_func_def **funcs;        /* `def` 定義のスタック */
    size_t                func_cnt, func_capa;

    uint32_t              break_label;  /* 0 = 進行中の break なし */
    VALUE                 error;        /* NUQ_NULL のとき例外なし */
} CTX;
```

すべてのフィルタは `c->input` を読んで、結果を `c->emit_buf` (= 配列
VALUE) に push する。dispatcher の戻り値は **制御フロー** のみ:

| 戻り値 | 意味 |
|---|---|
| `BR_OK` (0) | 正常終了 |
| `BR_BREAK` (1) | `break $label` 巻き上げ中 (label id は `c->break_label`) |
| `BR_ERROR` (2) | エラー巻き上げ中 (値は `c->error`) |

実値は emit_buf 経由で流れるので、戻り値と値表現の channel は
被らない。

## 3. 評価モデル — なぜ continuation chain ではなく tree walk なのか

ASTro サンプル (特に astrogre) は astrogre 流の **`next` operand を
チェーンする continuation passing** でフィルタを 1 つの SD 関数に
fold-in できる構造を取ることが多い。これは regex のように
straight-line な「成功 / 失敗」のチェーンには綺麗に乗るが、jq の以下
の特徴があると機械的に組みにくい:

- **左結合の算術**: `a - b - c` を `(a-b)-c` で評価するには、L 連鎖
  を CPS に変換する間に operand スパンを保存しなくてはいけない。
- **fan-out**: `(.a, .b) + (.c, .d)` は 4 つの emit を出す。binop は
  両 stream を集めて cartesian で組む。これは continuation で扱うのは
  かなり煩雑。
- **動的な制御**: `if-then-else / try-catch / reduce-foreach` は内部で
  別 stream を回したり buffer したりする。CPS 化すると保存と差替が
  深くネストする。

逆に **tree walk + emit buffer** で書くと、上の 3 つはすべて

```c
VALUE buf;
VALUE r = nuq_eval_collect_status(c, sub, input, &buf);
struct nuq_obj *bo = NUQ_PTR(buf);
for (size_t i = 0; i < bo->arr.len; i++) {
    /* この emit について何かする */
}
```

の同じ helper でまかなえる。`nuq_eval_collect_status(c, body, input, *out)`
は内部で `c->emit_buf` を新しい配列に差替えて `EVAL(body)` を呼び、
emit 配列を caller に返す。

代わりに失う性能上の特性:

- **pipe stage 越境の specialization**: `f | g` は `f` の出力を一度
  集めるので、SD specializer は `f` と `g` を別関数として焼く。
  AST 全体を 1 関数に fold-in することはできない (todo B1)。
- **streaming**: 長 stream は materialize される。

ただし **一般的な jq workload (小入力)** では tree walk のオーバーヘッドは
軽微。AOT bake はノード単位ではちゃんと効く (具体は § 5)。

## 4. ノード一覧 (node.def)

| グループ | ノード |
|---|---|
| 識別子 / リテラル | `node_identity` `node_recurse` `node_null` `node_true` `node_false` `node_int` `node_lit` `node_str` `node_interp` `node_format` |
| アクセス | `node_field` `node_field_opt` `node_index` `node_index_opt` `node_iter` `node_iter_opt` `node_slice` `node_slice_opt` |
| 合成 | `node_pipe` `node_comma` |
| 算術 | `node_add` `node_sub` `node_mul` `node_div` `node_mod` `node_neg` |
| 比較 / 論理 | `node_eq` `node_ne` `node_lt` `node_le` `node_gt` `node_ge` `node_and` `node_or` `node_not` `node_alt` |
| 構築 | `node_array` `node_array_empty` `node_object` |
| 制御 | `node_if` `node_try` `node_as` `node_var` `node_label` `node_break` `node_empty` `node_error0` `node_error1` |
| 関数 | `node_call0` `node_call1` `node_call2` `node_call3` `node_defs` |
| ループ | `node_reduce` `node_foreach` |
| 代入 (stub) | `node_assign` `node_update_assign` |

ほとんどのノードに `@noinline` が付いていて、`runtime.c` の helper
を呼ぶだけのスタブ。leaf (`node_int` `node_str` `node_identity`
`node_field` ほか) は inline 可能で `EVAL` 直下まで畳み込まれる。

## 5. 評価のパターン

### 5.1 単純なフィルタ — `node_field`

```c
NODE_DEF
node_field(CTX *c, NODE *n, const char *name)
{
    VALUE v;
    if (nuq_field_lookup(c->input, name, false, &v)) {
        nuq_emit(c, v);
        return BR_OK;
    }
    c->error = nuq_make_string("type error", 10);
    return BR_ERROR;
}
```

`c->input` を見て、object なら value、null なら null、それ以外は型
エラー。`nuq_emit` は `c->emit_buf` 配列に push するだけ。

### 5.2 sub-eval を要するフィルタ — `node_if`

```c
VALUE
nuq_if_eval(CTX *c, struct Node *cond, struct Node *thn, struct Node *els)
{
    VALUE cs;
    VALUE r = nuq_eval_collect_status(c, cond, c->input, &cs);
    if (r != BR_OK) return r;
    struct nuq_obj *co = NUQ_PTR(cs);
    for (size_t i = 0; i < co->arr.len; i++) {
        struct Node *branch = nuq_truthy(co->arr.items[i]) ? thn : els;
        if (branch == NULL) {
            nuq_emit(c, c->input);             /* else 省略時は `.` 通過 */
        } else {
            r = EVAL(c, branch);
            if (r != BR_OK) return r;
        }
    }
    return BR_OK;
}
```

cond は **stream** なので、各 emit ごとに分岐を選んで実行。jq 仕様
通り `(true, false) | if . then "T" else "F" end` のように cond
fan-out もちゃんと走る。

### 5.3 binop / cartesian fan-out — `node_add`

```c
VALUE
nuq_binop_eval(CTX *c, struct Node *lhs, struct Node *rhs, int op)
{
    VALUE las, ras;
    VALUE r = nuq_eval_collect_status(c, lhs, c->input, &las); if (r != BR_OK) return r;
    r = nuq_eval_collect_status(c, rhs, c->input, &ras);       if (r != BR_OK) return r;
    struct nuq_obj *la = NUQ_PTR(las);
    struct nuq_obj *rb = NUQ_PTR(ras);
    for (size_t i = 0; i < la->arr.len; i++)
        for (size_t j = 0; j < rb->arr.len; j++)
            nuq_emit(c, apply_binop(op, la->arr.items[i], rb->arr.items[j]));
    return BR_OK;
}
```

LHS / RHS をそれぞれ別 buffer に集めて cartesian で組合せる。

### 5.4 ストリーミングが必要な箇所 — `node_try`

`try f catch g` は body の emit を **直接 caller の emit_buf に流し込む**
(集めない) のがポイント。エラー前に出した emit を保持するため:

```c
VALUE
nuq_try_eval(CTX *c, struct Node *body, struct Node *handler)
{
    VALUE saved_error = c->error;
    VALUE r = EVAL(c, body);          /* body は caller の emit_buf に直接 emit */
    if (r == BR_ERROR) {
        VALUE err = c->error;
        c->error = saved_error;
        if (handler) {
            VALUE saved = c->input;
            c->input = err;
            r = EVAL(c, handler);
            c->input = saved;
            return r;
        }
        return BR_OK;                  /* `try f` 単独はエラーを呑む */
    }
    return r;
}
```

これで `[try (1, error("x"), 3) catch "k"]` が jq と同じく `[1,"k"]` に
なる (1 が出た後に error、catch の "k" が続く)。実装初期は body を
buf に集めてから return code を見ていて、エラー時に buf を捨てて
いたので 1 が消えるバグだった (`done.md` の修正履歴参照)。

## 6. パーサ — フィルタ言語

`filter.c` は手書きの再帰下降 + Pratt 風の優先順位ラダー:

```
parse_pipe        f | g
  parse_comma     f, g
    parse_alt     f // g
      parse_or    f or g
        parse_and f and g
          parse_compare  f == g, etc.
            parse_addsub  f + g, f - g
              parse_muldiv  f * g, f / g, f % g
                parse_unary   -f
                  parse_postfix  (a.b.c.[0]?, …)
                    parse_primary  リテラル / 変数 / `(...)` / 制御構文
```

特殊点:

- `as $x | body` は **postfix で起動** する: `parse_postfix` 内で `as`
  を見たらそこで終端し、`$x | body` を消化して `node_as` を返す。
  これにより `expr as $x | rest` の binding がパイプより強い。
- `reduce / foreach` の SRC 部分は通常の postfix では `as` を捕食
  してしまうので、`parse_term_for_keyword` という専用の "postfix
  だが `as` で停止" 関数を持つ。
- オブジェクト値 `{k: e}` の中の `e` は **comma を含めない pipe**
  までを許す (`parse_pipe_no_comma`)。`,` は entry 区切りと衝突するため。
- 文字列 `"prefix \(expr) suffix"` は lexer 段階で展開する。`\(...)` の
  括弧深さを数えて中の式テキストをスライスし、`nuq_compile_subexpr`
  に渡してサブフィルタとして parse。複数の interp が混ざると
  `node_interp(parts_id)` 1 個に集約。

## 7. JSON I/O

`json.c` に手書き再帰下降パーサ + pretty-printer。

- パーサは `(src, len, *endp, *errmsg) → VALUE`。`endp` を進めるので
  「JSON のストリーム」 (空白区切りで複数 value) を `while` で消費可。
- pretty-printer は jq 互換の数値整形:
  - 整数値の double は `%" PRId64 "` で integer 表記
  - それ以外は `%.15g → %.16g → %.17g` の最短 round-trip
- 文字列は ASCII printable をそのまま、制御コードは `\uXXXX` で escape
  (jq と同じ)。
- `\uXXXX` サロゲートペアは UTF-8 byte 列に decode する。

## 8. ASTro / Code Store

`INIT()` で `astro_cs_init("code_store", ".", 0)` を呼ぶ。`main.c` は
parser の出力 AST を `astro_cs_compile` → `astro_cs_build` →
`astro_cs_reload` → `astro_cs_load` に通して dispatcher を SD に差し替える
(`--no-compile` で skip)。

n が小さくて hot loop が `runtime.c` 内の helper 側にあるため、現状
SD specialization の効きは控えめ (todo B1, B2 を参照)。SD は確実に
焼ける — `code_store/c/SD_<hash>.c` が出る。

`astro_cs_build` の `make` が ccache 経由で落ちる環境では
`CCACHE_DISABLE=1` を渡す (project memory `feedback_ccache_disable`)。

## 9. 参考

- フレームワーク全体: [`../../docs/idea.md`](../../docs/idea.md)
- Code Store の罠: [`../../docs/code_store_quirks.md`](../../docs/code_store_quirks.md)
- 別流儀の参考: [`sample/astrogre`](../astrogre/) は continuation passing 流、
  [`sample/pystro`](../pystro/) は class + 多 dunder 系の tree walker。
