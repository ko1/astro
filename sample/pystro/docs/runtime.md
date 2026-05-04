# runtime.md — pystro のランタイム解説

pystro は ASTro 上に乗せた Python サブセットの tree-walking インタプリタ。
v0 時点で動く範囲は [done.md](./done.md)、未実装は [todo.md](./todo.md) を参照。

ファイル構成は ascheme と同じ:

| ファイル | 内容 |
|---|---|
| `node.def`   | AST ノード定義 (定数 / 変数 / 算術 / 比較 / 制御 / 呼出) |
| `context.h`  | `VALUE` 表現、`CTX`、シングルトン、option 構造体 |
| `node.h`     | `NodeHead` / 公開 API |
| `node.c`     | アロケータ + ASTroGen 生成ファイルの `#include` |
| `main.c`     | ヒープ / globals / apply / builtins / lexer / parser / driver |

## 1. VALUE 表現

```
xxxx_xxx1 → fixnum (signed 63-bit、左 1 シフト + 1)
xxxx_xxx0 → ヒープオブジェクト (`struct pyobj *`、8-byte aligned)
```

`True` / `False` / `None` は **静的に確保された singleton `pyobj`** で、
そのアドレスをそのまま `VALUE` として配る (`PY_TRUE` / `PY_FALSE` / `PY_NONE`)。
ascheme の `S_NIL_OBJ` 系と同じパターン。

ヒープ型は `enum pyobj_type` の `PY_T_*`:
`NONE / BOOL / FLOAT / STR / FUNC / BUILTIN / LIST(reserved)`。

float は **常にヒープ box**。NaN-boxing / inline flonum は v0 では入れていない
(導入する場合は CRuby / luastro と同じスキーム — `todo.md` 参照)。
fixnum オーバーフローは現状エラーではなく `py_add` の slow path に落ち、
double に格上げされる。bignum は無いので大きな数値計算は精度が落ちる。

## 2. 実行コンテキスト (`CTX`)

```c
typedef struct CTX_struct {
    struct pyframe *env;            // 現フレーム (top-level では NULL)
    struct gentry  *globals;        // 線形リスト + serial 番号
    size_t globals_size, globals_capa;
    uint64_t globals_serial;

    int    state;                   // PY_STATE_NORMAL / RETURN / RAISE
    VALUE  state_value;             // return 値 / 例外ペイロード

    jmp_buf err_jmp;
    int     err_jmp_active;
} CTX;
```

`state` は **return 伝播用のフラグ**。`node_return` が `state = RETURN; state_value = v`
を立て、上位の `node_seq` / `node_if` / `node_while` が `state != NORMAL` を見て
即座に return する。これで関数境界まで一直線に巻き戻る。
`longjmp` を使わないのは将来 try/except を入れたときに同じ仕組みで例外を扱いたいから。

## 3. フレームと変数解決

ローカルは `struct pyframe` の flat な `slots[nlocals]` に固定 index でアクセス。
parser が `def` を読む時に **suite を pre-scan** し、`NAME =` の左辺と仮引数を
ローカルとして登録する (Python の「関数内で代入された名前は local」ルールの近似)。

- 関数内の名前読み: parser scope に居ればその index で `node_lref(idx)`、
  そうでなければ `node_gref(name)`。
- 関数内の代入: 同様に `node_lset(idx, rhs)` か `node_gset(name, rhs)`。
- v0 ではネストした `def` での自由変数キャプチャは parser がエミットしないため
  「内側関数が外側関数のローカルを読む」と global lookup となり実行時エラー。
  closure capture 自体は `pyframe.parent` で繋ぐ仕組みは入れてあるので、
  parser を nonlocal-aware に拡張すれば動くようにはなっている。

global は線形配列。`name == name (pointer 比較)` ではなく `strcmp` で引いているが、
名前は **intern 済み** (`intern_name`) なので将来は識別子 → globals index の
inline cache (`@ref`) を入れて高速化できる (ascheme の `gref_cache` 相当)。
今は v0 のため未着手。

## 4. 関数呼び出し

`def name(p1, p2, ...): body` は parse 時に **クロージャオブジェクトを 1 個確保**
して global に登録するノード `node_def(name, nparams, nlocals, body)` を作る。
実行時 (`EVAL`):

1. `py_make_func(body, env=c->env, name, nparams, nlocals)` で `pyobj` を作る。
2. `py_global_define(c, name, fn)`。

呼び出し `f(a, b)` は arity に応じて `node_call_0/1/2/3` または可変 `node_call_n`
が `py_apply(c, fn, argc, argv)` を呼ぶ:

1. 新しい `pyframe` を `nlocals` 分確保し `slots[0..nparams) = argv` で埋める。
2. `c->env` を退避して新フレームに切り替え、`EVAL(body)`。
3. `state == RETURN` なら `state_value` を取り出して `state` をリセット、return。
   それ以外 (本体走り抜けた) は `None` を返す。
4. `c->env` を復元。

builtin (`py_make_builtin`) は同じ `py_apply` から条件分岐で呼ばれる。
組み込みは `print` / `str` / `int` / `float` / `len` / `abs` の 6 個 (v0)。

## 5. ノードの種類 (node.def)

| グループ | ノード |
|---|---|
| 定数      | `const_int / const_int64 / const_float / const_str / const_none / const_true / const_false` |
| 変数      | `lref / lset / gref / gset` |
| 単項      | `neg / not` |
| 算術      | `add / sub / mul / floordiv / mod` (fixnum fast path inline、それ以外は `py_*` ヘルパ) |
| 比較      | `lt / le / gt / ge / eq / ne` (fixnum fast path inline) |
| 論理      | `and / or` (short-circuit、Python 仕様で「決め手の operand」を返す) |
| 制御      | `if / while / seq / nop / return` |
| 呼出      | `def / call_0 / call_1 / call_2 / call_3 / call_n` |

`@noinline` を付けているのは `node_def` (本体は parser-time に確定するクロージャ生成 1 回きり)
と `node_call_n` (子は `PYSTRO_CALL_ARGS[]` の動的読みで SD specialize できない) の 2 つ。
固定 arity の `call_K` は子 NODE が typed `NODE *` operand なので SD が完全に
inline 展開でき、specialise 後はディスパッチ rep + 子の SD 群が
1 つの basic block に畳まれる (ascheme の同パターンと同じ理屈)。

## 6. parser

- **lexer がトークン列を全部メモリに乗せる**。indent 追跡で `INDENT` / `DEDENT`
  を生成、`#` コメント無視、`\n` で `NEWLINE` (paren_depth > 0 のときは抑制)。
- 続いて recursive-descent parser がトークンを舐めて AST を作る。Pratt 風だが
  優先順位は手書きの分割関数 (`parse_or → parse_and → parse_not → parse_compare → parse_arith → parse_term → parse_unary → parse_postfix → parse_atom`)。
- `def` を読むときは body の token range を `find_suite_end` で特定して
  **lvalue を pre-scan** し scope に登録、その後改めて parser を回す。
- v0 で扱える文: `def / if/elif/else / while / return / pass / 代入 / 式文`。

## 7. Code Store / AOT / JIT

`OPTIMIZE` が `astro_cs_load` を呼ぶ標準パターン (ascheme と同じ)。
`-c` で起動すると AST 構築直後に `astro_cs_compile + build + reload` で SD を焼き、
その run からそのまま使う。`--aot-compile` は焼いて exit。
PG / JIT は v0 では未着手。
