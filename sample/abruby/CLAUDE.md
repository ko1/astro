# abruby (a bit Ruby)

ASTro フレームワークを用いた Ruby サブセット言語インタプリタ。CRuby の C extension として実装。

言語仕様は [docs/abruby_spec.md](docs/abruby_spec.md) を参照。

## ビルド・実行

```sh
ruby extconf.rb && make   # ビルド
make test                  # テスト (245 tests)
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
  → VALUE                             # CRuby の VALUE として結果を返す
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
│   ├── object.c        # Object: inspect, to_s, ==, !=, nil?, class, p
│   ├── class.c         # Class: new, inspect
│   ├── integer.c       # Integer: 算術, 比較, inspect, to_s, zero?, abs
│   ├── string.c        # String: +, *, 比較, length, upcase, etc.
│   ├── array.c         # Array: [], []=, push, pop, first, last, etc.
│   ├── hash.c          # Hash: [], []=, keys, values, etc.
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
│   └── test_*.rb       # ノード/機能ごとのテスト (9 files)
├── docs/
│   └── abruby_spec.md  # 言語仕様
├── node_specialized.c  # AOT 用 (現在は空)
├── test.ab.rb          # サンプルプログラム (Point クラス)
└── example.ab.rb       # fib ベンチマーク
```

### ノード一覧 (16種)

| カテゴリ | ノード | オペランド |
|---|---|---|
| リテラル | `node_num` | `int32_t num` |
| | `node_true` | (なし) |
| | `node_false` | (なし) |
| | `node_nil` | (なし) |
| | `node_str` | `const char *str` |
| | `node_self` | (なし) |
| コンテナ | `node_array_new` | `uint32_t argc, uint32_t arg_index` |
| | `node_hash_new` | `uint32_t argc, uint32_t arg_index` |
| 変数 | `node_lget` | `uint32_t index` |
| | `node_lset` | `uint32_t index, NODE *rhs` |
| | `node_ivar_get` | `const char *name` |
| | `node_ivar_set` | `const char *name, NODE *value` |
| スコープ | `node_scope` | `uint32_t envsize, NODE *body` |
| 制御フロー | `node_seq` | `NODE *head, NODE *tail` |
| | `node_if` | `NODE *cond, NODE *then_node, NODE *else_node` |
| | `node_while` | `NODE *cond, NODE *body` |
| メソッド | `node_def` @noinline | `const char *name, NODE *body, uint32_t params_cnt, uint32_t locals_cnt` |
| | `node_method_call` | `NODE *recv, const char *name, uint32_t params_cnt, uint32_t arg_index` |
| | `node_const_get` | `const char *name` |
| OOP | `node_class_def` @noinline | `const char *name, NODE *body` |

### メソッドディスパッチ

全てのメソッド呼び出しは `node_method_call` で統一:
- トップレベルの `foo(args)` → `self.foo(args)` として Object にディスパッチ
- `obj.method(args)` → `AB_CLASS_OF(obj)` でクラス解決 → メソッドテーブル検索
- メソッドが見つからなければ `method_missing` を検索
- `p` は Object のメソッド

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
- 各クラスのメソッドは `builtin/<class>.c` で定義、`Init_abruby_<class>()` で登録
- `Class#new` で `abruby_new_object` + `initialize` 呼び出し
- メソッド検索は `abruby_class_find_method` で線形探索 + 継承チェーン走査
- トップレベル `def` は `ab_object_class` にメソッドを追加

### GC 連携
- 全 abruby ヒープオブジェクトは統一 `abruby_data_type` の T_DATA
- mark 関数が klass で分岐: String→rb_str, Array→rb_ary, Hash→rb_hash, Object→ivars をマーク
- NODE は別の `abruby_node_type` の T_DATA、`abruby_node_mark()` で子ノードをマーク
- VALUE スタック (グローバル配列) は CTX の T_DATA ラッパー経由で `rb_gc_mark_locations` でマーク
- free は当面なし (実験的)

### VALUE スタック
- グローバルな VALUE 配列 (固定サイズ 10000)
- `CTX.fp` がフレームポインタ、ローカル変数は `c->fp[index]`
- メソッド呼び出し時: レシーバをスロットに保存 → 引数を配置 → fp を移動して body を評価

### デバッグ
- `ruby extconf.rb -- --enable-debug && make` で `ABRUBY_DEBUG=1` ビルド
- `ab_verify(obj)` が AB_CLASS_OF 等で厳密なチェック (即値/T_DATA/TypedData/data_type/klass)
- `make debug-test` でデバッグビルド + テスト実行

### テスト
245 テスト (`make test`):
- test_literals (9), test_variables (9), test_control_flow (13)
- test_method_call (69), test_class (21), test_string (30)
- test_array (43), test_hash (30), test_gc_pressure (21)
