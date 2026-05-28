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

### 6.1 Binding と heap-promote

`binding` を取った瞬間に、 caller frame の lvar スロットを heap snapshot へ
コピー。 同時に `live_fp` / `live_frame_id` を覚えておき、 frame がまだ
生きている間は read/write を live_fp 側にも write-through する。 frame
epilogue で `c->bindings_head` を辿って snapshot を確定し、 live_fp は無効化。

```c
struct korb_binding {
    struct RBasic basic;
    VALUE *fp;            /* heap snapshot — primary 保管庫 */
    ID *names;            /* 表示順: [extras...,] primary, outer */
    uint32_t names_cnt, outer_names_cnt;
    VALUE *live_fp;       /* 元の caller fp; 生きてる間だけ valid */
    uint64_t live_frame_id;
    VALUE self, extra_vars, outer_vars;
    struct korb_cref *cref;
    const char *source_file; int source_line;
};
```

`Binding#eval(src [, file [, line]])` は `koruby_parse_with_scope_line` で
prism に scope_locals を渡し、 eval body 内の bare-name を local resolve
させる。 eval が新規 lvar を introduce すると binding に取り込み (extras
hash) + live frame に write-through。 nested eval / block-context eval も
含めて caller の slot を正しく辿るよう、 `c->fp` を `param_base` 分シフト
する。

`Proc#binding` は proc 捕捉済み env から Binding を組む — env は既に
heap-allocated (closure 化済) なので live tracking は不要。

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
make                          → koruby (interp 版、SD_* 関数なし)
./koruby --aot-compile s.rb   → 実行 + 各 AST から SPECIALIZE() を起動
                                  SD_<hash>(c, n) を `code_store/c/` に書き出し
                                  astro_cs_build() で `code_store/all.so` をリンク
./koruby s.rb                 → 起動時に `code_store/all.so` を auto-dlopen し、
                                  ALLOC_node_xxx 中の OPTIMIZE() が
                                  hash → SD ポインタ解決 (astro_cs_load) で swap
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

## 12. sp-based / RESULT ABI (新規約、 移行中)

C-local stale 問題 (precise GC 越し) と `c->state` 経路の散漫さを根本
解決するため、 全関数を **Lua 風の sp-based ABI** + **RESULT 返り値**
に統一する設計を採用 (Phase 1-9 の移行進行中)。

### 12.1 規約

すべての関数 (cfunc / EVAL_node body / C API helper) は **sp の引数** を
取り、 sp 周辺の slot で値をやりとりする。 過去の値は sp[-N..-1]、
自分の scratch は sp[0..]。

| function 種類 | signature | sp の意味 |
|---|---|---|
| cfunc | `RESULT cf(CTX *c, int argc, VALUE *sp)` | sp[-argc-1]=self, sp[-argc..-1]=args, sp[0..]=scratch |
| EVAL_node body | `RESULT EVAL_node_X(CTX *c, NODE *n, VALUE *sp, ...)` | parent の staging top |
| C API helper (固定 arity) | `RESULT h(CTX *c, VALUE *sp)` | sp[-N..-1]=args |
| C API helper (variadic) | `RESULT h(CTX *c, int argc, VALUE *sp)` | sp[-argc..-1]=args |
| 即値 helper (alloc しない) | 値で受けてよし | -- |

#### 例 (新 cfunc の ary_eq)

```c
static RESULT ary_eq(CTX *c, int argc, VALUE *sp) {
    /* sp[-2] = self (LHS Array), sp[-1] = other (RHS) */
    if (BUILTIN_TYPE(sp[-1]) != T_ARRAY) return RESULT_OK(Qfalse);
    long la = korb_ary_len(sp[-2]);
    if (la != korb_ary_len(sp[-1])) return RESULT_OK(Qfalse);
    for (long i = 0; i < la; i++) {
        /* sp[-2]/sp[-1] は GC 越しに auto-forward */
        if (!korb_eq(korb_ary_aref(sp[-2], i), korb_ary_aref(sp[-1], i))) {
            return RESULT_OK(Qfalse);
        }
    }
    return RESULT_OK(Qtrue);
}
```

### 12.2 RESULT 型

```c
typedef struct {
    VALUE   value;
    uint8_t state;
} RESULT;

#define RESULT_OK(v)        ((RESULT){(v), KORB_NORMAL})
#define RESULT_RAISE_R(v)   ((RESULT){(v), KORB_RAISE})
/* ... RESULT_BREAK_R / RESULT_RETURN_R / RESULT_NEXT_R / ... */

#define UNWRAP(call) ({ \
    RESULT _r = (call); \
    if (UNLIKELY(_r.state != KORB_NORMAL)) return _r; \
    _r.value; \
})

#define CHECK(call) ({ /* same as UNWRAP, discards value */ })
```

呼び出し側は:
```c
VALUE v = UNWRAP(some_helper(c, sp));   /* state は自動 propagate */
```

`c->state` 経路は撤廃され、 状態は in-band で関数の戻り値に乗る。
x86_64 ABI で RESULT (16 bytes) は rax+rdx の 2 register 返しなので
オーバーヘッド極小。

### 12.3 c->sp の同期

caller (= EVAL_node body や cfunc) は alloc を起こしうる helper を呼ぶ
**直前** に `c->sp = sp` で sync する。 これで visit_roots phase (a)
が staged value を scan 範囲に含める。

```c
EVAL_node_add(c, n, sp, lv, rv) {
    if (FIXNUM_P(lv) && FIXNUM_P(rv)) {
        return RESULT_OK(INT2FIX(FIX2LONG(lv) + FIX2LONG(rv)));  /* fast path = sync 不要 */
    }
    if (BUILTIN_TYPE(lv) == T_STRING) {
        c->sp = sp;                                   /* ← alloc 前 sync */
        return RESULT_OK(UNWRAP(korb_str_concat_r(c, &sp[-2], &sp[-1])));
    }
    ...
}
```

helper 側でも alloc を fire する直前に `c->sp = sp + N` を行う (自身の
scratch slot 確保も含む)。 sweep の規約はシンプル: **「alloc する
直前に c->sp を sync する」**。

### 12.4 dispatcher chain

```
EVAL_node_method_call
  → korb_dispatch_call_cached
    → prologue_cfunc_r_inl  (新 ABI)
      → mc->cfunc_r (c, argc, sp + argc + 1)
```

caller (= EVAL_node) が sp slot に self + args を spill 済の状態で
dispatcher を呼ぶ:

```c
sp[0] = recv;        /* self */
sp[1] = arg0;
sp[2] = arg1;
...
sp[argc] = argN-1;
c->sp = sp + argc + 1;
return korb_dispatch_call_cached_r(c, n, argc, sp + argc + 1, ...);
```

cfunc 側で:
- self は `sp[-argc-1]` (= 上の例の sp[0])
- args は `sp[-argc..-1]` (= sp[1..argc])
- scratch は `sp[0..]` (= sp[argc+1..])

### 12.5 Lua C API との対比

我々の規約は Lua の `lua_State *L` を介した stack API と本質的に同型:

| 概念 | Lua | koruby |
|---|---|---|
| stack 上の値 | L 内 (state-encapsulated) | c->sp 起点 (直接ポインタ) |
| arg 取得 | `lua_tonumber(L, i)` | `sp[-argc + i]` |
| 引数個数 | `lua_gettop(L)` | `argc` 直接渡し |
| scratch | `lua_push*` で stack に積む | `sp[0..]` に書く |
| 返値 | stack に push、 個数 return | RESULT 1 個 |
| GC 追跡 | stack 全域 | c->stack_base .. c->sp |
| 例外伝搬 | `lua_pcall` + setjmp | RESULT.state + UNWRAP |

### 12.6 移行 phase

| Phase | 内容 | 状態 |
|---|---|---|
| 1 | RESULT 型 / macro / typedef を foundation 追加 | 完了 (commit 03a5449f) |
| 2 | prologue_cfunc_r_inl + bridge | 完了 (commit 4352f5f5) |
| 3 | PoC: ary_eq を新 ABI で書き換え | 完了 (commit d1ae64a4) |
| 4 | 全 ~680 cfunc を新 signature に sweep | 部分完了 (math + boolean = 36 cfunc) |
| 5 | node.def の call 系 node を sp staging に | 未着手 |
| 6 | AST method prologue を sp 経由 args に | 未着手 |
| 7 | C API helper (korb_eq / korb_str_concat 等) を slot pointer 規約に | 未着手 |
| 8 | c->state 経路撤廃、 RESULT 化 | 未着手 |
| 9 | 動作確認 + 回帰 fix | 未着手 |

### 12.7 互換性 bridge

Phase 2-4 の移行期間中、 新 `cfunc_r` と旧 `cfunc` は両立する:
- `korb_method.u.cfunc.func_r` が non-NULL → 新 ABI
- `korb_method.u.cfunc.func` (旧 field) のみ → 旧 ABI
- `korb_dispatch_call_cached` と `korb_dispatch_to_method` 両方に bridge:
  新 ABI cfunc は dispatch 時に sp に self/args を stage して呼び、
  返り RESULT を c->state + VALUE に変換して upstream に返す。

すべて新 ABI に sweep 完了したら、 legacy field と bridge を撤廃する。
