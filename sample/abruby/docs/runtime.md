# abruby ランタイム構造

## 全体像

```
abruby_machine (AbRuby インスタンスごと)
  method_serial          ← メソッド定義時にインクリメント
  current_fiber ───────→ abruby_fiber (実行ファイバー)
    ctx                  ← CTX (実行コンテキスト、stack 含む)
  main_class_body        ← インスタンスごとの Object サブクラス
  gvars                  ← グローバル変数テーブル
  id_cache               ← rb_intern 結果のキャッシュ
  rb_self                ← Ruby 側の AbRuby インスタンス
  current_file           ← 実行中のファイルパス
  loaded_files           ← require 済みファイル一覧

CTX (実行コンテキスト、ノード評価関数の第1引数)
  abm ────────────────→ abruby_machine (所属する machine)
  stack[10000]           ← VALUE スタック (ローカル変数 + 引数)
  env ────────────────→ stack[0]
  fp  ────────────────→ stack[N]  (現在のフレームポインタ)
  self                   ← 現在のレシーバ
  current_class          ← クラス定義中のクラス (通常 NULL)
  current_frame ───────→ abruby_frame linked list
  ids ─────────────────→ id_cache
```

CTX から machine のフィールドへはマクロでアクセス:
```c
CTX_MAIN_CLASS(c)  // → &c->abm->main_class_body
CTX_GVARS(c)       // → &c->abm->gvars
```

## データ構造

### abruby_fiber (ファイバー)

CTX（実行コンテキスト）を所有する。将来的に複数 Fiber の切り替えに対応。

```c
struct abruby_fiber {
    CTX ctx;                             // 実行コンテキスト (stack 含む)
};
```

### abruby_machine (実行マシン)

AbRuby インスタンスごとに1つ。

```c
struct abruby_machine {
    uint32_t method_serial;              // メソッドバージョン (キャッシュ無効化用)
    struct abruby_fiber *current_fiber;  // 現在実行中のファイバー
    struct abruby_class main_class_body; // インスタンスごとの Object サブクラス
    struct ab_id_table gvars;            // グローバル変数
    struct abruby_id_cache id_cache;     // ID キャッシュ (+, -, <, method_missing 等)
    VALUE rb_self;                       // Ruby 側の AbRuby インスタンス
    VALUE current_file;                  // 実行中ファイルパス
    VALUE loaded_files;                  // require 済みファイル一覧
    // Per-instance built-in classes (cloned from templates at create_vm)
    struct abruby_class *object_class, *integer_class, *float_class,
                        *string_class, *symbol_class, *array_class, *hash_class,
                        *range_class, *regexp_class, *rational_class, *complex_class,
                        *true_class, *false_class, *nil_class,
                        *kernel_module, *module_class, *class_class,
                        *runtime_error_class;
};
```

**Per-instance 組み込みクラス**: 18 個の組み込みクラス（Integer, String, Float, Class, Module 等）は `abruby_machine` インスタンスごとに独立する。`Init_abruby()` 時に静的なテンプレートクラス (`_body` 変数) にメソッドを登録し、`create_vm()` の `init_instance_classes()` でテンプレートからメソッドテーブルを clone して per-instance にコピーする。これにより、あるインスタンスでの `class Integer; def foo; end; end` が他のインスタンスに影響しない。

ランタイムコードはすべて `c->abm->xxx_class` 経由で per-instance クラスにアクセスする (`AB_CLASS_OF_IMM` も CTX を受け取って即値を per-instance クラスに解決する)。

### CTX (実行コンテキスト)

ノード評価関数の第1引数。実行中の状態を保持する。

```c
struct CTX_struct {
    struct abruby_machine *abm;      // 所属する machine
    VALUE stack[ABRUBY_STACK_SIZE];  // VALUE スタック (ローカル変数 + 引数)
    VALUE *env;                      // スタックのベース (= stack)
    VALUE *fp;                       // フレームポインタ (ローカル変数の先頭)
    VALUE self;                      // 現在のレシーバ
    struct abruby_class *current_class;  // クラス定義の評価中のみ非 NULL
    struct abruby_frame *current_frame;  // フレーム linked list の先頭
    const struct abruby_id_cache *ids;   // ID キャッシュへのポインタ
};
```

gvars と main_class は machine 側に持ち、CTX からは `c->abm->` 経由でアクセスする。

### RESULT (実行結果)

全ノード評価の戻り値。2レジスタ (rax + rdx) に収まる。

```c
typedef struct {
    VALUE value;             // 値
    enum result_state state; // NORMAL=0 / RETURN=1 / RAISE=2 / BREAK=4 / NEXT=8
} RESULT;
```

`state` は bit flag で、メソッド境界で「特定の bit を demote」する処理が `r.state &= ~MASK` の単一命令になる。例えばメソッド境界での RETURN catch は `r.state &= ~RESULT_RETURN`、block-付き method 境界は さらに `RESULT_BREAK` も demote する。NEXT は `yield` サイトで demote される。

`UNWRAP(r)` マクロは state が NORMAL でなければ即座に return する。

### abruby_frame (呼び出しフレーム)

バックトレース用。C スタック上に確保し、linked list で管理。

```c
struct abruby_frame {
    struct abruby_frame *prev;            // 前のフレーム
    struct abruby_method *method;         // 実行中のメソッド (NULL = <main>)
    union {
        struct Node *caller_node;         // メソッドフレーム: 呼び出し元ノード
        const char *source_file;          // <main>: ファイルパス
    };
    const struct abruby_block *block;     // このメソッドが受け取った block (NULL = なし)
    uint32_t frame_id;                    // CTX ローカルの一意 ID（非ローカル return のターゲット）
};
```

`super` 解決は `method->defining_class->super` を使い、`frame->klass` は持たない（slim 化済み）。

**caller_node 方式**: 各フレームが「自分がどこから呼ばれたか」を記録する。
backtrace 構築時は `f->caller_node->line`（行番号）と `f->prev->method->name`（メソッド名）をペアにする。

### abruby_block (ブロックリテラル)

call サイトで C スタック上に組み立て、frame.block にポイントさせる。heap には乗らないため Proc には昇格できない（Phase 2 で対応予定）。

```c
struct abruby_block {
    struct Node *body;                  // block body AST
    VALUE *captured_fp;                 // caller の fp（closure 環境）
    VALUE captured_self;                // caller の self
    struct abruby_frame *defining_frame;// block を定義した enclosing メソッドフレーム
    uint32_t params_cnt;                // 必須パラメータ数
    uint32_t param_base;                // captured_fp[param_base..] にブロック引数を書く
};
```

**defining_frame**: block 実行中は `c->current_frame` を defining_frame にスワップする。これにより block body 内の `yield` が現在実行中のメソッド（= block を受け取ったメソッド）ではなく、block を定義したメソッドの block を参照する Ruby 仕様を実現する。`super` も同様に defining method の class から検索する。

**非ローカル return**: CTX に `return_target_frame_id` を持つ。`node_return` は以下のように target を設定:
- block body を実行中（`c->in_block == true`）: `c->current_block->defining_frame->frame_id`
- それ以外: `0`（wildcard = 最も近いメソッド境界で catch）

method 境界（`dispatch_method_frame` / `dispatch_method_with_klass_with_block`）は `return_target_frame_id` が 0 または自フレーム ID に一致する時のみ RETURN を demote し、それ以外はそのまま propagate させる。これで block 内 `return` が yield 連鎖を貫通して defining method から脱出できる。

**PUSH_FRAME / POP_FRAME マクロ**: インライン例外（0除算など dispatch_method_frame を経由しない箇所）で、例外発生位置を backtrace に含めるための軽量フレーム push/pop。

```c
PUSH_FRAME(node);     // caller_node = node のフレームを push
// ... abruby_exception_new(c, ...) ...
POP_FRAME();          // フレームを pop
```

### method_cache (インラインキャッシュ)

各 node_method_call / node_func_call に `@ref` で埋め込まれる。

```c
struct method_cache {
    struct abruby_class *klass;      // キャッシュしたクラス
    struct abruby_method *method;    // キャッシュしたメソッド
    uint32_t serial;                 // キャッシュ時の method_serial
    struct Node *body;               // method->u.ast.body (NULL = CFUNC)
    RESULT (*dispatcher)(...);       // body->head.dispatcher
};
```

`body` と `dispatcher` をキャッシュすることで、cache hit 時に method 構造体を経由する間接参照 2段を省略する。

### abruby_class (クラス)

```c
struct abruby_class {
    struct abruby_class *klass;     // メタクラス (= ab_class_class)
    ID name;
    struct abruby_class *super;     // 親クラス
    struct abruby_method methods[64];
    unsigned int method_cnt;
    VALUE rb_wrapper;               // GC 用 T_DATA ラッパー
    struct { ID name; VALUE value; } constants[64];
    unsigned int const_cnt;
};
```

メソッド検索は `abruby_class_find_method` で methods を線形探索し、見つからなければ super チェーンを辿る。

### abruby_method (メソッド)

```c
struct abruby_method {
    ID name;
    enum abruby_method_type type;   // AST or CFUNC
    union {
        struct {
            NODE *body;
            unsigned int params_cnt;
            unsigned int locals_cnt;
            const char *source_file;
        } ast;
        struct {
            abruby_cfunc_t func;    // RESULT (*)(CTX*, VALUE, uint, VALUE*)
            unsigned int params_cnt;
        } cfunc;
    } u;
};
```

### オブジェクト表現

全ヒープオブジェクトは CRuby の T_DATA で、先頭に `abruby_header` (klass ポインタ) を持つ。

```
即値 (Fixnum, Symbol, true, false, nil)
  → CRuby 即値をそのまま使用
  → AB_CLASS_OF → AB_CLASS_OF_IMM で対応クラスに解決

T_DATA ヒープオブジェクト
  → 先頭に abruby_header { klass } を持つ
  → AB_CLASS_OF は RTYPEDDATA_GET_DATA → klass で解決

  abruby_object   { klass, ivar_cnt, ivars[32] }      ユーザ定義オブジェクト
  abruby_string   { klass, rb_str }                    CRuby String ラッパー
  abruby_array    { klass, rb_ary }                    CRuby Array ラッパー
  abruby_hash     { klass, rb_hash }                   CRuby Hash ラッパー
  abruby_bignum   { klass, rb_bignum }                 CRuby Bignum ラッパー
  abruby_float    { klass, rb_float }                  CRuby Float ラッパー
  abruby_range    { klass, begin, end, exclude_end }   独自構造
  abruby_regexp   { klass, rb_regexp }                 CRuby Regexp ラッパー
  abruby_rational { klass, rb_rational }               CRuby Rational ラッパー
  abruby_complex  { klass, rb_complex }                CRuby Complex ラッパー
  abruby_exception{ klass, message, backtrace }        例外
```

### NodeHead (AST ノードヘッダ)

全 AST ノード共通のヘッダ。ノード固有データ（オペランド）はこの後に続く。

```c
struct NodeHead {
    struct NodeFlags flags;            // specialized, no_inline 等
    const struct NodeKind *kind;       // ノード種別 (関数ポインタ群)
    struct Node *parent;               // 親ノード
    node_hash_t hash_value;            // コードストア用ハッシュ
    const char *dispatcher_name;       // デバッグ用
    node_dispatcher_func_t dispatcher; // EVAL 時に呼ばれる関数
    VALUE rb_wrapper;                  // GC 用 T_DATA ラッパー
    enum jit_status jit_status;
    unsigned int dispatch_cnt;
    int32_t line;                      // ソース行番号 (backtrace 用)
};
```

`EVAL(c, n)` は `(*n->head.dispatcher)(c, n)` を呼ぶ。
コードストアで特殊化された場合、dispatcher は SD_* 関数に置き換わる。

## メソッド呼び出しの流れ

### node_func_call (暗黙 self-call: `foo(args)`)

レシーバが self であることがパース時に判明している呼び出し。
recv ノードを持たず、`c->self` を直接使う。

```
1. AB_CLASS_OF(c->self) → klass
2. インラインキャッシュチェック
   mc->klass == klass && mc->serial == abm->method_serial ?
   ├─ HIT:  dispatch_method_frame(c, n, klass, mc, ...)
   └─ MISS: node_method_call_slow(c, n, c->self, klass, ...)
             → メソッド検索 → method_cache_fill → dispatch
```

### node_method_call (明示的レシーバ: `obj.method(args)`)

レシーバを EVAL_ARG で評価する。self の save/restore が必要。

```
1. EVAL_ARG(recv) → recv_val  (間接呼び出し)
2. AB_CLASS_OF(recv_val) → klass
3. インラインキャッシュチェック
   mc->klass == klass && mc->serial == abm->method_serial ?
   ├─ HIT:  save self → c->self = recv_val
   │        dispatch_method_frame(c, n, klass, mc, ...)
   │        restore self
   └─ MISS: node_method_call_slow → dispatch_method_with_klass
```

### dispatch_method_frame (共通ディスパッチ)

cache hit パスで使用。self の save/restore はしない（呼び出し側が責任を持つ）。

```
1. フレーム push
   frame = { prev=current_frame, method=mc->method, klass, caller_node=call_site }
   c->current_frame = &frame

2. 呼び出し
   mc->body != NULL (AST)?
   ├─ YES: c->fp = save_fp + arg_index
   │       mc->dispatcher(c, mc->body)    ← 間接呼び出し
   │       c->fp = save_fp
   └─ NO:  mc->method->u.cfunc.func(c, c->self, argc, c->fp + arg_index)

3. フレーム pop
   c->current_frame = frame.prev
```

### dispatch_method_with_klass (非キャッシュパス)

cache miss、method_missing、super、算術フォールバックで使用。
一時的な method_cache を構築し、self の save/restore を行う。

```
1. method_cache_fill(c, &tmp_mc, klass, method)
2. save self → c->self = recv
3. dispatch_method_frame(c, ..., &tmp_mc, ...)
4. restore self
```

### フレームと backtrace の関係

```
呼び出し: main → b() → a() → raise

フレームチェーン:
  raise_frame { caller_node=a内のraise呼出行, method=Kernel#raise(CFUNC) }
    ↓ prev
  a_frame { caller_node=b内のa呼出行, method=a }
    ↓ prev
  b_frame { caller_node=main内のb呼出行, method=b }
    ↓ prev
  main_frame { source_file="test.rb", method=NULL }

backtrace 構築 (f->caller_node.line + f->prev->method.name):
  raise_frame.caller_node.line + a_frame.method    → "2:in `a'"
  a_frame.caller_node.line     + b_frame.method    → "5:in `b'"
  b_frame.caller_node.line     + main_frame         → "7:in `<main>'"
```

インライン例外 (0除算など) では PUSH_FRAME で軽量フレームを積み、
backtrace に例外発生行を含める。

### 算術演算のフォールバック

`a + b` はパース時に `node_fixnum_plus(a, b, arg_index)` になる。

```
node_fixnum_plus:
  lv = EVAL_ARG(left)
  rv = EVAL_ARG(right)
  両方 Fixnum?
  ├─ YES: tagged 加算 (1命令)
  └─ NO:  node_fixnum_plus_slow
            ├─ 両方 Integer? → swap_dispatcher to node_integer_plus
            └─ それ以外?     → swap_dispatcher to node_plus
                                → node_arith_fallback
                                   c->fp[arg_index] = rv
                                   dispatch_method_with_klass

arg_index はパース時に確保済みのスロット番号。
これにより fp 上の安全な位置に引数を配置できる。
swap_dispatcher はノードの dispatcher を書き換え、次回以降は新しいパスを通る。
```

### インラインキャッシュの無効化

```
method_serial (abm->method_serial):
  - メソッド定義 (abruby_class_add_method) でインクリメント
  - モジュール include でインクリメント

cache hit 条件:
  mc->klass == AB_CLASS_OF(recv) && mc->serial == abm->method_serial

serial が変わると全キャッシュが miss する (per-machine invalidation)。
初期値は 1。cache は 0 初期化なので初回アクセスは必ず miss。
```
