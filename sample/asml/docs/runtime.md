# asml ランタイム構造

asml は ASTro 上の **HM 型推論つき** SML サブセットインタプリタ。
本書ではコンパイル時パイプラインと実行時データ構造、特に「型情報が
どこでどう使われて動的チェックが消えるか」に焦点を当てる。

```
*.sml ファイル
  │ lex (手書き)
  ▼
TK_* トークン列
  │ parse (recursive descent, 手書き)
  ▼
struct expr 木 (EX_* IR; 各ノードが line + ty フィールドを持つ)
  │ infer  (Algorithm W、levels で let-poly)
  ▼
struct expr (各 ex->ty が確定; 失敗時は exit 2)
  │ lower_expr (ex->ty を見て node_*_int / node_if_bool / ... を選択)
  ▼
NODE 木 (head + union u, ALLOC_node_*)
  │ EVAL(c, node) = (*n->head.dispatcher)(c, n)
  ▼
VALUE  (1-bit tagged int64_t)
```

## 1. 値表現 (`VALUE`)

```c
typedef int64_t VALUE;
```

下位 1 bit で fixnum / heap pointer を分ける:

| パターン | 意味 |
|---|---|
| `xxxx_xxx1` | 63-bit 整数 (`ML_INT_VAL(v) = v >> 1`) |
| `xxxx_xxx0` | 8-byte aligned `struct mlobj *` または静的シングルトン |

シングルトンは静的 `mlobj` のアドレス: `ML_UNIT_OBJ`, `ML_TRUE_OBJ`,
`ML_FALSE_OBJ`, `ML_NIL_OBJ`。bit 0 が 0 なので heap pointer と区別不要。

### ヒープオブジェクト

```c
struct mlobj {
    int type;
    union {
        bool b;
        double dbl;
        VALUE refval;                                              // MLOBJ_REF
        struct { VALUE head, tail; } cons;                         // MLOBJ_CONS
        struct { char *chars; size_t len; } str;                   // MLOBJ_STRING
        struct { struct Node *body; struct mlframe *env;
                 int nparams; bool is_leaf; const char *name;
        } closure;                                                  // MLOBJ_CLOSURE
        struct { ml_prim_fn fn; const char *name;
                 int min_argc, max_argc; } prim;                   // MLOBJ_PRIM
        struct { int n; VALUE *items; } tup;                       // MLOBJ_TUPLE
        struct { const char *name; int n; VALUE *items; } var;     // MLOBJ_VARIANT, MLOBJ_EXN
    };
};
```

各タイプを単一 union に詰めるのは astocaml と同じ妥協 (size の最大型に
合わせて全オブジェクトがそのサイズになる)。production 化するなら type
ごとに別 struct で cache 圧を下げるべき。

## 2. 環境 (`mlframe` チェーン)

クロージャは作成時の環境をキャプチャ。レキシカルフレームは:

```c
struct mlframe {
    struct mlframe *parent;
    int             nslots;
    VALUE           slots[];
};
```

`CTX *c` に現フレーム `c->env` を持つ。変数参照は parse 時に
`(depth, idx)` ペアにコンパイルされ、`node_lref` は

```c
ml_env_at(c, depth)->slots[idx]
```

を返す。`depth` 段、parent を辿る。

「**パーサのスコープ ≡ ランタイムフレーム**」の原則が常に成り立つよう
注意して書く必要がある:

- `match_arm` は arity > 0 の時だけランタイム frame を push する。
  パーサの `parse_match_chain` も同条件で `scope_push()` する。
- `case` は `let scrut = value in arms end` に lower されるので、
  パーサは `parse_case` で `$scrut` の 1-slot scope を push する。
- `handle` arms 内では raised value が新 frame の slot 0 にあるので、
  パーサは `$exn` の 1-slot scope を push する。

これがズレると **lref の depth が壊れて任意の値を読む** ので慎重。

## 3. 型推論器

実装は `infer(level, e)` (約 250 行) + `infer_pat_walk` (約 100 行)。
mutable union-find で `struct ty` の `var.link` を辿る (path
compression あり)。

### レベル法 (Remy)

各 `TYK_VAR` は `level` を持つ (生成時の let-binding 深さ)。`let` の
RHS は `level + 1` で infer し、終わったら `level` で `ty_generalize`:
**`var.level > level` の var だけが量化される**。

外側スコープから内側に逃げた var (`level <= 外側 level`) は決して
量化されない。これで type-safe な let-poly になる。

### 値制約 (value restriction)

`ex_is_value(e)` が syntactic value (literal / lambda / ctor of values
/ tuple of values / lref / gref) を判定し、true の時のみ generalize。
それ以外 (`val r = ref []` 等) は monomorphic に残す。

### スキーム

```c
struct ty_scheme {
    int   n_quants;     // 量化された var の個数 (q_idx で 1..n_quants)
    TY   *body;
};
```

generalize は body を walk して `var.q_idx` に index を振る。`instantiate`
は q_idx → 新しい `ty_var(level)` の map を作って body を coppy し直す。

### コンストラクタ

`ctor_ty_register(name, arity, is_exn, scheme)` で各 ctor を登録。
組み込みは `install_prelude` で登録 (NONE, SOME, Match, Div, Empty, Fail)。
ユーザの `datatype 'a t = Foo of T | Bar` は `process_datatype()` が
parse して登録 — type vars は `'a` 等の名前を `tyvar_env` で管理し、
ctor ごとに fresh tyvar を allocate する (詳細は main.c コメント)。

## 4. lower_expr — 型駆動ノード選択

`expr->ty` を `ty_deref` した上で:

```c
case BO_ADD: return ALLOC_node_add_int(l, r);  // 推論器が int を保証
case BO_LT:  return ex_ty_is(e->bin.l, TYK_INT)
                 ? ALLOC_node_lt_int(l, r)      // int 比較は整数 cmov
                 : ALLOC_node_lt(l, r);         // 多相比較 (string/list/tuple)
case EX_IF:  return ALLOC_node_if_bool(...);   // 条件は常に bool
```

generic な `node_add` 等は実装は残す (interpreter の汎用パスに必要だが、
推論済み input では到達しない)。

## 5. 関数適用 — `ml_apply`

```c
VALUE ml_apply(CTX *c, VALUE fn, int argc, VALUE *argv)
```

ループ + goto で実装:

1. `fn` が prim なら呼ぶ (partial-application sentinel なら捕捉済み args
   を combine して再 loop)
2. fn が closure で argc < nparams なら **partial application** —
   `partial_state` を `OOBJ_PRIM` 装って sentinel を返す
3. argc == nparams なら frame allocate (`is_leaf` なら C スタック alloca)、
   `c->env = frame` し body を EVAL
4. body 終了後 `c->tail_call_pending` 立ってたら fn / argc を入れ替えて
   `goto loop` (トランポリン)
5. argc > nparams (over-application) なら `r` を fn として再帰

`node_app1/2/3` の hot path は `app_cache` (call site 単位の IC):
直前 fn と同じなら type-chain 検証をスキップして直接 frame 構築 →
body dispatcher 呼び出し。

## 6. 例外 — `ml_raise` / `ml_run_handle`

```c
struct ml_handler { jmp_buf buf; VALUE exn; struct mlframe *saved_env; };
CTX::handlers[256], handlers_top = -1;
```

`raise e` (e : exn) は `handlers_top` のハンドラに `longjmp(1)`。
ハンドラは `setjmp == 0` で body 評価、`!= 0` で `exn` を slot 0 に
入れて handler arms (パターン chain) を eval。

トップレベルでハンドラなしの raise は `uncaught exception <name>` で exit 2。

## 7. グローバル

```c
struct gentry { const char *name; VALUE value; };
CTX::globals[..]
CTX::globals_serial   // 任意の define ごとに bump
```

`node_gref` は **per-call-site の inline cache** を持つ
(`struct gref_cache *cache @ref`):

```c
if (LIKELY(cache->serial == c->globals_serial)) return cache->value;
v = ml_global_ref(c, name);
cache->serial = c->globals_serial;
cache->value = v;
```

global の rebinding が起きると serial が上がって IC が冷却 → 次回
linear search → 再 cache。ベンチでは globals は init 後動かないので
ホットパスで 2 load + 1 cmp で済む。

## 8. AST Code Store (AOT)

`-c` で起動した場合、各 top form を:

1. `astro_cs_compile(form, NULL)` → `code_store/c/SD_<hash>.c` を生成
2. 同時に `aot_add_entry` 登録済みの各 closure body も compile
3. `astro_cs_build(NULL)` → `make` で `code_store/all.so` 生成
4. `astro_cs_reload()` で dlopen
5. `astro_cs_load(form, NULL)` で dispatcher を SD に差し替え

`is_specialized` フラグで二度走らせない。

## 9. ノード一覧 (主要なもの)

| ノード | 用途 |
|---|---|
| `node_const_int` / `_real` / `_str` / `_bool` / `_unit` / `_nil` | リテラル |
| `node_lref` (depth, idx) | ローカル変数参照 |
| `node_gref` (name, IC) | グローバル参照 |
| `node_if`, **`node_if_bool`** | `if` (後者は cond の bool チェック省略) |
| `node_seq` | `;` |
| `node_let`, `node_letrec`, `node_letrec_n` | 束縛 |
| `node_match_arm` | case 1 アーム |
| `node_match_fail` | 全アーム失敗時 (Match raise) |
| `node_fn` | クロージャ生成 |
| `node_app0` ... `node_app3`, `node_appn` | 関数適用 (cache 付き) |
| `node_tail_app1` / `_app2` | 末尾呼び出し (トランポリン) |
| `node_add`, **`node_add_int`** ... | 整数算術 (`_int` で IS_INT スキップ) |
| `node_rdiv` | 実数除算 |
| `node_lt` ... `node_ne`, **`node_*_int`** | 比較 (`_int` で値直接比較) |
| `node_andalso/orelse/not`, **`*_bool`** | 論理 (`_bool` で bool チェック省略) |
| `node_concat`, **`node_concat_str`** | 文字列結合 |
| `node_cons`, `node_tuple` | リスト/タプル construct |
| `node_ref`, **`node_deref_unchecked`**, **`node_assign_unchecked`** | 参照 |
| `node_pat_test_*` / `node_proj_*` / `node_pat_and` | パターンの test と extract |
| `node_ctor0`, `node_ctor1` | コンストラクタ適用 |
| `node_raise`, `node_handle` | 例外 |
| `node_topbind` | トップレベルへの代入 |

`@ref` operand:
- `gref_cache *cache` — `node_gref` の IC
- `app_cache *cache` — `node_app1/2/3` の receiver IC
