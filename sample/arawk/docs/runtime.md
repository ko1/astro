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
| `context.h`    | VALUE エンコード / `arawk_obj` / CTX / RESULT / 公開 API / 算術 fast path |
| `node.c`       | アロケータ + ASTroGen 生成ファイルの `#include` 集約 |
| `runtime.c`    | VALUE 構築 / 強制変換 / 連想配列 / 出力 / 入力 / getline / printf 等 |
| `parse.c`      | tokenizer + 再帰下降 / Pratt パーサ (`PARSE_SOURCE`) |
| `main.c`       | CLI driver / option parser / `code_repo_add` stub / `arawk_resolve_body` |

ASTroGen が生成: `node_alloc.c` `node_dispatch.c` `node_eval.c` `node_dump.c`
`node_hash.c` `node_specialize.c` `node_replace.c` `node_head.h`。

## 1. 値モデル

```
xxxx_xxx1 → 63-bit signed fixnum (左 1 シフト + 1)
xxxx_xxx0 → ヒープオブジェクト (`struct arawk_obj *`、8-byte aligned)
```

`ARAWK_UNINIT` は静的 singleton (グローバル `struct arawk_obj ARAWK_UNINIT_OBJ`
のアドレス)。fixnum 範囲外の整数は heap-boxed double に昇格。

```c
struct arawk_obj {
    int type;          /* UNINIT / FLOAT / STRING / STRNUM / ARRAY */
    union {
        double dbl;
        struct { char *chars; size_t len; } str;
        struct arawk_array arr;
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

    struct arawk_record rec;             /* 現レコード状態 */

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

## 5. レコードとフィールド (lazy strnum)

```c
struct arawk_record {
    char    *record;          /* $0 raw bytes (NUL-terminated) */
    size_t   record_len;
    VALUE    record_v;        /* $0 を VALUE で見たキャッシュ */
    /* Lazy field representation. */
    int     *field_starts;    /* record 内 offset (atomic alloc) */
    int     *field_lens;      /* 長さ                            */
    VALUE   *fields;          /* lazy: 0 = 未生成 sentinel        */
    int      nf;
    int      fields_capa;
    bool     fields_split;
};
```

**lazy 分割**: `arawk_split_fields` は **境界 (offset, length) を 3 つの
並列配列に記録するだけ** で、 strnum VALUE は作らない。 `$N` の初回読み
で `arawk_get_field` が `arawk_make_strnum(record + start, len)` を作って
`fields[N-1]` に cache。 `fields[i] == 0` が「未生成」 sentinel (heap ptr
も `ARAWK_FIX(0) = 1` も非ゼロなので衝突しない)。

これで `{ wc += NF }` のような NF だけ使うスクリプトや `$2` 1 個だけ
読むスクリプトが全 field strnum を allocate せずに済む。 ベンチで
tt.07 (NF だけ) は 3× 改善、 GC pressure が 1/5 になった。

入力ループ (`arawk_input_next_record`) が新レコードを読むとき:

1. RS 区切り (現状 `\n` 固定) で 1 レコード読み (§7 の chunked reader 経由)
2. `c->rec.record` / `record_len` を更新、`record_v = 0` (キャッシュ無効化)
3. `NR` / `FNR` を更新
4. `arawk_split_fields(c)` で eager に **boundary だけ** 記録 (= `NF` を即決定、 strnum は作らない)

`arawk_split_fields` は `c->rec.fields_split` が false のときだけ走る。 `FS` への
代入で false に戻り、 次の `$N` 読み時に再分割される。 `fields[]` も同時
にクリアされて lazy 状態に戻る。

`$N = v` での代入は `arawk_set_field(c, N, v)` 経由で:
1. 3 並列配列を `arawk_grow_fields(c, N-1)` で拡張
2. 不足フィールドの埋め: `field_starts[i] = 0, field_lens[i] = 0,
   fields[i] = 0` → lazy で `""` が生成される
3. NF を更新
4. `$0` を OFS で再構築 (`arawk_rebuild_record`)、 `arawk_get_field`
   経由で必要な field を materialise

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

frame ベース inline cache は未実装だが、 perf record で `arawk_resolve_body`
の strcmp ループは hot じゃないことが判明 — 改善案 #4 (callcache) は
仮説外れで採用見送り。 `tt.14_function_call` は for-in iterator 化 (§6.5)
で 0.50× → 2.56× に。

## 7. 入出力ストリーム (chunked read)

### 出力 (`print | "cmd"`, `print > "file"`, `printf` 同上)

```c
static struct arawk_stream *arawk_streams = NULL;       /* hash 表代わりの線形配列 */

FILE *arawk_open_stream(int mode, VALUE dest);        /* 'w'=popen, 'o'=fopen w, 'a'=fopen a */
```

`node_print` / `node_print_to` / `node_printf_to` は `mode` + dest を
引数に持ち、 runtime で stream を lookup-or-open する。 プロセス終了
時に `arawk_close_all_streams` が `pclose` / `fclose` を呼ぶ —
pipe (sort 等) に EOF を渡して下流を完走させるために必要。

### 入力 — chunked reader

```c
struct arawk_rdbuf { char *data; size_t capa, len, pos; };  /* per-stream 64 KB */

static int arawk_read_line_buf(FILE *fp, struct arawk_rdbuf *rb,
                               char **out, size_t *out_len, size_t *out_capa);
```

入力は **per-FILE** に 64 KB の chunk バッファを持ち、 `fread` で
丸ごと読んで `memchr('\n')` で行末を探す。 fgetc を 1 文字ずつ
PLT 越しに呼ぶ旧実装より `_IO_getc` (18% hot) を完全に消せた。

- `arawk_streams[]` / `arawk_inputs[]` の各 entry が rdbuf を保有
- cur_input (input loop) 用は静的 `cur_input_rdbuf`、 `arawk_open_next_input` で
  ファイル切替時に reset (`nextfile` 後の旧 buffer 残骸を捨てる)

### getline 6 形態

```c
static struct arawk_stream *arawk_inputs = NULL;        /* 入力側 cache */

FILE *arawk_open_input(int mode, VALUE dest);       /* 'r'=popen, 'i'=fopen r */
```

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

`arawk_wrap_string(chars, len)` (context.h) は **既存の GC-traced buffer を
shareする zero-copy wrapper**。 caller が buffer の所有権を保持し続ける
ケース (典型: `for-in` で entry->key を share) で arawk_obj だけ
allocate して `chars` を共有する。 conservative scanner が wrap obj
経由で元 buffer を keep するので alive 期間も自動保証。

## 10. 文字コード

```c
typedef enum { ARAWK_ENC_BYTE = 0, ARAWK_ENC_UTF8 = 1 } arawk_encoding_t;
extern arawk_encoding_t ARAWK_ENCODING;
```

gawk と同じ **LC_CTYPE 自動判定**:

1. main.c で `setlocale(LC_CTYPE, "") + nl_langinfo(CODESET)`
2. 結果に `UTF-8` / `utf-8` / `UTF8` / `utf8` が含まれれば `ENC_UTF8`
3. `--byte` / `--posix` CLI flag で override (→ `ENC_BYTE`)
4. プロセス全体で 1 つ; 文字列 VALUE には encoding tag を持たせない

UTF-8 helper (`runtime.c`):

```c
static size_t arawk_utf8_char_count(const char *s, size_t bytes);
static size_t arawk_utf8_byte_at_char(const char *s, size_t bytes, size_t char_pos);
static size_t arawk_utf8_char_at_byte(const char *s, size_t bytes, size_t byte_pos);
```

`arawk_utf8_char_count` は **8 byte word stride の ASCII fast path** 付き:

```c
const uint64_t high_bit_mask = 0x8080808080808080ULL;
while (i + 8 <= bytes) {
    uint64_t w; memcpy(&w, s + i, 8);
    if (w & high_bit_mask) goto utf8_slow;
    i += 8;
}
/* 全 ASCII → byte 数 = codepoint 数で即 return */
```

全 ASCII の入力 (tt.* の foo.td 等) では 1 pass で済む。 UTF-8 mode に
することによる regression は geomean -0.02 (2%) の範囲。

`length` / `substr` / `index` が `ARAWK_ENCODING` で分岐。 `tolower` /
`toupper` は POSIX 仕様準拠範囲 = ASCII のみで処理 (gawk extension の
non-ASCII 大小化は未対応)。

Phase 2 で astrogre 統合時、 `ARAWK_ENCODING` をそのまま
`AGRE_ENC_UTF8` / `AGRE_ENC_ASCII` に渡せば regex も encoding-aware に。

## 11. AOT (Code Store)

`-c` フラグで `OPTIMIZE` 時に `astro_cs_compile` → `astro_cs_build` →
`astro_cs_reload` のサイクルが走り、特定の AST サブツリーが C コード
として specialize されて `code_store/all.so` に bake、 dlopen で reload
される。各 NODE の `head.dispatcher` が specialized 関数を指すように
fixup される。

効くケース: tree-walking dispatch + node-to-node inlining が hot な
ループ。 `tt.x2_sum_loop` (BEGIN 10M 回 fixnum 加算) で AOT 1.92× vs gawk。

効かないケース: runtime helper (PLT call) が hot path のとき。 ただし
B (lazy strnum) / A (chunked input) / C (for-in walker) で runtime
helper 側のコストが大幅に減ったため、 改善前は AOT が活きなかった
tt.* テストも軒並み AOT で更に伸びるようになった。 例: tt.x2_sum_loop
の AOT 効果は plain 0.392s → AOT 0.275s (30% 短縮)。

詳細は [`perf.md`](perf.md)。

## 12. 名前空間 prefix

将来の Phase 2 (astrogre 統合) で 1 binary に 2 つの AST interpreter を
並存させる際の symbol 衝突を避けるため、 識別子の規約を以下で統一済:

| 種類 | prefix | 例 |
|---|---|---|
| AST ノード | `node_*` | `node_add`, `node_lget`, `node_getline_cur` |
| ASTroGen 生成 | `ALLOC_node_*` 等 | `ALLOC_node_add`, `DISPATCH_node_lget` |
| Runtime 関数 / static global / struct / enum | `arawk_*` | `arawk_to_num`, `arawk_arr_get`, `struct arawk_obj` |
| マクロ / 定数 / public extern | `ARAWK_*` | `ARAWK_FIX`, `ARAWK_T_STRING`, `ARAWK_GLOB_NR`, `ARAWK_NODE_TABLE`, `ARAWK_CURRENT_CTX` |

ソース内 `\bawk_[a-z]` で始まる識別子は存在しない (検証済)。 Phase 2 で
astrogre と並ぶ際、 astrogre 側は `agre_*` / `AGRE_*` で対称、 ノード名は
両者とも `node_*` で AST マージ可能な状態。
