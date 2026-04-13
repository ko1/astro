# abruby (a bit Ruby)

ASTro フレームワークを用いた Ruby サブセット言語インタプリタ。CRuby の C extension として実装。

- 言語仕様: [docs/abruby_spec.md](docs/abruby_spec.md)
- 実装済み機能: [docs/done.md](docs/done.md)
- 未実装機能: [docs/todo.md](docs/todo.md)
- ランタイム構造: [docs/runtime.md](docs/runtime.md)

## 開発フロー

機能追加・バグ修正の際は以下の手順に従うこと:

1. **diff 確認** — `git diff` が空であることを確認。空でなければ、先にコミットするか聞く
2. **実装** — コード変更
3. **テスト** — しつこくテストを書く。境界値、異常系、組み合わせを網羅
4. **テスト実行** — `make test` で通常テスト、`make debug-test` でデバッグモードテスト。両方通ること
5. **docs/ 更新** — done.md / todo.md / abruby_spec.md / runtime.md を必要に応じて更新
6. **コミット**

## 開発時の注意

- GC のバグが良く起こります。GC に限らず、不可解なバグは絶対に放置せずデバッグしてください。テストを削るなんてもってのほかです。
- 警告が出たら無視せず無くす努力をすること。解決が難しければ、報告すること。
- ソースコードのコメントは英語で書くこと。
- cfunc（ビルトインメソッド）は CRuby の `rb_raise` を使わず、abruby の例外（`abruby_exception_new` + `RESULT_RAISE`）を使うこと。CRuby 例外はバックトレースが出ず、rescue でも捕捉できない。やむを得ず CRuby API を呼ぶ場合は `rb_protect` 等で catch して abruby 例外に変換すること。
- bool や enum が適切な場合は int の代わりに積極的に利用すること。
- AST ノードの rewrite（型に基づくノード切り替え、例: node_plus → node_fixnum_plus）で ALLOC + node_replace する際、子ノードのポインタは DISPATCH から渡されたパラメータ（stale になる可能性あり）ではなく、**ノードのフィールドから再読み込み**すること（`n->u.node_xxx.left` 等）。EVAL_ARG 中に子ノードが自身の rewrite で replace され、親のフィールドが更新されるため。（注: specialize は ASTro の部分評価による最適化を指す別の用語。混同しないこと）
- `ulimit -s unlimited` は絶対に実行しないこと。stack overflow のガードが効かなくなり OOM で WSL ごと殺される。

## ビルド・実行

```sh
ruby extconf.rb && make   # ビルド
make test                  # テスト (1045 tests)
make run                   # exe/abruby test.ab.rb を実行
make clean                 # 生成ファイル含め全削除
make debug-test            # ABRUBY_DEBUG=1 でビルド + テスト
```

```sh
./exe/abruby file.ab.rb       # ファイルを実行
./exe/abruby -e 'p(1 + 2)'   # 式を評価
./exe/abruby --dump -e 'expr' # AST を pretty print
./exe/abruby -v               # バージョン表示
```

## アーキテクチャ

### 処理の流れ
```
AbRuby.eval(code)
  → Ruby: Prism.parse(code)          # CRuby 内蔵パーサで AST 生成
  → Ruby: Parser#transduce(prism_ast) # Prism AST → AbRuby AST (ALLOC_* 呼び出し)
  → C:    EVAL(ctx, ast)              # ASTro インタプリタで実行
  → RESULT {value, state}             # VALUE + 実行状態 (NORMAL/RETURN/RAISE/BREAK)
```

### ファイル構成
```
sample/abruby/
├── abruby.c            # C extension エントリ (Init_abruby, T_DATA 型, ヘルパー, ALLOC ラッパー)
├── node.def            # ASTro ノード定義 (全ノードの EVAL 実装)
├── node.h              # NodeHead, NodeKind, EVAL, ヘルパー宣言
├── node.c              # ランタイム (hash, alloc, optimize, mark 関数, generated includes)
├── context.h           # CTX, abruby_machine, abruby_class/object/... 構造体, AB_CLASS_OF
├── builtin/
│   ├── builtin.h       # ビルトイン共通ヘッダ (マクロ, Init 宣言)
│   ├── kernel.c        # Kernel: p, raise, Rational, Complex
│   ├── object.c        # Object: inspect, to_s, ==, !=, !, nil?, class, is_a?
│   ├── class.c         # Class: new, inspect; Module: include, const_get, const_set
│   ├── integer.c       # Integer: 算術, 比較, ビット演算, inspect, to_s, etc.
│   ├── float.c         # Float: 算術, 比較, floor, ceil, round, etc.
│   ├── string.c        # String: +, *, 比較, length, upcase, etc.
│   ├── symbol.c        # Symbol: ==, !=, to_s, inspect
│   ├── array.c         # Array: [], []=, push, pop, first, last, etc.
│   ├── hash.c          # Hash: [], []=, keys, values, etc.
│   ├── range.c         # Range: first, last, include?, to_a, etc.
│   ├── regexp.c        # Regexp: match?, match, =~, source, etc.
│   ├── rational.c      # Rational: 算術, 比較, numerator, denominator, etc.
│   ├── complex.c       # Complex: 算術, real, imaginary, abs, etc.
│   ├── exception.c     # RuntimeError: message, backtrace, inspect
│   ├── true.c          # TrueClass: inspect, to_s
│   ├── false.c         # FalseClass: inspect, to_s
│   └── nil.c           # NilClass: inspect, to_s, nil?
├── extconf.rb          # ビルド設定 (mkmf, builtin/ のソースも含む)
├── depend              # Makefile 追加ルール (ASTroGen, clean, test, run)
├── lib/
│   ├── abruby.rb       # Prism AST → AbRuby AST 変換, AbRuby.eval/dump
│   └── abruby/
│       └── version.rb  # AbRuby::VERSION
├── exe/
│   └── abruby          # コマンドラインツール (-e, --dump, -v)
├── test/
│   ├── run_all.rb      # テストランナー
│   ├── test_helper.rb  # assert_eval
│   └── test_*.rb       # ノード/機能ごとのテスト (27 files, 780 tests)
├── docs/
│   ├── abruby_spec.md  # 言語仕様
│   ├── done.md         # 実装済み機能
│   ├── todo.md         # 未実装機能
│   └── runtime.md      # ランタイムデータ構造
├── node_specialized.c  # AOT 用 (現在は空)
├── test.ab.rb          # サンプルプログラム
└── example.ab.rb       # fib ベンチマーク
```

### ノード一覧

| カテゴリ | ノード | オペランド |
|---|---|---|
| リテラル | `node_num` | `int32_t num` |
| | `node_float_new` | `const char *str` |
| | `node_bignum_new` | `const char *str` |
| | `node_true` | (なし) |
| | `node_false` | (なし) |
| | `node_nil` | (なし) |
| | `node_str_new` | `const char *str` |
| | `node_str_concat` | `uint32_t argc, uint32_t arg_index` |
| | `node_sym` | `const char *str` |
| | `node_self` | (なし) |
| | `node_range_new` | `NODE *begin_node, NODE *end_node, uint32_t exclude_end` |
| | `node_regexp_new` | `const char *source, const char *flags` |
| コンテナ | `node_ary_new` | `uint32_t argc, uint32_t arg_index` |
| | `node_hash_new` | `uint32_t argc, uint32_t arg_index` |
| 変数 | `node_lvar_get` | `uint32_t index` |
| | `node_lvar_set` | `uint32_t index, NODE *rhs` |
| | `node_gvar_get` | `ID name` |
| | `node_gvar_set` | `ID name, NODE *rhs` |
| | `node_ivar_get` | `ID name` |
| | `node_ivar_set` | `ID name, NODE *value` |
| スコープ | `node_scope` | `uint32_t envsize, NODE *body` |
| 制御フロー | `node_seq` | `NODE *head, NODE *tail` |
| | `node_if` | `NODE *cond, NODE *then_node, NODE *else_node` |
| | `node_while` | `NODE *cond, NODE *body` |
| | `node_return` | `NODE *value` |
| | `node_break` | `NODE *value` |
| 例外 | `node_rescue` | `NODE *body, NODE *rescue_body, NODE *ensure_body, uint32_t exception_lvar_index` |
| 定数 | `node_const_get` | `ID name` |
| | `node_const_set` | `ID name, NODE *value` |
| | `node_const_path_get` | `ID parent_name, ID name` |
| メソッド定義 | `node_def` @noinline | `ID name, NODE *body, uint32_t params_cnt, uint32_t locals_cnt` |
| メソッド呼出 | `node_method_call` | `NODE *recv, ID name, uint32_t params_cnt, uint32_t arg_index, struct method_cache *mc@ref` |
| | `node_func_call` | `ID name, uint32_t params_cnt, uint32_t arg_index, struct method_cache *mc@ref` |
| | `node_super` | `uint32_t params_cnt, uint32_t arg_index` |
| OOP | `node_class_def` @noinline | `ID name, NODE *super_expr, NODE *body` |
| | `node_module_def` @noinline | `ID name, NODE *body` |
| 算術 | `node_plus/minus/mul/div` | `NODE *left, NODE *right, uint32_t arg_index` — 汎用 (メソッドディスパッチ) |
| 算術 (fixnum) | `node_fixnum_plus/minus/mul/div` | `NODE *left, NODE *right, uint32_t arg_index` — Fixnum 高速パス |
| 算術 (integer) | `node_integer_plus/minus/mul/div` | `NODE *left, NODE *right, uint32_t arg_index` — Bignum 対応 |
| 比較 | `node_lt/le/gt/ge` | `NODE *left, NODE *right, uint32_t arg_index` |
| 比較 (fixnum) | `node_fixnum_lt/le/gt/ge` | `NODE *left, NODE *right, uint32_t arg_index` |
| 等値 | `node_eq/neq` | `NODE *left, NODE *right, uint32_t arg_index` |
| 等値 (fixnum) | `node_fixnum_eq/neq` | `NODE *left, NODE *right, uint32_t arg_index` |
| 剰余 | `node_mod` | `NODE *left, NODE *right, uint32_t arg_index` |
| 剰余 (fixnum) | `node_fixnum_mod` | `NODE *left, NODE *right, uint32_t arg_index` |

算術/比較/等値ノードの `arg_index` は、型が合わずメソッドディスパッチにフォールバックする際の引数配置先。`swap_dispatcher` でノード種別を動的に切り替える（例: `node_fixnum_plus` → `node_integer_plus` → `node_plus`）。

### メソッドディスパッチ

2種類のメソッド呼び出しノード:
- **`node_func_call`** — 暗黙 self-call (`foo(args)`, `self.foo(args)`)。recv ノードなし、`c->self` を直接使用。
- **`node_method_call`** — 明示的レシーバ (`obj.method(args)`)。recv を EVAL_ARG で評価、self の save/restore が必要。

ディスパッチ関数:
- **`dispatch_method_frame`** (static inline) — cache hit パス。フレーム push/pop + fp 操作 + EVAL。self の save/restore はしない。
- **`dispatch_method_with_klass`** (static inline) — cache miss パス。一時 method_cache を構築、self save/restore + dispatch_method_frame。
- **`method_cache_fill`** — mc にキャッシュ内容（klass, method, serial, ivar_slot, body, dispatcher）を書き込む。

インラインキャッシュ (`struct method_cache`, `@ref` でノードに埋め込み):
- cache hit 条件: `mc->klass == AB_CLASS_OF(recv) && mc->serial == abm->method_serial`
- `mc->body` / `mc->dispatcher` で method 構造体を経由せず直接呼び出し
- `method_serial` は `abruby_machine` にあり、メソッド定義/include でインクリメント

### T_DATA 統一構造

全ての abruby ヒープオブジェクトは統一された `abruby_data_type` の T_DATA で、先頭に `klass` + `obj_type` を持つ:

```c
struct abruby_object { klass, obj_type, ivar_cnt, *extra_ivars, ivars[4] };  // 4 inline + heap
struct abruby_string { klass, obj_type, rb_str };
struct abruby_array  { klass, obj_type, rb_ary };
struct abruby_hash   { klass, obj_type, rb_hash };
struct abruby_class  { klass, obj_type, instance_obj_type, name, super, methods, ... };
struct abruby_proc   { klass, obj_type, body, *env, params_cnt, is_lambda, ... };
struct abruby_fiber  { klass, obj_type, ctx, state, proc_value, crb_fiber, ... };
```

### AB_CLASS_OF (static inline, context.h)

```c
AB_CLASS_OF(obj):
  ヒープオブジェクト (T_DATA) → RTYPEDDATA_GET_DATA(obj)->klass  // 最初にチェック
  即値 → AB_CLASS_OF_IMM(obj)  // Fixnum/Symbol/true/false/nil → 対応クラス
```

`ABRUBY_DEBUG=1` 時は `ab_verify(obj)` で T_DATA / TypedData / data_type / klass の厳密チェック。

### abruby_machine と CTX

```c
struct abruby_machine {              // AbRuby インスタンスごと
    uint32_t method_serial;          // メソッドバージョン (キャッシュ無効化用)
    struct abruby_fiber *current_fiber; // 現在実行中のファイバー
    struct abruby_class main_class_body;
    struct ab_id_table gvars;        // グローバル変数 (動的テーブル)
    struct abruby_class *proc_class, *fiber_class, ...;  // per-instance クラス (21個)
    struct abruby_fiber *root_fiber; // ブートストラップファイバー
    ...
};

struct CTX_struct {                  // ノード評価関数の第1引数
    struct abruby_machine *abm;      // 所属する machine
    VALUE stack[10000];              // VALUE スタック
    VALUE *fp;                       // フレームポインタ
    VALUE *sp;                       // GC 用 high-water mark
    VALUE self;                      // 現在のレシーバ
    const struct abruby_cref *cref;  // lexical 定数スコープ
    const struct abruby_block *current_block;       // yield 実行中のブロック
    const struct abruby_frame *current_block_frame; // ブロック定義元フレーム
    ...
};
```

CTX から machine のフィールドへはマクロでアクセス:
- `CTX_MAIN_CLASS(c)` → `&c->abm->main_class_body`
- `CTX_GVARS(c)` → `&c->abm->gvars`

### クラスシステム

```c
struct abruby_class {
    struct abruby_class *klass;             // メタクラス (per-instance class_class)
    enum abruby_obj_type obj_type;          // CLASS or MODULE
    enum abruby_obj_type instance_obj_type; // このクラスのインスタンスの型
    ID name;
    struct abruby_class *super;             // 継承チェーン (Object が基底)
    struct ab_id_table methods;             // メソッドテーブル (動的ハッシュ)
    VALUE rb_wrapper;
    struct ab_id_table constants;           // 定数テーブル (動的ハッシュ)
    struct ab_id_table ivar_shape;          // ivar name → slot index
};
```

- ビルトインクラス: Object, Class, Module, Integer, Float, String, Symbol, Array, Hash, Range, Regexp, Rational, Complex, TrueClass, FalseClass, NilClass, Proc, Fiber, GC, File
- ビルトインモジュール: Kernel（Object に include）
- 各クラスのメソッドは `builtin/<class>.c` で定義、`Init_abruby_<class>()` で登録
- `Class#new` で `abruby_new_object` + `initialize` 呼び出し
- メソッド検索は `abruby_class_find_method` で ab_id_table 検索 + 継承チェーン走査
- トップレベル `def` は `CTX_MAIN_CLASS(c)` にメソッドを追加

### GC 連携
- 全 abruby ヒープオブジェクトは統一 `abruby_data_type` の T_DATA（先頭に klass + obj_type）
- mark 関数が obj_type で分岐: String→rb_str, Array→rb_ary, Hash→rb_hash, Object→ivars, Proc→env, Fiber→ctx をマーク
- NODE は別の `abruby_node_type` の T_DATA、`abruby_node_mark()` で子ノードをマーク
- VALUE スタック (CTX 内) は machine の T_DATA ラッパー経由で `rb_gc_mark_locations` でマーク（sp で high-water mark 管理）
- NODE は `RUBY_DEFAULT_FREE` で GC sweep 時に解放される

### フレームと backtrace
- `struct abruby_frame` を C スタック上に確保し、linked list で管理
- `caller_node` 方式: 各フレームが「自分がどこから呼ばれたか」を記録
- backtrace は `f->caller_node->line` + `f->prev->method->name` をペアにして構築
- `PUSH_FRAME(node)` / `POP_FRAME()` マクロでインライン例外 (0除算等) の位置を記録

### VALUE スタック
- CTX 内の `VALUE stack[10000]` (固定サイズ)
- `c->fp` がフレームポインタ、ローカル変数は `c->fp[index]`
- メソッド呼び出し時: 引数を配置 → fp を移動して body を評価 → fp を復元

### デバッグ
- `make debug-test` で `ABRUBY_DEBUG=1` ビルド + テスト + 通常ビルド復帰
- `ab_verify(obj)` が AB_CLASS_OF 等で厳密なチェック (即値/T_DATA/TypedData/data_type/klass)

### テスト
1045 テスト (`make test`, 35 ファイル):
- test_literals (9), test_literals_extra (9), test_variables (9)
- test_control_flow (55), test_break (9), test_exception (44)
- test_method_call (114), test_method_override (14), test_class (53), test_class_body (12), test_constants (10)
- test_case (30)
- test_integer (41), test_bignum (51), test_float (48), test_numeric_mixed (42)
- test_string (32), test_symbol (22), test_array (42), test_hash (30)
- test_range (32), test_regexp (17)
- test_rational (10), test_complex (13)
- test_global_variables (9), test_multi_assign (15), test_multi_assign_targets (11)
- test_or_and_write (22), test_splat_call (15)
- test_block_basic (48), test_block_control (42), test_block_iterator (60)
- test_proc (31), test_fiber (24)
- test_gc_pressure (20)
