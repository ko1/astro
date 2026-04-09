# abruby (a bit Ruby)

ASTro フレームワークを用いた Ruby サブセット言語インタプリタ。CRuby の C extension として実装。

- 言語仕様: [docs/abruby_spec.md](docs/abruby_spec.md)
- 実装済み機能: [docs/done.md](docs/done.md)
- 未実装機能: [docs/todo.md](docs/todo.md)

## 開発フロー

機能追加・バグ修正の際は以下の手順に従うこと:

1. **diff 確認** — `git diff` が空であることを確認。空でなければ、先にコミットするか聞く
2. **実装** — コード変更
3. **テスト** — しつこくテストを書く。境界値、異常系、組み合わせを網羅
4. **テスト実行** — `make test` で通常テスト、`make debug-test` でデバッグモードテスト。両方通ること
5. **docs/ 更新** — done.md / todo.md / abruby_spec.md を必要に応じて更新
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
make test                  # テスト (607 tests)
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
├── node.def            # ASTro ノード定義
├── node.h              # NodeHead, NodeKind, EVAL, ヘルパー宣言
├── node.c              # ランタイム (hash, alloc, optimize, mark 関数, generated includes)
├── context.h           # CTX, abruby_class/object/string/array/hash 構造体, AB_CLASS_OF
├── builtin/
│   ├── builtin.h       # ビルトイン共通ヘッダ (マクロ, Init 宣言)
│   ├── kernel.c        # Kernel: p, raise
│   ├── object.c        # Object: inspect, to_s, ==, !=, !, nil?, class
│   ├── class.c         # Class: new, inspect; Module: include
│   ├── integer.c       # Integer: 算術, 比較, ビット演算, inspect, to_s, etc.
│   ├── float.c         # Float: 算術, 比較, floor, ceil, round, etc.
│   ├── string.c        # String: +, *, 比較, length, upcase, etc.
│   ├── symbol.c        # Symbol: ==, !=, to_s, inspect
│   ├── array.c         # Array: [], []=, push, pop, first, last, etc.
│   ├── hash.c          # Hash: [], []=, keys, values, etc.
│   ├── range.c         # Range: first, last, include?, to_a, etc.
│   ├── regexp.c        # Regexp: match?, match, =~, source, etc.
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
│   └── test_*.rb       # ノード/機能ごとのテスト (22 files)
├── docs/
│   └── abruby_spec.md  # 言語仕様
├── node_specialized.c  # AOT 用 (現在は空)
├── test.ab.rb          # サンプルプログラム (Point クラス)
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
| | `node_sym` | `const char *str` |
| | `node_self` | (なし) |
| | `node_range_new` | `NODE *begin_node, NODE *end_node, uint32_t exclude_end` |
| | `node_regexp_new` | `const char *source, const char *flags` |
| コンテナ | `node_ary_new` | `uint32_t argc, uint32_t arg_index` |
| | `node_hash_new` | `uint32_t argc, uint32_t arg_index` |
| 変数 | `node_lvar_get` | `uint32_t index` |
| | `node_lvar_set` | `uint32_t index, NODE *rhs` |
| | `node_gvar_get` | `const char *name` |
| | `node_gvar_set` | `const char *name, NODE *rhs` |
| | `node_ivar_get` | `const char *name` |
| | `node_ivar_set` | `const char *name, NODE *value` |
| スコープ | `node_scope` | `uint32_t envsize, NODE *body` |
| 制御フロー | `node_seq` | `NODE *head, NODE *tail` |
| | `node_if` | `NODE *cond, NODE *then_node, NODE *else_node` |
| | `node_while` | `NODE *cond, NODE *body` |
| | `node_return` | `NODE *value` |
| | `node_break` | `NODE *value` |
| 例外 | `node_rescue` | `NODE *body, NODE *rescue_body, NODE *ensure_body, uint32_t exception_lvar_index` |
| 定数 | `node_const_get` | `const char *name` |
| | `node_const_set` | `const char *name, NODE *value` |
| | `node_const_path_get` | `const char *parent_name, const char *name` |
| メソッド | `node_def` @noinline | `const char *name, NODE *body, uint32_t params_cnt, uint32_t locals_cnt` |
| | `node_method_call` | `NODE *recv, const char *name, uint32_t params_cnt, uint32_t arg_index` |
| OOP | `node_class_def` @noinline | `const char *name, const char *super_name, NODE *body` |
| | `node_module_def` @noinline | `const char *name, NODE *body` |
| 算術 | `node_plus/minus/mul/div` | `NODE *left, NODE *right` — Fixnum inline + fallback |
| 算術 (type-specific) | `node_fixnum_plus/minus/mul/div` | `NODE *left, NODE *right` — Fixnum 前提の高速パス。rewrite で生成 |

### メソッドディスパッチ

全てのメソッド呼び出しは `node_method_call` で統一:
- トップレベルの `foo(args)` → `self.foo(args)` として Object にディスパッチ
- `obj.method(args)` → `AB_CLASS_OF(obj)` でクラス解決 → メソッドテーブル検索
- メソッドが見つからなければ `method_missing` を検索
- `p`, `raise` は Kernel モジュールのメソッド（Object に include）
- cfunc は RESULT を返す（非局所脱出を直接シグナル可能）

### T_DATA 統一構造

全ての abruby ヒープオブジェクトは統一された `abruby_data_type` の T_DATA で、先頭に `abruby_class *klass` を持つ:

```c
struct abruby_header { struct abruby_class *klass; };  // 全 T_DATA 共通

struct abruby_object { struct abruby_class *klass; ... ivars ... };
struct abruby_string { struct abruby_class *klass; VALUE rb_str; };
struct abruby_array  { struct abruby_class *klass; VALUE rb_ary; };
struct abruby_hash   { struct abruby_class *klass; VALUE rb_hash; };
struct abruby_class  { struct abruby_class *klass; ... methods ... };
```

### AB_CLASS_OF (static inline, context.h)

```c
AB_CLASS_OF(obj):
  即値 (Fixnum/true/false/nil) → 固定クラスポインタ
  T_DATA → DATA_PTR(obj)->klass  // 共通ヘッダで直引き
```

`ABRUBY_DEBUG=1` 時は `ab_verify(obj)` で T_DATA / TypedData / data_type / klass の厳密チェック。

### クラスシステム

```c
struct abruby_class {
    struct abruby_class *klass;     // 自身のクラス (= ab_class_class)
    const char *name;
    struct abruby_class *super;     // 継承チェーン (Object が基底)
    struct abruby_method methods[]; // メソッドテーブル (AST or CFUNC)
};
```

- ビルトインクラス: Object, Class, Integer, String, Array, Hash, TrueClass, FalseClass, NilClass
- ビルトインモジュール: Kernel（Object に include、p/raise を提供）
- 各クラスのメソッドは `builtin/<class>.c` で定義、`Init_abruby_<class>()` で登録
- `Class#new` で `abruby_new_object` + `initialize` 呼び出し
- メソッド検索は `abruby_class_find_method` で線形探索 + 継承チェーン走査
- トップレベル `def` は `ab_object_class` にメソッドを追加

### GC 連携
- 全 abruby ヒープオブジェクトは統一 `abruby_data_type` の T_DATA
- mark 関数が klass で分岐: String→rb_str, Array→rb_ary, Hash→rb_hash, Object→ivars をマーク
- NODE は別の `abruby_node_type` の T_DATA、`abruby_node_mark()` で子ノードをマーク
- VALUE スタック (グローバル配列) は CTX の T_DATA ラッパー経由で `rb_gc_mark_locations` でマーク
- NODE は `RUBY_DEFAULT_FREE` で GC sweep 時に解放される

### VALUE スタック
- グローバルな VALUE 配列 (固定サイズ 10000)
- `CTX.fp` がフレームポインタ、ローカル変数は `c->fp[index]`
- メソッド呼び出し時: レシーバをスロットに保存 → 引数を配置 → fp を移動して body を評価

### デバッグ
- `make debug-test` で `ABRUBY_DEBUG=1` ビルド + テスト + 通常ビルド復帰
- `ab_verify(obj)` が AB_CLASS_OF 等で厳密なチェック (即値/T_DATA/TypedData/data_type/klass)

### テスト
607 テスト (`make test`):
- test_literals (9), test_literals_extra (9), test_variables (9)
- test_control_flow (55), test_break (9), test_exception (24)
- test_method_call (69), test_class (28), test_constants (6)
- test_integer (35), test_bignum (51), test_float (48), test_numeric_mixed (42)
- test_string (32), test_symbol (22), test_array (42), test_hash (30)
- test_range (32), test_regexp (17)
- test_global_variables (9), test_multi_assign (9)
- test_gc_pressure (20)
