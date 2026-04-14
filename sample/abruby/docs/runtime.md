# abruby ランタイム構造

## 全体像

```
abruby_machine (AbRuby インスタンスごと)
  method_serial          ← メソッド定義時にインクリメント
  current_fiber ───────→ abruby_fiber (実行ファイバー)
    ctx                  ← CTX (実行コンテキスト、stack 含む)
  root_fiber             ← ブートストラップ (main) ファイバー
  main_class_body        ← インスタンスごとの Object サブクラス
  gvars                  ← グローバル変数テーブル (ab_id_table)
  id_cache               ← rb_intern 結果のキャッシュ
  rb_self                ← Ruby 側の AbRuby インスタンス
  current_file           ← 実行中のファイルパス
  loaded_files           ← require 済みファイル一覧
  loaded_asts            ← AST 保持 (require/eval で定義したメソッドの NODE が GC されないように)

CTX (実行コンテキスト、ノード評価関数の第1引数)
  abm ────────────────→ abruby_machine (所属する machine)
  stack[10000]           ← VALUE スタック (ローカル変数 + 引数)
  fp  ────────────────→ stack[N]  (現在のフレームポインタ)
  sp  ────────────────→ stack[M]  (GC 用 high-water mark)
  self                   ← 現在のレシーバ
  current_class          ← クラス定義中のクラス (通常 NULL)
  cref ───────────────→ abruby_cref linked list (定数スコープ)
  current_frame ───────→ abruby_frame linked list
  ids ─────────────────→ id_cache
  current_block ───────→ abruby_block (yield 実行中のブロック)
  current_block_frame ──→ abruby_frame (ブロック定義元フレーム)
```

CTX から machine のフィールドへはマクロでアクセス:
```c
CTX_MAIN_CLASS(c)  // → &c->abm->main_class_body
CTX_GVARS(c)       // → &c->abm->gvars
```

## データ構造

### abruby_fiber (ファイバー)

CTX（実行コンテキスト）を所有し、CRuby Fiber API でスタック切り替えを行う。

```c
struct abruby_fiber {
    struct abruby_class *klass;               // per-instance fiber_class
    enum abruby_obj_type obj_type;
    CTX ctx;                                  // 実行コンテキスト (own VALUE stack)
    enum abruby_fiber_state state;            // NEW / RUNNING / SUSPENDED / DONE
    bool is_main;                             // true for the bootstrap fiber
    VALUE proc_value;                         // Fiber.new で渡された Proc
    VALUE transfer_value;                     // resume/yield 間の値受け渡し
    unsigned int done_state;                  // body 完了時の RESULT state
    VALUE crb_fiber;                          // CRuby fiber (machine stack scan 用)
    void *crb_callback;                       // CRuby fiber callback struct
    struct abruby_fiber *resumer;             // resume を呼んだファイバー
    struct abruby_machine *abm;               // VM への back-pointer
    VALUE rb_wrapper;                         // GC 用 T_DATA ラッパー
};
```

### abruby_machine (実行マシン)

AbRuby インスタンスごとに1つ。

```c
struct abruby_machine {
    uint32_t method_serial;              // メソッドバージョン (キャッシュ無効化用)
    struct abruby_fiber *current_fiber;  // 現在実行中のファイバー
    struct abruby_class main_class_body; // インスタンスごとの Object サブクラス
    struct ab_id_table gvars;            // グローバル変数 (動的テーブル)
    struct abruby_id_cache id_cache;     // cached rb_intern results
    VALUE rb_self;                       // Ruby 側の AbRuby インスタンス
    VALUE current_file;                  // 実行中ファイルパス
    VALUE loaded_files;                  // require 済みファイル一覧
    VALUE loaded_asts;                   // AST 保持 (NODE が GC されないように)
    // Per-instance built-in classes (cloned from templates at create_vm)
    struct abruby_class *object_class, *integer_class, *float_class,
                        *string_class, *symbol_class, *array_class, *hash_class,
                        *range_class, *regexp_class, *rational_class, *complex_class,
                        *true_class, *false_class, *nil_class,
                        *kernel_module, *module_class, *class_class,
                        *runtime_error_class,
                        *method_class,       // Object#method 結果用
                        *proc_class,         // Proc.new / lambda / & 変換用
                        *fiber_class;        // Fiber.new / resume / yield 用
    struct abruby_fiber *root_fiber;         // ブートストラップ (main) ファイバー
};
```

**Per-instance 組み込みクラス**: 21 個の組み込みクラス（Integer, String, Float, Class, Module, Proc, Fiber 等）は `abruby_machine` インスタンスごとに独立する。`Init_abruby()` 時に静的なテンプレートクラス (`_body` 変数) にメソッドを登録し、`create_vm()` の `init_instance_classes()` でテンプレートからメソッドテーブルを clone して per-instance にコピーする。これにより、あるインスタンスでの `class Integer; def foo; end; end` が他のインスタンスに影響しない。

ランタイムコードはすべて `c->abm->xxx_class` 経由で per-instance クラスにアクセスする (`AB_CLASS_OF_IMM` も CTX を受け取って即値を per-instance クラスに解決する)。

### CTX (実行コンテキスト)

ノード評価関数の第1引数。実行中の状態を保持する。

```c
struct CTX_struct {
    struct abruby_machine *abm;              // 所属する machine
    VALUE stack[ABRUBY_STACK_SIZE];          // VALUE スタック (ローカル変数 + 引数)
    VALUE *fp;                               // フレームポインタ
    VALUE *sp;                               // GC 用 high-water mark
    VALUE self;                              // 現在のレシーバ
    struct abruby_class *current_class;      // クラス定義中のみ非 NULL
    const struct abruby_cref *cref;          // lexical 定数スコープチェーン
    struct abruby_frame *current_frame;      // フレーム linked list の先頭
    const struct abruby_id_cache *ids;       // ID キャッシュへのポインタ
    const struct abruby_block *current_block;       // yield 実行中のブロック
    const struct abruby_frame *current_block_frame; // ブロック定義元フレーム
};
```

gvars と main_class は machine 側に持ち、CTX からは `c->abm->` 経由でアクセスする。

**ブロック実行コンテキスト**: `yield` / `abruby_yield` は `current_block` と `current_block_frame` をセットする（旧値は C ローカルに退避）。`dispatch_method_frame` はこれらを一切触らない — `current_block_frame == current_frame` の条件は、メソッド呼び出しで新しいフレームが push されると自動的に false になるため、ブロックコンテキスト状態はフレームスコープで管理され、per-call の save/restore コストがゼロ。

### abruby_cref (定数スコープ)

lexical な定数解決チェーン。クラス定義やメソッド定義時にキャプチャされる。

```c
struct abruby_cref {
    struct abruby_class *klass;
    const struct abruby_cref *outer;
};
```

### RESULT (実行結果)

全ノード評価の戻り値。2レジスタ (rax + rdx) に収まる。

```c
typedef struct {
    VALUE value;             // 値
    unsigned int state;      // 低位 bit: NORMAL=0 / RETURN=1 / RAISE=2 / BREAK=4 / NEXT=8
                             // 上位 bit: 非ローカル return の skip-count (SHIFT=16)
} RESULT;
```

`state` は bit flag で、メソッド境界で「特定の bit を demote」する処理が `r.state &= ~MASK` の単一命令になる。例えばメソッド境界での RETURN catch は `r.state &= ~RESULT_RETURN_CATCH_MASK`（RETURN bit と skip bits を同時にクリア）、block-付き method 境界は さらに `RESULT_BREAK` も demote する。NEXT は `yield` サイトで demote される。

`UNWRAP(r)` マクロは state が NORMAL でなければ即座に return する。

### abruby_frame (呼び出しフレーム)

バックトレース用。C スタック上に確保し、linked list で管理。

```c
struct abruby_frame {
    struct abruby_frame *prev;            // 前のフレーム
    const struct abruby_method *method;   // 実行中のメソッド (NULL = <main>)
    union {
        const struct Node *caller_node;   // メソッドフレーム: 呼び出し元ノード
        const char *source_file;          // <main>: ファイルパス
    };
    const struct abruby_block *block;     // このメソッドが受け取った block (NULL = なし)
};
```

`super` 解決は `method->defining_class->super` を使い、`frame->klass` は持たない（slim 化済み）。

**caller_node 方式**: 各フレームが「自分がどこから呼ばれたか」を記録する。
backtrace 構築時は `f->caller_node->line`（行番号）と `f->prev->method->name`（メソッド名）をペアにする。

### abruby_block (ブロックリテラル)

call サイトで C スタック上に組み立て、frame.block にポイントさせる。`&block` パラメータや `Proc.new` で heap の `abruby_proc` に昇格可能。

```c
struct abruby_block {
    struct Node *body;                   // block body AST
    VALUE *captured_fp;                  // caller の fp（closure 環境）
    VALUE captured_self;                 // caller の self
    struct abruby_frame *defining_frame; // block を定義した enclosing メソッドフレーム
    const struct abruby_cref *cref;      // lexical 定数スコープ
    uint32_t params_cnt;                 // 必須パラメータ数
    uint32_t param_base;                 // captured_fp[param_base..] にブロック引数を書く
    uint32_t env_size;                   // heap Proc に escape する際の環境スロット数
};
```

**defining_frame**: block 実行中は `abruby_context_frame(c)` ヘルパーが `abruby_in_block(c)` なら `c->current_block->defining_frame` を返し、そうでなければ `c->current_frame` を返す。これにより block body 内の `yield`/`super` が lexical enclosing frame を参照する Ruby 仕様を実現する。

**非ローカル return（RESULT skip-count 方式）**: `RESULT.state` の上位ビット（`RESULT_SKIP_SHIFT = 16` 以降）に「RETURN を何回 method boundary で skip するか」を埋め込む。

- ブロック状態判定: `abruby_in_block(c)` ≡ `c->current_block_frame == c->current_frame`。`yield` / `abruby_yield` は `c->current_block_frame = c->current_frame` にセットしてブロック本体を評価する。ブロック本体から method を call すると新しい frame が push され、`c->current_frame` がシフトするため `abruby_in_block(c)` は自動的に false に切り替わる（その method の内側では yield/return は method 自身のものを参照）。これにより `dispatch_method_frame` は block 状態を一切 save/restore しなくて済む
- `node_return`:
  - ブロック本体を実行中（`abruby_in_block(c)`）: `c->current_frame` から `c->current_block->defining_frame` まで `prev` を辿って間の method frame 数を数え、skip-count として state に埋める
  - それ以外: skip-count = 0（最も近いメソッド境界で catch）
- method 境界（`dispatch_method_frame` / `dispatch_method_frame_with_block`）:
  - `r.state == 0` ならそもそも何もしない（非ブロック経路の fast path、CTX ロードなし）
  - `r.state & RESULT_RETURN`:
    - skip bits == 0 → `RESULT_RETURN_CATCH_MASK` で RETURN と skip bits をまとめてクリア（この frame が真の target）
    - skip bits > 0 → `RESULT_SKIP_UNIT` を 1 つ減らし、propagate

この方式は frame push 時に一切の追加仕事をせず、`frame_id_counter` / `frame.frame_id` / `return_target_frame_id` のフィールドを一切持たない。通常の `return` のコストは「`r.state` ゼロチェック 1 回」のみ。block 内 `return` という例外的な経路でだけ frame chain walk を払う。

**PUSH_FRAME / POP_FRAME マクロ**: インライン例外（0除算など dispatch_method_frame を経由しない箇所）で、例外発生位置を backtrace に含めるための軽量フレーム push/pop。

```c
PUSH_FRAME(node);     // caller_node = node のフレームを push
// ... abruby_exception_new(c, ...) ...
POP_FRAME();          // フレームを pop
```

### abruby_proc (Proc / lambda)

`Proc.new` / `proc` / `lambda` / `&block` パラメータで生成。ブロックの closure 環境を heap にコピーして保持する。

```c
struct abruby_proc {
    struct abruby_class *klass;        // per-instance proc_class
    enum abruby_obj_type obj_type;     // ABRUBY_OBJ_PROC
    struct Node *body;                 // block body AST
    VALUE *env;                        // heap-allocated closure 環境
    uint32_t env_size;                 // 環境スロット数
    uint32_t params_cnt;               // 必須パラメータ数
    uint32_t param_base;               // env[param_base..] にパラメータを配置
    bool is_lambda;                    // true なら lambda（return で自身だけ脱出）
    VALUE captured_self;               // closure self
    const struct abruby_cref *cref;    // lexical 定数スコープ
};
```

`abruby_block_to_proc()` がスタック上の `abruby_block` を heap の `abruby_proc` に変換する。

### method_cache (インラインキャッシュ)

各 node_method_call / node_func_call に `@ref` で埋め込まれる。

```c
struct method_cache {
    const struct abruby_class *klass;    // キャッシュしたクラス
    const struct abruby_method *method;  // キャッシュしたメソッド
    uint32_t serial;                     // キャッシュ時の method_serial
    uint32_t ivar_slot;                  // IVAR_GETTER/SETTER: キャッシュした ivar スロット番号
    struct Node *body;                   // method->u.ast.body (NULL = CFUNC)
    RESULT (*dispatcher)(CTX *, NODE *); // body->head.dispatcher
};
```

`body` と `dispatcher` をキャッシュすることで、cache hit 時に method 構造体を経由する間接参照 2段を省略する。`ivar_slot` は `attr_reader`/`attr_writer` の dispatch インライン化用。

### ivar_cache (ivar インラインキャッシュ)

各 node_ivar_get / node_ivar_set に `@ref` で埋め込まれる。

```c
struct ivar_cache {
    const struct abruby_class *klass;  // NULL なら未充填
    unsigned int slot;                 // obj->ivars への直接インデックス
};
```

### abruby_class (クラス)

```c
struct abruby_class {
    struct abruby_class *klass;             // メタクラス (= per-instance class_class)
    enum abruby_obj_type obj_type;          // CLASS or MODULE
    enum abruby_obj_type instance_obj_type; // このクラスのインスタンスの型
    ID name;
    struct abruby_class *super;             // 親クラス
    struct ab_id_table methods;             // key=ID, val=(VALUE)(struct abruby_method*)
    VALUE rb_wrapper;                       // GC 用 T_DATA ラッパー
    struct ab_id_table constants;           // key=ID, val=VALUE
    struct ab_id_table ivar_shape;          // ivar name → slot index (shape-based)
};
```

メソッド・定数・ivar shape は全て `ab_id_table`（動的ハッシュテーブル）で管理。メソッド検索は `abruby_class_find_method` で methods テーブルを検索し、見つからなければ super チェーンを辿る。

### ab_id_table (動的ハッシュテーブル)

小テーブル（capa ≤ 4）は packed linear、大テーブルは open-addressing Fibonacci ハッシュ。inline storage により小テーブルは別途 alloc 不要。

```c
#define AB_ID_TABLE_SMALL_CAPA 4

struct ab_id_table {
    unsigned int cnt;
    unsigned int capa;
    struct ab_id_table_entry *entries;
    struct ab_id_table_entry inline_storage[AB_ID_TABLE_SMALL_CAPA];
};
```

### abruby_method (メソッド)

```c
struct abruby_method {
    ID name;
    enum abruby_method_type type;            // AST / CFUNC / IVAR_GETTER / IVAR_SETTER
    const struct abruby_class *defining_class; // super chain 解決用
    union {
        struct {
            struct Node *body;
            unsigned int params_cnt;
            unsigned int locals_cnt;
            const char *source_file;
            const struct abruby_cref *cref;  // lexical 定数スコープ
        } ast;
        struct {
            abruby_cfunc_t func;             // RESULT (*)(CTX*, VALUE, uint, VALUE*)
            unsigned int params_cnt;
        } cfunc;
        struct {
            ID ivar_name;                    // attr_reader/writer 用 ivar 名
        } ivar_accessor;
    } u;
};
```

### オブジェクト表現

全ヒープオブジェクトは CRuby の T_DATA で、先頭に `klass` + `obj_type` を持つ。

```
即値 (Fixnum, Symbol, true, false, nil)
  → CRuby 即値をそのまま使用
  → AB_CLASS_OF → AB_CLASS_OF_IMM で対応クラスに解決

T_DATA ヒープオブジェクト
  → 先頭に klass + obj_type を持つ
  → AB_CLASS_OF は RTYPEDDATA_GET_DATA → klass で解決

  abruby_object   { klass, obj_type, ivar_cnt, *extra_ivars, ivars[4] }
                   4 slots inline + heap overflow (制限なし)
  abruby_string   { klass, obj_type, rb_str }              CRuby String ラッパー
  abruby_array    { klass, obj_type, rb_ary }              CRuby Array ラッパー
  abruby_hash     { klass, obj_type, rb_hash }             CRuby Hash ラッパー
  abruby_bignum   { klass, obj_type, rb_bignum }           CRuby Bignum ラッパー
  abruby_float    { klass, obj_type, rb_float }            CRuby Float ラッパー
  abruby_range    { klass, obj_type, begin, end, exclude_end }  独自構造
  abruby_regexp   { klass, obj_type, rb_regexp }           CRuby Regexp ラッパー
  abruby_rational { klass, obj_type, rb_rational }         CRuby Rational ラッパー
  abruby_complex  { klass, obj_type, rb_complex }          CRuby Complex ラッパー
  abruby_exception{ klass, obj_type, message, backtrace }  例外
  abruby_bound_method { klass, obj_type, recv, method_name } Method オブジェクト
  abruby_proc     { klass, obj_type, body, *env, ... }     Proc / lambda
  abruby_fiber    { klass, obj_type, ctx, state, ... }     Fiber
```

### NodeHead (AST ノードヘッダ)

全 AST ノード共通のヘッダ。ノード固有データ（オペランド）はこの後に続く。

```c
struct NodeHead {
    // cold zone (SPECIALIZE / HASH / GC only)
    node_hash_t hash_value;            // コードストア用ハッシュ
    const char *dispatcher_name;       // デバッグ用
    VALUE rb_wrapper;                  // GC 用 T_DATA ラッパー

    // warm zone
    struct NodeFlags flags;            // specialized, no_inline 等
    const struct NodeKind *kind;       // ノード種別 (関数ポインタ群)
    int32_t line;                      // ソース行番号 (backtrace 用)

    // hot zone (adjacent to union for cache locality)
    node_dispatcher_func_t dispatcher; // EVAL 時に呼ばれる関数
};
// sizeof(NodeHead) = 56  (parent/jit_status/dispatch_cnt は除去済み)
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
   frame = { prev=current_frame, method=mc->method, caller_node=call_site }
   c->current_frame = &frame

2. 呼び出し
   mc->body != NULL (AST)?
   ├─ YES: c->fp = save_fp + arg_index
   │       mc->dispatcher(c, mc->body)    ← 間接呼び出し
   │       c->fp = save_fp
   └─ NO:  mc->method->u.cfunc.func(c, c->self, argc, c->fp + arg_index)

   IVAR_GETTER / IVAR_SETTER の場合:
     → frame 構築をスキップし、直接 obj->ivars[slot] を読み書き

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
