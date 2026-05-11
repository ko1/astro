# runtime.md — arawk の実装詳解

arawk は ASTro 上に乗せた **POSIX awk subset の tree-walking interpreter**。
基本的な構造は astr (R subset) に倣い、LSB-tagged VALUE + Boehm-Demers-
Weiser GC + 3-arg dispatcher (`CTX *, NODE *, VALUE *fp`) を採用。POSIX
awk 固有のパターン-アクションループ・フィールド分割・複雑な I/O (pipe /
getline / printf redirect) を専用ノード + runtime helper で実装する。

## ファイル構成

| ファイル | 内容 |
|---|---|
| `node.def`     | 全 AST ノード定義 (literal / 算術 / 制御 / array / 関数 / I/O 等) |
| `arawk_gen.rb`   | ASTroGen 用 NodeDef サブクラス (operand 型カスタマイズ) |
| `node.h`       | NodeHead 宣言 + EVAL inline + 生成 `node_head.h` を include |
| `context.h`    | VALUE エンコード / `awk_obj` / CTX / RESULT / 公開 API / 算術 fast path |
| `node.c`       | アロケータ + ASTroGen 生成ファイルの `#include` 集約 |
| `runtime.c`    | VALUE 構築 / 強制変換 / 連想配列 / 出力 / 入力 / getline / printf 等 |
| `parse.c`      | tokenizer + 再帰下降 / Pratt パーサ (`PARSE_SOURCE`) |
| `main.c`       | CLI driver / option parser / `code_repo_add` stub / `arawk_resolve_body` |

ASTroGen が生成: `node_alloc.c` `node_dispatch.c` `node_eval.c` `node_dump.c`
`node_hash.c` `node_specialize.c` `node_replace.c` `node_head.h`。

## 1. 値モデル

```
xxxx_xxx1 → 63-bit signed fixnum (左 1 シフト + 1)
xxxx_xxx0 → ヒープオブジェクト (`struct awk_obj *`、8-byte aligned)
```

`ARAWK_UNINIT` は静的 singleton (グローバル `struct awk_obj ARAWK_UNINIT_OBJ`
のアドレス)。fixnum 範囲外の整数は heap-boxed double に昇格。

```c
struct awk_obj {
    int type;          /* UNINIT / FLOAT / STRING / STRNUM / ARRAY */
    union {
        double dbl;
        struct { char *chars; size_t len; } str;
        struct awk_array arr;
    };
};
```

| `type`           | 用途 |
|---|---|
| `ARAWK_T_UNINIT`   | singleton; 数値文脈で 0、文字列文脈で `""` |
| `ARAWK_T_FLOAT`    | double 値 |
| `ARAWK_T_STRING`   | 純粋な文字列 (リテラルや関数の戻り値) |
| `ARAWK_T_STRNUM`   | フィールド / getline の入力 — 数値形なら数値、 そうでなければ文字列 |
| `ARAWK_T_ARRAY`    | 連想配列 (FNV-1a + chained bucket; load > 0.75 で rehash) |

awk の数値 / 文字列二面性は `arawk_to_num` / `arawk_to_cstr` / `val_is_numeric`
が引き受ける。`STRNUM` のみ「数値らしき形」かを毎回 strtod で判定する
(`val_is_numeric` 参照)。

strtod は C99 で `"inf"` / `"infinity"` / `"nan"` を recognise するが
awk 仕様では「先頭から数字でなければ 0」が正しい。`arawk_to_num` で
sign-digit / `.digit` の prefix check を入れて回避済。

## 2. RESULT プロトコル

各 NODE_DEF は `RESULT { VALUE value; unsigned state; }` を返す。`state`
で非局所脱出を表現:

| state             | 効果 |
|---|---|
| `RESULT_NORMAL`   | 通常の値返却 |
| `RESULT_NEXT`     | `next` — 現レコード残りスキップ |
| `RESULT_NEXTFILE` | `nextfile` — 現入力ファイル残スキップ |
| `RESULT_EXIT`     | `exit [n]` — END を経由してプロセス終了 |
| `RESULT_BREAK`    | `break` |
| `RESULT_CONTINUE` | `continue` |
| `RESULT_RETURN`   | 関数からの `return` |

`UNWRAP(EVAL_ARG(c, node))` マクロが NORMAL 以外を**呼び出し元に
return** することで伝搬する (setjmp なし、各 NODE_DEF がカスケード)。

```c
#define UNWRAP(r) ({                                 \
    RESULT _r = (r);                                 \
    if (UNLIKELY(_r.state != RESULT_NORMAL)) return _r; \
    _r.value;                                        \
})
```

`node_main_loop` だけは特別で、レコードループの中で NEXT/NEXTFILE を
catch して continue する。EXIT は親へ伝搬。

## 3. 算術 fast path

`context.h` に `static inline` で fixnum 高速路、slow path は `_slow`
接尾辞で `runtime.c`:

```c
static inline VALUE
arawk_add(VALUE a, VALUE b) {
    if (LIKELY(ARAWK_IS_FIX(a) & ARAWK_IS_FIX(b))) {
        int64_t la = ARAWK_FIX_VAL(a), lb = ARAWK_FIX_VAL(b), r;
        if (LIKELY(!__builtin_add_overflow(la, lb, &r) &&
                   r <= ARAWK_FIX_MAX && r >= ARAWK_FIX_MIN)) {
            return ARAWK_FIX(r);
        }
    }
    return arawk_add_slow(a, b);
}
```

inline 対象: `add` / `sub` / `mul` / `neg`。`div` / `mod` / `pow` は
awk 仕様で常に double 演算なので fast path なし。

AOT bake (`-c`) で SD specialize されたコードは inline fast path をそのまま
emit し、 hot loop (例: `tt.x2_sum_loop`) を `lea`/`add` 数命令に畳む。

## 4. CTX

```c
typedef struct CTX_struct {
    VALUE   *env;                      /* グローバル変数の配列 (~4096) */
    VALUE   *fp;                       /* 現フレーム; トップレベルは fp == env */

    struct awk_record rec;             /* 現レコード状態 */

    struct function_entry *func_set;   /* user function テーブル */
    unsigned int  func_set_cnt;

    FILE   *cur_input;                 /* 現入力ファイル */
    int     cur_input_idx;
    bool    input_done;
} CTX;
```

`env[0..ARAWK_GLOB_RESERVED-1]` (= 16 slot) は **特殊変数の固定 slot**:

| slot | 名前 |
|---:|---|
| 0 | `NR` |
| 1 | `NF` |
| 2 | `FS` |
| 3 | `OFS` |
| 4 | `ORS` |
| 5 | `RS` |
| 6 | `FILENAME` |
| 7 | `FNR` |
| 8 | `SUBSEP` |
| 9 | `CONVFMT` |
| 10 | `OFMT` |
| 11 | `RSTART` |
| 12 | `RLENGTH` |
| 13 | `ENVIRON` |
| 14 | `ARGC` |
| 15 | `ARGV` |
| 16+ | ユーザ global |

特殊変数を **普通の global slot 経由**で扱うことで、`NF = 5` のような代入
は通常の `node_gset` パスを通る。 `gset` は FS / NF への代入を
hook して、フィールド分割の invalidate や `$0` 再構築を起こす。

CTX グローバルへの単一参照 (`ARAWK_CURRENT_CTX`) も持つ。`arawk_to_cstr`
等 CTX を引数で受け取らない runtime helper が `CONVFMT` を読むために使う。

## 5. レコードとフィールド

```c
struct awk_record {
    char    *record;          /* $0 raw bytes (NUL-terminated) */
    size_t   record_len;
    VALUE    record_v;        /* $0 を VALUE で見たキャッシュ */
    VALUE   *fields;          /* $1..$NF の strnum */
    int      nf;
    int      fields_capa;
    bool     fields_split;    /* 分割済か */
};
```

入力ループ (`arawk_input_next_record`) が新レコードを読むとき:

1. RS 区切り (現状 `\n` 固定) で 1 レコード読み
2. `c->rec.record` / `record_len` を更新、`record_v = 0` (キャッシュ無効化)
3. `NR` / `FNR` を更新
4. `arawk_split_fields(c)` で eager に FS 分割 (= `NF` を即決定)

`arawk_split_fields` は `c->rec.fields_split` が false のときだけ走る。FS への
代入で false に戻し、次の `$N` 読み時に再分割される。

`$N = v` での代入は `arawk_set_field(c, N, v)` 経由で:
1. fields[] が必要なら拡張、不足するフィールドを `""` で埋める
2. NF を更新
3. `$0` を OFS で再構築 (`arawk_rebuild_record`)

`NF = N` 代入は `arawk_set_nf` 経由で同様にフィールド配列を伸縮 + 再構築。

## 6. ユーザー関数

astr の callcache パターンを未だ採用していない簡易版:

```c
NODE_DEF node_def(c, n, fp, name, body, frame_size, params_cnt)
{
    fe = arawk_get_func_entry(c, name);
    fe->name = name; fe->body = OPTIMIZE(body);
    fe->params_cnt = params_cnt; fe->locals_cnt = frame_size;
}

NODE_DEF node_call_user(c, n, fp, name, base_args, argc)
{
    body = arawk_resolve_body(c, name, &params_cnt);  /* strcmp ループ */
    VALUE F[ARAWK_FRAME_MAX];                          /* VLA frame */
    F[0..argc-1] = 引数評価結果
    F[argc..MAX-1] = ARAWK_UNINIT
    return EVAL(c, body, F);                           /* fp 切替 */
}
```

関数本体内では parser が「**local 名は `fp[slot]` 参照、global 名は
`c->env[slot]` 参照**」を区別する emit helper (`emit_var_get`, `emit_arr_set`,
`emit_postinc` 等) を介してノードを発行。 ローカルは `*_l`、 グローバルは
`*_g` 接尾辞のノード (例: `node_lget` / `node_gget`)。

frame ベース inline cache 未実装のため `tt.14_function_call` は遅い (0.45×
vs gawk)。callcache 実装は perf.md の改善案 #4。

## 7. 入出力ストリーム

### 出力 (`print | "cmd"`, `print > "file"`, `printf` 同上)

```c
static struct awk_stream *arawk_streams = NULL;       /* hash 表代わりの線形配列 */

FILE *arawk_open_stream(int mode, VALUE dest);        /* 'w'=popen, 'o'=fopen w, 'a'=fopen a */
```

`print` / `print_to` / `printf_to` ノードは `mode` + dest を引数に持ち、
runtime で stream を lookup-or-open する。プロセス終了時に
`arawk_close_all_streams` が `pclose` / `fclose` を呼ぶ — pipe (sort 等)
に EOF を渡して下流を完走させるために必要。

### 入力 (`getline`)

```c
static struct awk_stream *arawk_inputs = NULL;        /* 入力側 cache */

FILE *arawk_open_input(int mode, VALUE dest);       /* 'r'=popen, 'i'=fopen r */
```

`getline` 6 形態それぞれに対応するノード:

| 構文                | ノード                                |
|---|---|
| `getline`           | `node_getline_cur`              |
| `getline NAME`      | `node_getline_cur_l/_g`         |
| `getline < expr`    | `node_getline_file`             |
| `getline NAME < expr` | `node_getline_file_l/_g`      |
| `expr \| getline`   | `node_getline_cmd`              |
| `expr \| getline NAME` | `node_getline_cmd_l/_g`       |

POSIX の更新仕様 (NR/FNR/$0/NF/FILENAME のどれを更新するか) は
`runtime.c` の `arawk_getline_*` ヘルパが内部で正しい組み合わせを実行。

`close(name)` は output / input の両 cache を順に検索する。

## 8. パーサーの構造

`parse.c` は tokenizer + 再帰下降パーサ + Pratt スタイルの式パーサ:

1. **tokenizer**: 字句解析 (peek/take + pushback stack 深さ 4)
2. **globals_intern**: 識別子 → env slot 番号の解決
3. **LocalScope**: 関数本体内のローカル名解決
4. **resolve_name**: 「local scope → globals」の順で名前を解決し `Var{is_local, slot}` を返す
5. **emit_*ヘルパ**: `Var` を見てローカル / グローバル系ノードを発行
6. **Pratt chain**: ternary → or → and → in → rel → concat → add → mul → unary → pow → primary
7. **continuation 関数**: `_continue` 接尾辞で「事前パース済の lhs から climb」する。 `NAME[k]` を assignment / rvalue で分岐するときに使う

`getline` は parse_primary (先頭が getline) と parse_concat_continue (式の中の `cmd | getline`) の **2 箇所**で受け付ける。`|` トークンは expression-level では cmd-getline 専用、 statement-level (print 直後) では出力 redirect。

## 9. メモリ管理

**Boehm-Demers-Weiser conservative GC** (libgc) を使う。astr / koruby と
同じ。

- `GC_malloc(sz)`: 内部にポインタを含み得る領域 (デフォルト)
- `GC_malloc_atomic(sz)`: ポインタを含まない領域 (char buffer / double[] 等)
- `GC_realloc`: 再確保

明示 free なし。`ARAWK_UNINIT_OBJ` 等の singleton はグローバル static で
libgc の管理外。

GC roots:
- C stack (libgc が conservative scan する)
- `parse_ctx` 等のグローバル
- `CTX->env` / `CTX->func_set` / 各種 stream cache

性能影響:
- フィールド分割で毎レコード strnum allocate → tt.03 系 (sum_field) が遅い
- `substr` / `concat` で毎回 fresh string → tt.11 が遅い

改善案: フィールドの lazy strnum、 substr の copy-on-write (perf.md 改善案
#2, #3)。

## 10. AOT (Code Store)

`-c` フラグで `OPTIMIZE` 時に `astro_cs_compile` → `astro_cs_build` →
`astro_cs_reload` のサイクルが走り、特定の AST サブツリーが C コード
として specialize されて `code_store/all.so` に bake、 dlopen で reload
される。各 NODE の `head.dispatcher` が specialized 関数を指すように
fixup される。

効くケース: tree-walking dispatch + node-to-node inlining が hot な
ループ。`tt.x2_sum_loop` (BEGIN 10M 回 fixnum 加算) で AOT 1.90× vs gawk。

効かないケース: runtime helper (PLT call) が hot path のとき。`tt.01_print`
(fwrite per item)、`tt.03_sum_length` (毎行 split + GC malloc)、
`tt.11_substr` (毎回 string allocate)、`tt.14_function_call` (関数 lookup
strcmp) 等。これは SD bake してもインライン展開できない領域。

詳細は [`perf.md`](perf.md)。

## 11. 名前空間 prefix

将来の Phase 2 (astrogre 統合) で 1 binary に 2 つの AST interpreter を
並存させる際の symbol 衝突を避けるため、 ノード名は **`arawk_node_*`** で
統一済 (e.g. `node_add`, `node_call_user`)。ASTroGen が
生成する `ALLOC_*` / `EVAL_*` / `DISPATCH_*` / `HASH_*` / `NodeKind`
enum 値も自動的に `arawk_node_*` prefix で出る。

runtime helper は `awk_*` prefix (e.g. `arawk_arr_get`, `arawk_getline_cur`)。
astrogre 側の `agre_*` と衝突しない。
