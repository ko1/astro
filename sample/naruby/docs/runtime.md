# naruby ランタイム構造

言語仕様は [spec.md](spec.md)、実装済み機能は [done.md](done.md)、
未対応は [todo.md](todo.md) を参照。

naruby は ASTro フレームワーク (`../../lib/astrogen.rb` + `../../runtime/`)
の上に乗った最小サブセット Ruby インタプリタ。**Plain / AOT /
Profile-Guided / JIT** の 4 モードを 1 バイナリで切り替えるのが特徴。

## 1. パイプライン全景

```
   foo.na.rb
       │
       ▼
   Prism (libprism.so)
       │   pm_node_t* (CRuby と同じ Ruby AST)
       ▼
   transduce  (naruby_parse.c)
       │   PM_* → ALLOC_node_*  (未対応 PM_* は "unsupported" で exit)
       │   ローカル変数を slot 番号化、関数名を C string に確保
       ▼
   NODE * (heap, ASTroGen 形式)
       │
       ▼
   OPTIMIZE(ast)               ← node.c:88
       │   if (-i)            : 何もしない
       │   if (-j) jit submit : astro_jit_submit_query (UDS で L1 へ)
       │   else              : astro_cs_load(ast, NULL) で
       │                        code_store/all.so から SD_<hash> を dlsym
       ▼
   EVAL(c, ast)                ← node.h:64 (inline)
       │   (*ast->head.dispatcher)(c, ast)
       ▼
   再帰 dispatch (node_seq → node_call → node_scope → ...)
       │
       ▼
   VALUE (= int64_t)           ← printf("Result: %ld\n", ...)
       │
       ▼   (デフォルト or `-c` 時)
   build_code_store(ast)        ← main.c
       │   astro_cs_compile(ast, NULL)        ── code_store/c/SD_<hash>.c
       │   astro_cs_compile(各 code_repo body) ── 同上
       │   astro_cs_build(NULL)               ── make -C code_store all.so
       │   astro_cs_reload()                  ── dlopen 切り替え
```

## 2. NODE の構造

すべてのノードは `struct Node` で、`head` と `u`(union) からなる。

```c
struct Node {
    struct NodeHead head;
    union {
        struct node_num_struct       node_num;       // { int32_t num; }
        struct node_lget_struct      node_lget;      // { uint32_t index; }
        struct node_lset_struct      node_lset;      // { uint32_t index; NODE *rhs; }
        struct node_seq_struct       node_seq;       // { NODE *head; NODE *tail; }
        ...
    } u;
};
```

`union u` は `node.def` の `NODE_DEF` 宣言から ASTroGen が生成
(`node_head.h`)。各ノードのオペランド型がそのままフィールドになる。

`NodeHead` (node.h):

```c
struct NodeHead {
    struct NodeFlags {
        bool has_hash_value;      // hash_value 有効か
        bool has_hash_opt;        // hash_opt 有効か (PGC 用、現状 v0 で未使用)
        bool is_specialized;      // SD_<hash> に置換済み
        bool is_specializing;     // SPECIALIZE 中 (再帰検出用)
        bool is_dumping;          // DUMP 中 (再帰検出用)
        bool no_inline;           // SPECIALIZE 出力で head.dispatcher を直読みする
    } flags;

    const struct NodeKind *kind;  // ノード種別のメタ (kind table は ASTroGen 生成)
    struct Node *parent;          // (オプション機能、ASTRO_NODEHEAD_PARENT)

    node_hash_t hash_value;       // 構造ハッシュ (HORG)
    node_hash_t hash_opt;         // プロファイル含むハッシュ (HOPT、現状 == HASH)

    const char *dispatcher_name;  // "DISPATCH_node_xxx" or "SD_<hex>"
    node_dispatcher_func_t dispatcher; // VALUE (*)(CTX *c, NODE *n)

    enum jit_status { ... } jit_status;  // JIT 状態 (Querying / Compiling / ...)
    unsigned int dispatch_cnt;
    int line;                     // 元 .rb の行 (PGC 用、v0 未使用)
};
```

ハッシュは `node.def` 上のオペランドを再帰的に折り畳んで作る (ASTroGen
が `node_hash.c` を生成)。子 NODE は `hash_node()` を再帰呼び出し。
組み込み関数ポインタは `hash_builtin_func()` (naruby 独自、`bf->name` と
`bf->func_name` を結合) で扱う。

## 3. CTX とフレームポインタ

```c
typedef struct CTX_struct {
    VALUE *env;                          // 大域 VALUE スタック (malloc 確保)
    unsigned int func_set_cnt;
    struct function_entry *func_set;     // 関数テーブル (max 2000)
    state_serial_t serial;               // 関数定義版番。callcache 無効化に使う
} CTX;
```

ローカル変数フレームポインタ `fp` は **dispatcher の第 3 引数として
レジスタ渡し**しており、CTX には持たない。castro パターン。

```c
typedef RESULT (*node_dispatcher_func_t)(CTX *c, NODE *n, VALUE *fp);
```

各 NODE_DEF は `(CTX *c, NODE *n, VALUE *fp, ...)` で受け取る。

- `node_lget(idx)` / `node_lset(idx, rhs)` は `fp[idx]` を直接読み書き
  (`c->fp` のメモリ往復ゼロ)。
- `node_scope(envsize, body)` は `EVAL(c, body, fp + envsize)` で
  body を呼び、本体は新しい fp を見る。
- `node_call*` は `fp + arg_index` を callee に渡す (caller が
  予め `lset` で並べた arg slots がそのまま callee の局所スロット
  になる)。

`function_entry` (`name → body / params_cnt / locals_cnt`) は線形リスト。
名前検索は線形探索で、`callcache` が hit すればそれを使う。

## 3.1 RESULT 型と非局所脱出

`return` を setjmp なしで実装するため、dispatcher の戻り値は **2 ワード
の RESULT envelope** (= `{ VALUE value; unsigned int state; }`)。
`{rax, rdx}` 2 レジスタで戻るので関数呼び出しの ABI コストはほぼ変わらない。

```c
#define RESULT_NORMAL 0u
#define RESULT_RETURN 1u

typedef struct { VALUE value; unsigned int state; } RESULT;

#define RESULT_OK(v)        ((RESULT){(v), RESULT_NORMAL})
#define RESULT_RETURN_(v)   ((RESULT){(v), RESULT_RETURN})

#define UNWRAP(r) ({ RESULT _r = (r); \
    if (UNLIKELY(_r.state != RESULT_NORMAL)) return _r; \
    _r.value; })
```

各 NODE_DEF は子 EVAL の戻り RESULT を `UNWRAP` で受ける。state が
NORMAL なら value を取り出して続行、そうでなければそのまま return
で上に投げ返す。`return` の意味論は:

```
node_return         → emit  RESULT { value, RETURN }
  ↓ propagate up via UNWRAP
node_seq / node_if / node_while → return as-is
  ↓
call_body / node_call2 / node_call_static (function boundary)
  → if state == RETURN, set state = NORMAL (caught)
  → return RESULT { value, NORMAL } to caller of the function
```

最適化:

- インライン化された SD の連鎖では state が compile-time 0 で
  伝わるので、`if (UNLIKELY(state))` チェックは gcc が DCE する。
  非インライン境界 (関数呼び出し再帰、cross-SD call) でだけ
  状態チェックの分岐が残る — branch predictor がほぼ無料で扱う。
- naruby は今のところ RETURN 以外の状態 (BREAK / CONTINUE / GOTO)
  を使っていないので、関数境界の catch は単純な
  `if (state == RETURN) state = NORMAL` で済む。

## 4. EVAL_ARG とディスパッチャ

ASTroGen が生成する `node_eval.c` の頭:

```c
#define EVAL_ARG(c, n) (EVAL_ARG_CHECK(n), (*n##_dispatcher)(c, n))
```

`node_seq.head` を eval する場合、parser は予め `head_dispatcher` という
ローカル変数 (= `head->head.dispatcher`) を取って、`(*head_dispatcher)(c,
head)` を呼ぶ。これが「子ディスパッチャの load を 1 つ早く済ませる」
ための specialize-friendly な書き方。

specialize された SD_<hash> 関数では、子 dispatcher は **コンパイル時定数**
として埋め込まれる (`SD_<child_hash>` のシンボルを直接書く)。これにより:

- インタプリタ経路: indirect call (関数ポインタ越し)
- specialize 経路: direct call (シンボル決定済)

になる。これが ASTro の論文での評価対象。

## 5. 4 つの実行モード

### 5.1 Plain (`-i`)

最も単純。`OPTIMIZE` は何もせず、`head.dispatcher` は ASTroGen が出した
`DISPATCH_node_xxx` のまま。すべての子呼び出しは indirect call。

### 5.2 AOT (デフォルト or `-c`)

```
1st run:  parse → EVAL (interpreter で動く) → build_code_store
                    ↓
                 code_store/c/SD_<hash>.c が出る
                 make -C code_store → all.so 生成
                 dlopen → astro_cs.all_handle にハンドル

2nd run:  parse → OPTIMIZE
                    ↓
                 astro_cs_load(n, NULL) → dlsym("SD_<hash>")
                 hit すれば head.dispatcher を SD_ に書き換え
                 → EVAL は specialized code で走る
```

`-c` を付けると EVAL をスキップして 1st run を完了させる(コンパイル専用)。
`./naruby -c file.rb && ./naruby file.rb` がベイク → ホット実行の標準
2 ステップ。

`-b` は逆に build_code_store をスキップ (load はする)。ベンチで AOT
load 後の純粋実行時間を測るときに使う。

### 5.3 Profile-Guided (`-p`)

parser が `node_call` ではなく `node_call2` を吐く。`node_call2` は
`callcache` に加えて `sp_body` (specialized body) フィールドを持ち、
本体 NODE への直接リンクを保持する。AOT モードよりも 1 段ポインタを
少なくできるのが狙い (`node_call` の `cc->body` 経由は 2 段、
`node_call2` の `sp_body` 経由は 1 段)。

#### sp_body の link タイミング

`naruby_parse.c` の `callsite_resolve` (PARSE 末尾で呼ばれる) が、
`def f` 完了時点で `f` への pending callsite を全部巡回し、
`node_call2.sp_body` を本体 NODE に書き戻す。これで前方参照・自己再帰
どちらも parse 終了時には sp_body が解決済みになる。

```ruby
def fib(n)
  if n < 2
    1
  else
    fib(n - 2) + fib(n - 1)   # 自己再帰: parse 中は sp_body=placeholder
  end                          # callsite_resolve が def 終了後に patch
end

fib(40)                        # 順方向: parse 時点で sp_body=fib_body
```

#### sp_body と HASH の関係

「parse 時に link」を実現するためには、sp_body の patch がノードの
HASH を変えてはいけない。それと同時に、再帰関数の sp_body=自己 が
HASH 計算でサイクルを起こしてもいけない。両方を解決するため、
**`sp_body` は HASH の計算式から完全に除外**してある (`naruby_gen.rb`
の `hash_call(val, kind:)` で `'0'` を返す)。

ASTro の Merkle 原則は「同一構造 ≡ 同一ハッシュ」だが、特定パラメータを
ハッシュに含めない (= 0 を返す) のはフレームワーク内の合法な操作。
今回 sp_body を除外する代わりに、call site の SD は body 側のシンボル
名を `hash_node(sp_body)` から直接合成している (= sp_body の identity
は SD body 内に直接埋め込まれる) ので、結果として「sp_body の差は
SD body の識別に効く / call site のキャッシュキーには効かない」という
住み分けになる。

> ASTro 原則上の落とし穴: call site の specialize 中に **body NODE の
> head を書き換えない**こと。body は別エントリとしてベイクされる側で、
> その中で head.dispatcher_name を設定する。call site の build_specializer
> が `sp_body->head.dispatcher_name = ...` を触ると、ベイク順依存で
> 変なことが起きる。naruby_gen.rb は body の hash から直接シンボル名を
> 計算するだけで body には触らない。

結果として:

- `HASH(node_call2)` は `name`, `params_cnt`, `arg_index` だけから
  決まる安定値 (callcache や sp_body は無視)。
- `HASH(fib_body)` は fib の中身ノードの HASH を再帰的にたたんだ値。
  fib 内の node_call2 の hash は sp_body に依らないので、自己参照
  は HASH に登場せず無限再帰しない。

#### SD は direct call をベイク

`naruby_gen.rb` の `build_specializer` で sp_body の emission を
上書きし、SD は **`SD_<HASH(sp_body)>` を C 上の定数として埋め込む**。
ベイクされた all.so 内では `-Wl,-Bsymbolic` で intra-.so の参照が
直接アドレス解決され、`addr32 call <SD_addr>` の direct call まで
落ちる。`sp_body->head.dispatcher` のロードは出ない。

メソッド再定義 (`def f` 二度目) との整合は **slowpath での
dispatcher 降格** で取る:

- `cc->serial != c->serial` でキャッシュが古い → `node_call2_slowpath`
  に飛ぶ。
- slowpath は `cc->serial != 0` (= 「キャッシュは過去に埋まったが
  今は古い」= 再定義) なら、その node の `head.dispatcher` を
  default の `DISPATCH_node_call2` に巻き戻す。次回以降の呼び出しは
  interpreter 経路で `cc->body->head.dispatcher` 経由 indirect dispatch。
  正しいが SD が外れる。
- 初回 cold miss (`cc->serial == 0`) は再定義ではないので降格しない。
  cache を埋めるだけで SD は維持される。

つまり「再定義はそのままだと SD が陳腐化するが、降格すれば再定義後も
正しく動く」というトレードオフ。ベンチでは再定義は起きないので
SD が定常で効く。

#### 定常時の dispatch 経路

```c
SD_<call2_hash>(c, n, fp) {
    cc = &n->u.node_call2.cc;            // @ref で inline 構造体
    if (LIKELY(c->serial == cc->serial)) {
        sp_body = n->u.node_call2.sp_body;
        return SD_<HASH(sp_body)>(c, sp_body, fp + arg_index);  // direct
    }
    return node_call2_slowpath(c, n, fp, name, ..., cc);
}
```

定常時 (再定義していない / 全関数が AOT load 済み):

- load 1: `cc->serial`  (cc は `@ref` で NODE inline、1 ロード)
- compare: `c->serial`
- load 2: `n->u.node_call2.sp_body` (アドレス)
- direct call → ベイクされた `SD_<HASH(body)>`
- 子の dispatcher load は **不要** (定数ベイク済)

`cc->body` にはアクセスしないので、`cc` のメモリ参照は serial 1 word
分で済む。

実測では fib(40) AOT 1.21s → PG 0.91s の高速化が出ている
(詳細は [perf.md](perf.md))。

### 5.4 JIT (`-j`)

`astro_jit.c` (740 行) が独立した worker (L1 サーバ) と Unix Domain
Socket で通信。

```
naruby (L0)               L1 (別プロセス)
   │                          │
   │ submit_query(NODE *)    │
   ├─────────────────────────►│   PRELOAD: 既に baked か？
   │ ◄─────────────────────── │   yes → so_path 返却 (L0 が dlopen)
   │                          │
   │ submit_compile(NODE *)  │   no  → SPECIALIZED_SRC を送信
   ├─────────────────────────►│         L1 が gcc で .so を作る
   │ ◄─────────────────────── │         compiled の通知 → L0 が dlopen
```

L0 側の dispatch counter (`head.dispatch_cnt`) が閾値を超えると
`OPTIMIZE` (= `astro_jit_submit_query`) を 1 回呼ぶ。L1 が baked .so
を返したら `head.dispatcher` を差し替える。astro_code_store と JIT は
独立した dlopen ハンドル (`astrojit/l1_so_store/all.so` vs
`code_store/all.so`) を持つ。

## 6. ASTroGen 連携 (`naruby_gen.rb`)

`node.def` を読んで C コードを 8 ファイル生成する。naruby では
`NaRubyNodeDef < ASTroGen::NodeDef` で次のオペランド型を上書き:

| 型 | hash_call | dumper | specializer |
|---|---|---|---|
| `struct builtin_func *` | `hash_builtin_func(...)` | `bf:NAME` | 識別子直書き |
| `builtin_func_ptr` | 0 | `<ptr>` | `(builtin_func_ptr)func_name` (have_src 時) |
| `state_serial_t` | 0 | `<serial>` | フィールドそのまま |
| `struct callcache *` | 0 | `<cc>` | フィールドそのまま |

ハッシュ計算で `state_serial_t` / `callcache *` を 0 で吸収するのが要点。
これにより「同じ構造の関数呼び出し」は同じ hash になり、SD_ がキャッシュ
ヒットする。

## 7. ファイル一覧

```
sample/naruby/
├── main.c                   # main, code_repo, define_builtin_functions, build_code_store
├── naruby_parse.c           # Prism AST → naruby NODE (transduce)
├── naruby_gen.rb            # ASTroGen の naruby カスタム
├── node.def                 # ノード定義 (naruby 言語仕様の中核)
├── node.h                   # NodeHead / EVAL マクロ / public API
├── node.c                   # node_allocate, OPTIMIZE, INIT + 共有 runtime/ の include 集約
├── context.h                # CTX, OPTION, VALUE, builtin_func, callcache
├── bf.h                     # 組み込み関数 (narb_p / narb_zero / narb_add)
├── astro_jit.h              # JIT API (start/stop/submit_query/submit_compile)
├── astro_jit.c              # JIT 実装 (UDS + dlopen)
├── Makefile                 # ビルド & ベンチ
├── prism/                   # libprism のサブモジュール
├── bench/
│   ├── bench.rb             # ベンチランナー
│   ├── *.na.rb              # ベンチ題材
│   └── *.c                  # 同等 C コード (gcc 比較用)
├── docs/                    # このディレクトリ
│   ├── spec.md              # 言語仕様
│   ├── done.md              # 実装済み機能
│   ├── todo.md              # 未対応 / 既知の制限
│   ├── runtime.md           # このファイル
│   └── perf.md              # ベンチ結果
├── README.md                # English overview
├── test.na.rb               # 既定の test 入力
└── (生成物)
    ├── node_eval.c          # ASTroGen 出力 (eval ロジック)
    ├── node_dispatch.c      # ASTroGen 出力 (DISPATCH_*)
    ├── node_dump.c          # ASTroGen 出力 (DUMP_*)
    ├── node_hash.c          # ASTroGen 出力 (HASH_*)
    ├── node_specialize.c    # ASTroGen 出力 (SPECIALIZE_*)
    ├── node_replace.c       # ASTroGen 出力
    ├── node_alloc.c         # ASTroGen 出力 (ALLOC_*)
    ├── node_head.h          # ASTroGen 出力 (kind table, struct Node)
    └── code_store/          # 実行時生成
        ├── Makefile
        ├── c/SD_<hash>.c
        ├── o/SD_<hash>.o
        └── all.so
```

`node_exec.c` と `naruby_code.c` は現在使われていない (詳細は [todo.md](todo.md))。

## 8. runtime/ との接続

`node.c` の中での #include 順は意味がある:

```c
#include "node.h"
#include "context.h"
#include "astro_jit.h"

// User-provided
extern size_t node_cnt;
static NODE *node_allocate(size_t size) { ... }   // ASTroGen が呼ぶ
static void dispatch_info(...) { ... }             // 旧式 trace (no-op)

// 共有インフラ
#include "astro_node.c"        // hash_*, HASH, DUMP, alloc_dispatcher_name
                               // → static、node_allocate を見つけられるよう先に定義

// naruby 固有
static node_hash_t hash_builtin_func(...) { ... }  // astro_node の hash_cstr/merge を利用

#include "astro_code_store.c"  // SPECIALIZE, astro_cs_*
                               // → astro_node.c の HORG/HOPT/hash_node を利用

// User-provided (続き)
void clear_hash(NODE *) { ... }
NODE *OPTIMIZE(NODE *) { ... }                    // ここで astro_cs_load を呼ぶ
char *SPECIALIZED_SRC(NODE *) { ... }              // JIT 用 (astro_jit.c から呼ばれる)

// ASTroGen 生成
#include "node_eval.c"
... etc

void INIT(void) { astro_cs_init("code_store", ".", 0); }
```

`astro_node.c` は `hash_uint64` / `hash_double` を `__attribute__((unused))`
で囲ってあるので、naruby が使っていない型のハッシュ関数があっても警告は
出ない。

`astro_code_store.c` の関数のうち naruby が使うのは:

| 関数 | 用途 |
|---|---|
| `astro_cs_init` | 起動時に `code_store/all.so` を dlopen |
| `astro_cs_load(n, NULL)` | OPTIMIZE で SD_<hash> に差し替え |
| `astro_cs_compile(n, NULL)` | 終了時に SD_<hash>.c を書き出す |
| `astro_cs_build(NULL)` | `make -C code_store all.so` |
| `astro_cs_reload` | dlopen を新しい all.so に切り替え |
| `astro_cs_disasm(n)` | (任意) objdump でディスアセンブル |

PGC 用の `(HORG, file, line) → HOPT` 索引は naruby では使っていない
(`HOPT(n) HASH(n)` にしてある)。
