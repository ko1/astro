# abruby ランタイム構造

## 全体像

```
abruby_machine (AbRuby インスタンスごと)
  running_ctx ─────────→ CTX (実行コンテキスト)
  stack[10000]           ← VALUE スタック (ローカル変数 + 引数)
  main_class_body        ← インスタンスごとの Object サブクラス
  gvars                  ← グローバル変数テーブル
  id_cache               ← rb_intern 結果のキャッシュ
  rb_self                ← Ruby 側の AbRuby インスタンス
  current_file           ← 実行中のファイルパス
  loaded_files           ← require 済みファイル一覧

CTX (実行コンテキスト、ノード評価関数の第1引数)
  abm ────────────────→ abruby_machine (所属する machine)
  vm  ────────────────→ abruby_vm_global (グローバル状態)
  env ────────────────→ stack[0]
  fp  ────────────────→ stack[N]  (現在のフレームポインタ)
  self                   ← 現在のレシーバ
  current_class          ← クラス定義中のクラス (通常 NULL)
  current_frame ───────→ abruby_frame linked list
  ids ─────────────────→ id_cache

abruby_vm_global (グローバル状態、プロセスに1つ)
  method_serial          ← メソッド定義時にインクリメント
```

CTX から machine のフィールドへはマクロでアクセス:
```c
CTX_MAIN_CLASS(c)  // → &c->abm->main_class_body
CTX_GVARS(c)       // → &c->abm->gvars
```

## データ構造

### abruby_machine (実行マシン)

AbRuby インスタンスごとに1つ。実行状態の全てを保持する。

```c
struct abruby_machine {
    CTX running_ctx;                     // 実行コンテキスト
    VALUE stack[ABRUBY_STACK_SIZE];      // VALUE スタック (10000)
    struct abruby_class main_class_body; // インスタンスごとの Object サブクラス
    struct abruby_gvar_table gvars;      // グローバル変数
    struct abruby_id_cache id_cache;     // ID キャッシュ (+, -, <, method_missing 等)
    VALUE rb_self;                       // Ruby 側の AbRuby インスタンス
    VALUE current_file;                  // 実行中ファイルパス
    VALUE loaded_files;                  // require 済みファイル一覧
};
```

### CTX (実行コンテキスト)

ノード評価関数の第1引数。実行中の状態を指す。

```c
struct CTX_struct {
    struct abruby_machine *abm;      // 所属する machine
    struct abruby_vm_global *vm;     // グローバル状態
    VALUE *env;                      // スタックのベース
    VALUE *fp;                       // フレームポインタ (ローカル変数の先頭)
    VALUE self;                      // 現在のレシーバ
    struct abruby_class *current_class;  // クラス定義の評価中のみ非 NULL
    struct abruby_frame *current_frame;  // フレーム linked list の先頭
    const struct abruby_id_cache *ids;   // ID キャッシュへのポインタ
};
```

### abruby_vm_global (グローバル状態)

全 machine インスタンスで共有。インラインキャッシュの無効化に使う。

```c
struct abruby_vm_global {
    uint32_t method_serial;  // メソッド定義ごとにインクリメント
};
```

### RESULT (実行結果)

全ノード評価の戻り値。2レジスタ (rax + rdx) に収まる。

```c
typedef struct {
    VALUE value;             // 値
    enum result_state state; // NORMAL / RETURN / RAISE / BREAK
} RESULT;
```

非局所脱出 (return, raise, break) は state で伝播する。
`UNWRAP(r)` マクロは state が NORMAL でなければ即座に return する。

### abruby_frame (呼び出しフレーム)

バックトレース用。C スタック上に確保し、linked list で管理。

```c
struct abruby_frame {
    struct abruby_frame *prev;      // 前のフレーム
    struct abruby_method *method;   // 実行中のメソッド (NULL = <main>)
    struct abruby_class *klass;     // レシーバのクラス (super 用)
    union {
        struct Node *caller_node;   // メソッドフレーム: 呼び出し元ノード
        const char *source_file;    // <main>: ファイルパス
    };
};
```

**caller_node 方式**: 各フレームが「自分がどこから呼ばれたか」を記録する。
backtrace 構築時は `f->caller_node->line`（行番号）と `f->prev->method->name`（メソッド名）をペアにする。

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
   mc->klass == klass && mc->serial == vm->method_serial ?
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
   mc->klass == klass && mc->serial == vm->method_serial ?
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
1. method_cache_fill(&tmp_mc, klass, method)
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
method_serial (abruby_vm_global.method_serial):
  - メソッド定義 (abruby_class_add_method) でインクリメント
  - CFUNC 登録 (abruby_class_add_cfunc) でインクリメント
  - モジュール include でインクリメント

cache hit 条件:
  mc->klass == AB_CLASS_OF(recv) && mc->serial == vm->method_serial

serial が変わると全キャッシュが miss する (global invalidation)。
```
