# runtime.md — koruby のランタイム解説

koruby の **どこで何が起きているか** を、ホットパス (メソッド呼出 / クロージャ / 例外) を中心に説明する。
詳細実装は `object.c`, `node.def` を参照。

## 1. VALUE 表現 — CRuby 互換

```
Qfalse  = 0x00       ...0000 0000
Qnil    = 0x08       ...0000 1000
Qtrue   = 0x14       ...0001 0100
Qundef  = 0x34       ...0011 0100

FIXNUM  = ...x01     low bit  = 1
FLONUM  = ...x10     low 2 bits = 0b10  (encoded double; 現状未使用 — Float はヒープ)
SYMBOL  = ...0c      low 8 bits = 0x0c
pointer = ...000     low 3 bits = 0
```

ヒープオブジェクトは全て `RBasic` で開始する。

```c
struct RBasic {
    VALUE flags;   // low 5 bits = T_xxx (T_OBJECT=1, T_CLASS=2, ...)
    VALUE klass;   // 所属クラスへのポインタ
};
```

`BUILTIN_TYPE(v)` は `((struct RBasic *)v)->flags & T_MASK` で型タグを取り出す。

なぜ CRuby と同じ表現にしたか:
- `RARRAY_LEN`/`RARRAY_PTR` 系の CRuby マクロをそのまま流用しやすい
- 将来 `array.c`/`hash.c` の CRuby 実装をコピーして使うのが楽
- ユーザ指示で「NaN-boxing は禁止」「CRuby 互換が便利」と明示された

## 2. 実行コンテキスト (CTX)

```c
typedef struct CTX_struct {
    VALUE *stack_base, *stack_end;   // 1M slot の VALUE スタック
    VALUE *fp;                       // 現フレームの起点 (locals[0] が fp[0])
    VALUE *sp;                       // GC scan の高水位
    VALUE self;
    struct korb_class *current_class;
    struct korb_cref *cref;          // 字句的定数スコープ
    const char *current_file;        // require_relative 解決用
    state_serial_t method_serial;
    int   state;                     // KORB_NORMAL / RAISE / RETURN / BREAK / NEXT
    VALUE state_value;
    struct korb_frame *current_frame;
} CTX;
```

スタックは **VALUE 配列1本** で、フレームは fp の前進 / 後退で表現される (Smalltalk VM 風)。

## 3. メソッド呼び出し

### 3.1 ノードレベル

呼び出しは parser が以下の3種類のいずれかを選ぶ:

| ノード | 意味 |
|---|---|
| `node_func_call(name, argc, arg_index, mc)` | 暗黙 self (`foo(args)`) |
| `node_method_call(recv, name, argc, arg_index, mc)` | 明示レシーバ (`x.foo(args)`) |
| `node_method_call_block(recv, name, argc, arg_index, blk, mc)` | ブロック付き |
| `node_func_call_block(name, argc, arg_index, blk, mc)` | 暗黙 self ブロック付き |

引数は parser が **先に staging slot** に書き込んでおく:

```ruby
foo(1+2, 3*4)
```

を parser は次のように展開する:

```
seq( seq( lvar_set(arg_index, plus(int(1), int(2))),
          lvar_set(arg_index+1, mul(int(3), int(4))) ),
     func_call(:foo, argc=2, arg_index=arg_index, mc) )
```

### 3.2 ランタイムレベル — `korb_dispatch_call`

```c
VALUE korb_dispatch_call(CTX *c, NODE *callsite, VALUE recv, ID name,
                         uint32_t argc, uint32_t arg_index,
                         struct korb_proc *block, struct method_cache *mc)
```

1. `klass = korb_class_of_class(recv)` でレシーバのクラスを取得
2. インラインキャッシュ判定: `mc->serial == method_serial && mc->klass == klass`
   - **ヒット**: `mc->method` を即使用、ここで klass 探索を完全省略
   - **ミス**: `korb_class_find_method(klass, name)` で継承チェイン探索 → `mc` を埋める
3. 種別判定:
   - `KORB_METHOD_CFUNC`: そのまま `cfunc(c, recv, argc, &c->fp[arg_index])`
   - `KORB_METHOD_AST`: 以下のフレーム作業
4. AST メソッドのフレーム遷移:
   - `prev_fp = c->fp` を保存
   - `c->fp = prev_fp + arg_index` — 呼出元の staging 領域がそのまま被呼出メソッドのローカル領域になる (引数は fp[0..argc-1] に既にある)
   - `argc < locals_cnt` の余地分は `Qnil` で埋める (GC 安全)
   - `current_frame` を push (super / backtrace 用)
   - 直接 `mc->dispatcher(c, mc->body)` を呼ぶ — これは AOT 特化済みの `SD_<hash>` 関数になる
   - 戻ったら `c->fp = prev_fp`、frame pop
5. `c->state == KORB_RETURN` ならここで catch (`return` がメソッドの境界で消滅)

### 3.3 method_cache の中身

```c
struct method_cache {
    state_serial_t serial;
    struct korb_class *klass;
    struct korb_method *method;
    struct Node *body;             // mc->method->u.ast.body
    korb_dispatcher_t dispatcher;  // body->head.dispatcher (AOT 特化済み)
    uint32_t locals_cnt;
    uint32_t required_params_cnt;
    uint8_t  type;                 // 0=AST, 1=CFUNC
    VALUE (*cfunc)(...);
};
```

`body` と `dispatcher` を直接持つことで:
- `mc->method->u.ast.body->head.dispatcher` の **3 段階間接参照を 1 段階に減らす**
- AOT 特化された SD_xxx 関数を直接呼べる

`method_serial` はメソッド定義のたびに +1。これによりキャッシュ全体を無効化できる。

## 4. クロージャ (yield ベース)

ブロック (`{ ... }` / `do ... end`) は **共有 fp** が原則。
ただし block 体に `proc { }` などの inner block_literal が含まれる場合は
**fresh-env-with-writeback** に切り替わる (per-iteration capture のため)。

### 4.1 ブロック作成 (`node_block_literal`)

```c
NODE_DEF @noinline
node_block_literal(CTX *c, NODE *n, NODE *body, uint32_t params_cnt,
                   uint32_t param_base, uint32_t env_size, uint32_t creates_proc)
{
    VALUE p = korb_proc_new(body, c->fp, env_size, params_cnt, param_base, c->self, false);
    if (creates_proc) ((struct korb_proc *)p)->creates_proc = true;
    return p;
}
```

- `c->fp` を **そのまま env として保存** — コピーしない (fast path)
- `env_size` = ブロック内で使う最大 slot (parser が `frame->max_cnt` から決める)
- `param_base` = ブロックの第1パラメータが入る fp slot (= `slot_base` of block frame)
- `creates_proc` = parse 時に「body 中に `proc { }` / lambda / `->()` リテラルが含まれる」と検出されたフラグ

parse 時の検出:
```c
// parse.c push_frame(): 新しい block 子 frame を push する瞬間に、
// 親 frame chain を上方向に walk して全ての is_block 親 の has_inner_block を立てる
if (is_block) {
    for (frame *p = f->prev; p; p = p->prev) {
        if (p->is_block) p->has_inner_block = true;
    }
}
// pop_frame() 時に has_inner_block をその block の `creates_proc` 引数として
// node_block_literal_xxx に渡す
```

### 4.2 yield fast path (共有 fp、`creates_proc == false`)

```c
VALUE korb_yield(CTX *c, uint32_t argc, VALUE *argv) {
    struct korb_proc *blk = current_block;
    if (UNLIKELY(blk->creates_proc)) return korb_yield_slow(c, blk, argc, argv);
    VALUE *fp = blk->env;   // 親と同じ fp
    fp[blk->param_base] = argv[0];   // 単一引数 fast path
    c->fp = fp;
    return EVAL(c, blk->body);  // body は共通 fp で動く
}
```

- `c->fp` を blk->env (親 fp) に切替
- ブロックが親の `s` を参照すれば fp[親の slot] を直接見る (= 親と同じメモリ)
- ブロックが書き込めば親の値も即更新される (closure semantics ✓)
- `each` / `map` / `reduce` body のような proc-を-作らない一般ケースでは
  追加の allocation なしで動く

### 4.3 yield slow path (fresh env、`creates_proc == true`)

```c
VALUE korb_yield_slow(CTX *c, struct korb_proc *blk, ...) {
    VALUE *fp;
    VALUE *outer_env_ptr = blk->env;
    if (blk->creates_proc) {
        fp = malloc(blk->env_size * sizeof(VALUE));
        memcpy(fp, blk->env, blk->env_size * sizeof(VALUE));   // outer も含めて全コピー
    } else {
        fp = blk->env;
    }
    /* ... params 書き込み、body eval ... */
    if (blk->creates_proc) {
        /* outer slot だけ shared env に書き戻す */
        for (i = 0; i < blk->param_base; i++) outer_env_ptr[i] = fp[i];
    }
}
```

これで:
- `(1..3).each { |i| procs << proc { i } }` の各 iter は fresh fp を持ち、
  内側 proc が異なる env に bind される → 後で `procs.map(&:call)` が `[1, 2, 3]`
- `[1,2,3].each { |x| count += 1 }` は fresh fp で count 読み書きするが、
  block 終了時に outer の `count` slot に copy back → ちゃんと 3 になる

### 4.4 escape 対応

`korb_proc_snapshot_env_if_in_frame` が enclosing method 終了時に
proc.env を heap copy する。 これで「method を return して proc を返す」
パターンが安全に動く。

### 4.5 proc.call の env

proc.call は **blk->env を直接共有**する (snapshot しない)。 過去には
snapshot していて `r = ...` が outer に伝搬しなかったが、 escape 時の
snapshot で env はすでに heap にあるので直接利用で正しい。

## 5. 例外伝搬 (state propagation)

setjmp/longjmp は使わない。代わりに `CTX::state` を毎 EVAL_ARG 後にチェック。

### 5.1 EA マクロ

`node.def` で内部的に使うラッパ:

```c
#define EA(c, n) ({                                       \
    VALUE _v = EVAL_ARG(c, n);                            \
    if (UNLIKELY((c)->state != KORB_NORMAL)) return Qnil; \
    _v;                                                   \
})
```

`UNLIKELY` で正常パスは予測ヒット率が高い。

### 5.2 raise 系

```c
NODE_DEF
node_raise(...)
{
    ...
    c->state = KORB_RAISE;
    c->state_value = exc_obj;
    return Qnil;
}

void korb_raise(CTX *c, ko_class *klass, const char *fmt, ...) {
    /* 同様 */
}
```

### 5.3 rescue / ensure

```c
NODE_DEF @noinline
node_rescue(CTX *c, NODE *n, NODE *body, NODE *rescue_body, uint32_t exc_idx)
{
    VALUE v = EVAL_ARG(c, body);
    if (c->state == KORB_RAISE && rescue_body) {
        VALUE exc = c->state_value;
        c->state = KORB_NORMAL;
        c->fp[exc_idx] = exc;
        /* $! を rescue body の間だけ exc に挿げ替える。 ensure 後に
         * 復元するため prev_bang を保存しておく。 */
        VALUE prev_bang = korb_gvar_get(korb_intern("$!"));
        korb_gvar_set(korb_intern("$!"), exc);
        v = EVAL_ARG(c, rescue_body);
        korb_gvar_set(korb_intern("$!"), prev_bang);
    }
    return v;
}
```

bare `raise` (引数なし) は `$!` の値を再 raise:
```c
if (argc == 0) {
    VALUE bang = korb_gvar_get(korb_intern("$!"));
    if (!NIL_P(bang)) { c->state = KORB_RAISE; c->state_value = bang; }
    else korb_raise(c, NULL, "unhandled exception");
}
```

NODE_DEF
node_ensure(CTX *c, NODE *n, NODE *body, NODE *ensure_body)
{
    int saved_state = KORB_NORMAL;
    VALUE saved_value = Qnil;
    VALUE r = EVAL_ARG(c, body);
    if (c->state != KORB_NORMAL) {
        saved_state = c->state;
        saved_value = c->state_value;
        c->state = KORB_NORMAL;
    }
    EVAL_ARG(c, ensure_body);
    if (c->state == KORB_NORMAL && saved_state != KORB_NORMAL) {
        c->state = saved_state;
        c->state_value = saved_value;
    }
    return r;
}
```

### 5.4 全 state 一覧

| state | 用途 | 設定箇所 |
|---|---|---|
| `KORB_NORMAL` | 通常 | (defalt) |
| `KORB_RAISE` | 例外 raise 中 | `node_raise` / `korb_raise` |
| `KORB_RETURN` | メソッド return | `node_return` |
| `KORB_BREAK`  | ループ break / yield 中の break | `node_break` |
| `KORB_NEXT`   | ループ next / yield 中の next | `node_next` |
| `KORB_REDO`   | block redo | `node_redo` |
| `KORB_RETRY`  | rescue 中の retry | `node_retry` |
| `KORB_THROW`  | catch/throw の unwind | `kernel_throw` |

`KORB_THROW` は state_value に `[tag, value]` の 2-element Array を載せる。
`kernel_catch` は受信側で tag 比較 → 一致なら state を NORMAL に戻して
value を返す。 不一致は state を維持して呼出元へ伝搬。

### 5.5 setjmp/longjmp と比べて

| 項目 | state 伝搬 | setjmp/longjmp |
|---|---|---|
| 正常パスのコスト | 各 EVAL_ARG で `cmp+je` | ゼロ |
| 例外パスのコスト | 連続 return | longjmp 1回 + register restore |
| C コンパイラ最適化 | 全コード見える → DCE 可 | setjmp barrier で阻害される |
| portable | はい | はい |
| ASTro 特化と相性 | ◎ (部分木で state チェック消える) | △ |

abruby は同じ思想で `RESULT { VALUE, state }` を 2 レジスタ返り値にしている。性能は近いが、koruby は実装簡略化のため CTX フィールドにしてある。

## 6. 字句的定数スコープ (cref)

```c
struct korb_cref {
    struct korb_class *klass;
    struct korb_cref *prev;
};
```

`module Foo; class Bar; X end end` のとき、`X` の lookup は:

1. innermost cref (= `Bar`) で `const_get`
2. なければ `Foo`
3. なければ `Bar` の super チェイン (`Object` まで)
4. なければ `Object`
5. それでもなければ `NameError` を raise

`node_class_def` / `node_module_def` で:

```c
struct korb_cref new_cref = { .klass = klass, .prev = c->cref };
c->cref = &new_cref;
EVAL_ARG(c, body);
c->cref = new_cref.prev;
```

スタック上に確保 → 終了時に自動破棄。

## 7. インスタンス変数 (shape ベース)

CRuby の object_shapes に倣った **クラス単位の slot 配列**:

```c
struct korb_class {
    ID *ivar_names;   // [@x, @y, @z, ...]
    uint32_t ivar_count, ivar_capa;
    ...
};
struct korb_object {
    struct RBasic basic;
    uint32_t ivar_cnt, ivar_capa;
    VALUE *ivars;     // 配列 — クラスの slot に対応
};
```

書き込み:
1. `ivar_slot_assign(klass, name)` で slot 番号を確定 (なければ klass に追加)
2. 自分の `ivars` 配列を必要なら拡張
3. `ivars[slot] = v`

これにより:
- ivar 取得は配列アクセス 1 回 (CRuby のような shape transition tree より単純)
- klass 内ハッシュ検索は **書き込み初回のみ**

ただし shape evolution に対応していないので、同じクラスの異なるインスタンス間で ivar 配列のレイアウトは固定 — つまり `Foo` クラスのある instance に `@x` だけ、別 instance に `@y` だけ書くと、後者の instance の `@y` は slot 1 になる (前者は slot 0 の @x のみ)。slot 衝突しないので OK だがメモリ効率は劣る。

## 8. 文字列 / 配列 / ハッシュ

```c
struct korb_string { struct RBasic basic; char *ptr; long len, capa; };
struct korb_array  { struct RBasic basic; VALUE *ptr; long len, capa; };
struct korb_hash   { struct RBasic basic; struct korb_hash_entry **buckets;
                     uint32_t bucket_cnt, size;
                     struct korb_hash_entry *first, *last;  // 挿入順
                     VALUE default_value; };
```

- 文字列: 単純な `realloc` ベース。ASCII バイト前提
- 配列: `realloc` ベース。GC scan は Boehm が自動
- ハッシュ: bucket + insertion-order リンクリスト (Ruby の挙動に合わせて enumerate 順は挿入順)

未対応: small / embed 配列、open addressing、Robin-Hood 等の最適化。

## 9. ファイルロード (require / require_relative)

```
korb_load_file(c, path)
  → 既ロード check  (循環防止)
  → fopen + read all
  → korb_eval_string(c, src, len, path)
       → koruby_parse(...)
       → save c->{fp,self,current_class,cref,current_file}
       → set up top-level state (fp = sp+1, self = main_obj, cref = [Object])
       → c->current_file = path
       → OPTIMIZE(ast); EVAL(c, ast)
       → restore
```

`require_relative` は `dirname(c->current_file) + name + ".rb"` で resolve する。

## 10. AOT 特化フロー

```
make                     → koruby (interp 版、SD_*関数なし)
./koruby -c script.rb    → 実行 + node_specialized.c 出力
                              SPECIALIZE() が AST をルートから DFS
                              各ノードに対し SD_<hash>(c, n) を生成
                              dispatcher = EVAL_<kind>(...) を直接呼ぶ
                              子ノードは dispatcher_name を引用して SD 直呼び
touch node.c && make     → SD_xxx を埋め込んで再ビルド
                              `#include "node_specialized.c"` で取り込み
                              INIT() が sc_entries[] を sc_repo に登録
                              ALLOC_node_xxx 中の OPTIMIZE() が hash → SD swap
./koruby script.rb       → 実行時に各ノードの dispatcher が SD_xxx に置換済み
                              C コンパイラがインライン展開で大規模に最適化
```

`SD_<hash>` 関数は再帰的 static inline で構成され、ASTro の Merkle ハッシュにより **同形のサブツリーは同じ SD を共有**する。

## 11. 主要なデータフロー — `fib(n) = fib(n-1) + fib(n-2)` の例

```
fib(35)         (top-level)
  → node_func_call("fib", 1, 0, mc)
       miss → korb_class_find_method(Main, :fib) → ast method
       fill mc; c->fp = prev_fp + 0 (args at fp[0..0])
       call mc->dispatcher(c, mc->body) = SD_<fib_body>
         body:
           if (n < 2) ...
              → node_lt(lvar_get(0), int_lit(2), arg_index)
                 FIXNUM_P(l) && FIXNUM_P(r) → 高速パス
           else
              → node_plus(call(fib, n-1), call(fib, n-2), arg_index)
                 call hits cache (mc->serial == method_serial)
                 dispatcher 直呼び → 再帰
              FIXNUM 加算で高速パス → 完了
       restore fp
```

ホットパスの呼出ごとに発生するオーバヘッド:
- mc キャッシュ読み (1 メモリアクセス)
- フレーム push/pop (`fp += arg_index` / `fp = prev_fp`、locals zero)
- state 復帰チェック (`if state == RETURN`)

これらが naruby に比べて多く、現状 YJIT より遅い理由。詳細は [perf.md](./perf.md)。
