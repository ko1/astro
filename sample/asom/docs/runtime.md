# asom ランタイム構造

## 全体像

asom は SOM (Simple Object Machine, 教育用 Smalltalk dialect) を ASTro
フレームワーク上に実装したもの。AST を直接 walking する interpreter と、
ASTro の partial evaluation で出力した SD (Specialized Dispatcher) コードに
よる AOT/PG 実行を切り替えられる。

```
asom 実行モデル
                                                          
  CTX (実行コンテキスト)                                     
    cls_object / cls_class / cls_integer / ... ← bootstrap classes
    val_nil / val_true / val_false              ← well-known instances
    frame ─────────────────────→ struct asom_frame (現在のフレーム)
    serial                                       ← 動的 IC 失効用       
                                                                       
  asom_frame (heap-allocated, leak)                                    
    self                       ← 現在のレシーバ                          
    locals[args + locals]      ← VALUE *（heap 上）                      
    method ──────────→ struct asom_method                               
    parent ──────────→ asom_frame (caller)                              
    home   ──────────→ asom_frame (^ NLR target)                        
    lexical_parent ──→ asom_frame (block の outer scope)                
                                                                       
  parsed AST tree                                                       
    NODE.head.dispatcher → DISPATCH_node_xxx (interp)                   
                       │ 　     OR                                        
                       └─→ SD_<hash> (AOT specialized)                  
                                                                       
  Code Store (AOT/PG)                                                   
    code_store/                                                         
      c/SD_<Horg>.c           ← ASTroGen specializer の出力             
      o/SD_<Horg>.o                                                     
      all.so                  ← 全 SD の dlopen 対象                      
      hopt_index.txt          ← (Horg, file, line) → Hopt (PGC 用)      
```

## VALUE 表現

タグ付き 64-bit ワード:

```
ビット 0=1   → SmallInteger (62-bit signed value)
ビット 0=0   → struct asom_object へのポインタ (8-byte aligned)
```

ヒープ上の各オブジェクトは `struct asom_object` 共通ヘッダで始まり、
継承クラスがフィールドを足す:

```c
struct asom_object {
    struct asom_class *klass;
    // followed by inline fields[]
};

struct asom_string  { struct asom_object hdr; size_t len; const char *bytes; };
struct asom_array   { struct asom_object hdr; uint32_t len; VALUE *data; };
struct asom_double  { struct asom_object hdr; double value; };
struct asom_block   { struct asom_object hdr; struct asom_method *method; struct asom_frame *home, *lexical_parent; VALUE captured_self; };
struct asom_class   { struct asom_object hdr; const char *name; superclass; metaclass; methods; class_side_fields; ... };
```

`nil` / `true` / `false` は `cls_nil` / `cls_true` / `cls_false` クラスの
シングルトンインスタンスとして boot 時に確保。`==` がそのまま identity
比較になる。

## クラス階層

```
                      cls_metaclass (klass = cls_metaclass)
                            ↑
                       (metaclass-of-metaclass)
                            │
   ┌──────────────────cls_class────────────────────┐
   │                  (klass=cls_metaclass)        │
   │                                               │
   ↓ superclass                                    ↓ superclass
   nil                                       cls_object class (Object's metaclass)
                                                   │
                                       ┌───────────┼──────────────┐
                                       ↓           ↓              ↓
                              cls_integer class   cls_array class  ...
                                       │
                                       ↓ superclass
                                  cls_object metaclass
```

各 bootstrap クラス (Object, Integer, String, ...) は **独自の metaclass** を
持つ。`cls->hdr.klass = its_metaclass`、`metaclass->superclass = parent's
metaclass` 。これで:

```
1 class                  → Integer
1 class class            → Integer's metaclass
1 class class class      → Metaclass
1 class class superclass → Object's metaclass    (← stdlib テストが要求)
```

ユーザクラス (`.som` ファイル) もロード時に同じ patten で metaclass が
作られる。`Foo class>>new = (^super new init)` のような class-side method の
super send が正しく Object's metaclass の `new` (= `class_new` primitive) に
解決される。

## メソッドディスパッチ

### Inline cache (per-callsite)

各 `node_send*` ノードは `struct asom_callcache *cc@ref` を inline 持つ。
@ref は ASTroGen が hash 計算と直書き込みコピーから外す annotation で、
ノードハッシュには寄与しないが per-callsite mutable side data として残る。

```c
struct asom_callcache {
    state_serial_t serial;          // c->serial と一致しなければ miss
    struct asom_class *cached_class; // 受信者のクラス
    struct asom_method *cached_method;
};
```

`asom_send` の hot path は header の `static inline` で、SD コードに直接
インライン展開される:

```c
static inline VALUE
asom_send(CTX *c, VALUE recv, const char *sel, uint32_t nargs, VALUE *args,
          struct asom_callcache *cc) {
    struct asom_class *cls = asom_class_of(c, recv);
    if (LIKELY(cc && cc->serial == c->serial && cc->cached_class == cls)) {
        return asom_invoke_method(c, cc->cached_method, recv, args, nargs);
    }
    return asom_send_slow(c, recv, sel, nargs, args, cc);  // miss / DNU
}
```

`asom_invoke_method` も header inline で、primitive arity 0–3 の case を直接
分岐:

```c
static inline VALUE
asom_invoke_method(CTX *c, struct asom_method *m, VALUE recv,
                   VALUE *args, uint32_t nargs) {
    if (LIKELY(m->primitive)) {
        switch (nargs) {
            case 0: return ((asom_prim0_t)m->primitive)(c, recv);
            case 1: return ((asom_prim1_t)m->primitive)(c, recv, args[0]);
            case 2: return ((asom_prim2_t)m->primitive)(c, recv, args[0], args[1]);
            case 3: return ((asom_prim3_t)m->primitive)(c, recv, args[0], args[1], args[2]);
        }
    }
    return asom_invoke_ast(c, m, recv, args, nargs);  // setjmp + heap frame
}
```

### Selector intern と SD 互換

selector 文字列は parse 時に `asom_intern_cstr` でインターン (FNV-64 ハッシュ
+ linear probe)。同名 selector はポインタ等しく、`cc->cached_class` 比較と
合わせれば cache hit が極小コスト。

ただし SD 生成コードは bare 文字列リテラル (`"whileTrue:"` 等) を
`asom_send` に渡すため、intern プールを通らない。`asom_class_lookup` は:

1. ポインタハッシュ probe (interned 経由の高速パス)
2. miss 時に linear strcmp スキャン (SD 由来のリテラル selector 用 fallback)

の二段で動く。

### `doesNotUnderstand:` / `unknownGlobal:` / `escapedBlock:`

- **DNU**: `asom_send_slow` で `asom_class_lookup` が NULL を返したとき、
  `doesNotUnderstand:arguments:` を receiver に送る。args は `Array` に
  まとめる。
- **Unknown global**: `node_global_get` で globals 表 + lazy class load の
  両方が NULL を返したとき、`unknownGlobal: aSymbol` を `c->frame->self` に
  送る。
- **Escaped block**: ブロックを `value` した時点で、すでに home メソッドが
  return 済みの場合、`asom_nonlocal_return` は home が見つからず
  `is_block_value=true` の unwind 点へ longjmp。`asom_block_invoke` 側の
  catch path が `escapedBlock: aBlock` を sender (caller frame) の self に
  送る。

## ブロックと NLR

```
method M:
  | x |
  ^ [ :y | x + y ] value: 10
       ↓
  asom_make_block:                                          
    block.method.body = AST                                 
    block.home          = M's frame                         
    block.lexical_parent = M's frame                        
    block.captured_self  = M's self                         

  asom_block_invoke (block value: arg):                     
    new_frame.self   = block.captured_self                  
    new_frame.locals = [arg, ...]                           
    new_frame.home   = block.home    (^ targets M)          
    new_frame.lexical_parent = block.lexical_parent         
    setjmp（escape catcher）                                 
    EVAL(c, m->body)                                         
```

`node_local_get(scope, idx)` は `lexical_parent` チェーンを `scope` 段
辿る。`scope` は parser がコンパイル時に決定。

`^` (`node_return_nlr`) は `asom_nonlocal_return` を呼んで `home` を unwind
スタックから探し、見つかれば longjmp。見つからない (home が return 済み)
ときは escape として `asom_block_invoke` 側のユニバーサル catch にジャンプし、
`escapedBlock:` を発動。

## 標準ライブラリ統合

bootstrap class は C で書いた primitive を持つ。stdlib の `.som` ファイルを
`asom_runtime_init` 後に **overlay merge** する:

- `<.som>` の method が `primitive` 宣言 → 既存 C primitive を残す
- `<.som>` の method に AST body あり、かつ既存に primitive 無し → AST 版を install
- `<.som>` の method に AST body あり、既存に primitive あり → スキップ (高速 primitive を優先)

これで stdlib の `Array>>sum` (= `^self inject: 0 into: [:s :e | s + e]`) や
`Vector>>append:` のような Smalltalk-side helper が使えるが、
`Array>>at:put:` のような hot path は C 直 (`arr_at_put_`) のまま。

## AOT / PG コンパイル

abruby 流儀 (`-c` / `-p`) を踏襲。

### `-c, --aot-compile-first`

```
1. asom_runtime_init + asom_install_primitives + asom_merge_stdlib
2. asom_load_class(entry)         ← user class + 依存をロード
3. (--preload で追加クラスを eager load)
4. for each registered entry:
     astro_cs_compile(body, NULL)  ← code_store/c/SD_<Horg>.c を生成
5. astro_cs_build(NULL)            ← make -j で .c → .o → all.so
6. astro_cs_reload()               ← 新 all.so を dlopen
7. for each registered entry:
     astro_cs_load(body, NULL)     ← dispatcher を SD_<Horg> に swap
8. run.
```

実行中、SD 関数は AST traversal を 1 つの巨大関数にインライン展開する
(EVAL_node_xxx は all `static inline`、子ノードの DISPATCH もコンパイル
時定数化)。ただし `asom_send` 内の primitive 呼出しは依然として関数
ポインタ間接呼出しで、これが現状 asom の hot path。

### `-p, --pg-compile`

```
1–3. interp で 1 度走らせる（dispatch_cnt が増える）
4. for each registered entry:
     dispatch_cnt < threshold → skip (cold)
     dispatch_cnt ≥ threshold → astro_cs_compile(body, file)
       (Hopt != Horg なら PGSD_<Hopt>.c + hopt_index.txt 行追加、
        Hopt == Horg なら SD_<Horg>.c のみ)
5. astro_cs_build(NULL)
```

asom は現在 `HOPT == HORG` (profile-aware ハッシュ未実装) なので、PG bake は
事実上 hot な entry に対する AOT bake と同じ。`hopt_index.txt` 行は記録
されるが Hopt 別 SD は emit されない。

### selector ポインタ vs SD

SD コードは selector を bare 文字列リテラルで埋め込む。intern プールを
通らないので、`asom_class_lookup` のポインタハッシュ probe が失敗する。
fallback の linear strcmp が拾うため動作は正しいが、cache miss 時の cost が
通常より重い。型特化ノード (`node_fixnum_plus` 等) を入れれば selector lookup
自体を消せる — TODO 参照。

## ファイル構成

```
sample/asom/
  README.md                — 概要 + 性能 + 使い方
  docs/                    — 設計・状態
    runtime.md             — このファイル
    done.md                — 実装済み機能
    todo.md                — 未実装 / 既知の課題
  Makefile                 — 生成タスク + test/bench/compare ターゲット
  asom_gen.rb              — ASTroGen subclass (custom operand types)

  context.h                — VALUE / CTX / class struct
  node.h                   — NodeHead / EVAL / OPTIMIZE / HORG / HOPT
  node.c                   — astro infra include + INIT
  node.def                 — 全 AST ノード定義 (25 種)

  asom_runtime.{h,c}       — オブジェクトモデル、IC / invoke fast path
  asom_loader.c            — .som file loader + stdlib overlay merge
  asom_parse.{h,c}         — lexer + recursive-descent parser
  asom_primitives.c        — ~200 個の C primitive

  main.c                   — CLI (フラグパース、AOT/PG ドライバ)

  test/                    — Bench.som / Suite.som / run_compare.sh / ...
  SOM/                     — submodule (SOM-st/SOM)
```

## 主要な型

| 型 | 定義場所 | 用途 |
|----|----------|------|
| `VALUE` | `context.h` | 全ての SOM 値。`intptr_t`、tag 付き |
| `CTX` | `context.h` | 実行コンテキスト。第一引数で渡し回す |
| `struct asom_class` | `context.h` | クラス本体 (instance + class-side fields, methods) |
| `struct asom_method_table` | `context.h` | open hash + ordered insertion list |
| `struct asom_callcache` | `asom_runtime.h` | per-callsite IC (`@ref` で hash 対象外) |
| `struct asom_frame` | `context.h` | 実行フレーム。heap allocated, リーク |
| `struct asom_block` | `context.h` | block closure (home + lexical_parent capture) |
| `struct NodeHead` | `node.h` | 全ノード共通ヘッダ。dispatcher fn ptr + flags |

## ASTro 連携

ASTroGen が `node.def` から生成するファイル:

| ファイル | 役割 |
|----------|------|
| `node_head.h` | 構造体宣言、`NodeKind` 表 |
| `node_eval.c` | `EVAL_node_xxx` (operand を取り出して semantic 関数を呼ぶ) |
| `node_dispatch.c` | `DISPATCH_node_xxx` (interp 用 default dispatcher) |
| `node_alloc.c` | `ALLOC_node_xxx` + `kind_node_xxx` 定義 |
| `node_hash.c` | `HASH_node_xxx` (Merkle ハッシュ) |
| `node_dump.c` | `DUMP_node_xxx` (debug pretty-print) |
| `node_specialize.c` | `SPECIALIZE_node_xxx` (SD コード generator) |
| `node_replace.c` | `REPLACER_node_xxx` (子ノード差し替え API) |

`@noinline` 付きノード (`node_method_body`, `node_block_body`) は
`SPECIALIZE_*` が空。エントリ境界として、内側の seq/return tree が SD 化対象。
そのため `m->body` には node_method_body wrapper を挟まず、内側の seq tree を
直接格納している。

ランタイム側は `runtime/astro_node.c` (HASH/DUMP/alloc_dispatcher_name) と
`runtime/astro_code_store.c` (cs_init/compile/build/reload/load) を `node.c` に
include して取り込む。
