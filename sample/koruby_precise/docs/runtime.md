# runtime.md — koruby のランタイム解説

> ⚠ **これは v1 (2026-05 時点) の文書**。koruby は 2026-06 に slots ABI で全面再構築
> (v2) されており、**具体的な値・ファイル名はもう現行と一致しない**。ホットパスの
> 考え方や用語の説明として読む分には有効なので残してある。
>
> 現行と違う代表例:
> - ファイル: v1 の `object.c` は無い。現行は `korb_runtime.c` + `builtins/*.c`。
> - VALUE 表現: §1 の CRuby 互換タグ (Qnil=0x08 / Qtrue=0x14 / SYMBOL low 8bit=0x0c、
>   flonum 未使用) は **v2 で変わった** — 現行は `context.h` のとおり
>   nil=0 / false=4 / true=20 / undef=36、Symbol は `(id<<4)|0xC`、**flonum は実装済み**
>   (`FLONUM_P(v) = (v & 3) == 2`)。
> - sp / staging: v2 は slots ABI (`docs/v2_design.md`)。v1 の sp 二本問題は
>   [closure_sp_model.md](./closure_sp_model.md) の総括どまり。
>
> 現行の設計は [v2_design.md](./v2_design.md) / [v2_spec.md](./v2_spec.md)、
> rooting 規約は [rooting_guide.md](./rooting_guide.md) を見ること。

koruby の **どこで何が起きているか** を、ホットパス (メソッド呼出 / クロージャ / 例外) を中心に説明する。

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

## 5. 例外伝搬 (state propagation) — Phase 8d で RESULT 化

setjmp/longjmp は使わない。 全 dispatcher / NODE_DEF body が `RESULT` を
返し、 state は in-band (= VALUE+state の 2 register) で伝搬する。
abruby / castro と同じ設計。

```c
typedef struct {
    VALUE value;
    uint8_t state;     /* KORB_NORMAL / KORB_RAISE / ... */
} RESULT;
```

### 5.1 EVAL_ARG_UNWRAP マクロ

`node.def` で内部的に使うラッパ:

```c
#define EVAL_ARG(c, n) (EVAL_ARG_CHECK(n), (*n##_dispatcher)(c, n, sp))
#define EVAL_ARG_UNWRAP(c, n) UNWRAP(EVAL_ARG(c, n))

/* UNWRAP は context.h 由来 — 非 NORMAL は早期 return RESULT。 */
#define UNWRAP(call) ({                                   \
    RESULT _r = (call);                                   \
    if (__builtin_expect(_r.state != KORB_NORMAL, 0))     \
        return _r;                                        \
    _r.value;                                             \
})
```

`UNLIKELY` で正常パスは予測ヒット率が高い。 値が要らない場合は `CHECK(call)`。

### 5.2 raise 系

```c
NODE_DEF
node_raise(CTX *c, NODE *n, VALUE *sp, NODE *exc_expr)
{
    VALUE e = ...;
    return (RESULT){ e, KORB_RAISE };
}

RESULT korb_raise(CTX *c, struct korb_class *klass, const char *fmt, ...) {
    /* 同様、 (RESULT){exc, KORB_RAISE} を返す */
}
```

break / next / retry / redo / return も同じパターン。

### 5.3 rescue / ensure (RESULT-native)

```c
NODE_DEF
node_rescue(CTX *c, NODE *n, VALUE *sp, NODE *body, NODE *rescue_body, uint32_t exc_idx)
{
retry:;
    VALUE prev_bang = korb_gvar_get(korb_intern("$!"));
    RESULT _br = EVAL_ARG(c, body);
    if (_br.state == KORB_RAISE && rescue_body) {
        VALUE exc = _br.value;
        c->current_frame->fp[exc_idx] = exc;
        korb_gvar_set(korb_intern("$!"), exc);
        RESULT _rr = EVAL_ARG(c, rescue_body);
        if (_rr.state == KORB_RETRY) goto retry;
        /* ... */
        return _rr.state == KORB_NORMAL ? RESULT_OK(_rr.value) : _rr;
    }
    return _br;
}

NODE_DEF
node_ensure(CTX *c, NODE *n, VALUE *sp, NODE *body, NODE *ensure_body)
{
    VALUE prev_bang = korb_gvar_get(korb_intern("$!"));
    RESULT _br = EVAL_ARG(c, body);
    if (_br.state == KORB_RAISE) korb_gvar_set(korb_intern("$!"), _br.value);
    RESULT _er = EVAL_ARG(c, ensure_body);
    /* ensure body の state が wins。 さもなくば body の state を伝搬。 */
    if (_er.state != KORB_NORMAL) return _er;
    if (_br.state != KORB_RAISE) korb_gvar_set(korb_intern("$!"), prev_bang);
    return _br;
}
```

### 5.4 全 state 一覧

| state | 用途 | 設定箇所 |
|---|---|---|
| `KORB_NORMAL` | 通常 | (default) |
| `KORB_RAISE` | 例外 raise 中 | `node_raise` / `korb_raise` |
| `KORB_RETURN` | メソッド return | `node_return` |
| `KORB_BREAK`  | ループ break / yield 中の break | `node_break` |
| `KORB_NEXT`   | ループ next / yield 中の next | `node_next` |
| `KORB_REDO`   | block redo | `node_redo` |
| `KORB_RETRY`  | rescue 中の retry | `node_retry` |
| `KORB_THROW`  | catch/throw の unwind | `kernel_throw` |

### 5.5 setjmp/longjmp と比べて

| 項目 | RESULT 伝搬 | setjmp/longjmp |
|---|---|---|
| 正常パスのコスト | 各 EVAL_ARG で `cmp+je` (2 レジ返り値) | ゼロ |
| 例外パスのコスト | 連続 return | longjmp 1 回 + register restore |
| C コンパイラ最適化 | 全コード見える → DCE 可 | setjmp barrier で阻害される |
| portable | はい | はい |
| ASTro 特化と相性 | ◎ (部分木で state チェック消える) | △ |

baruby_precise / castro / abruby も同じ `RESULT { VALUE, state }` を採用。

### 5.6 c->state 廃止 (Phase 8d-final) — 完了

**目標**: `CTX::state` / `CTX::state_value` フィールドを完全削除し、 例外/
break/return/throw 等の非 NORMAL 制御フローを **すべて RESULT (2 reg) で
伝搬**。 baruby_precise / castro と同じ ABI に揃える。

**ステータス**: **2026-05-29 R5 まで完了**。 `c->state` / `c->state_value`
field を CTX 構造体から削除。 残った c->state 文字列参照 (15 件) は
すべてコメント (旧アーキ解説)。

#### 実装進捗 (2026-05-29 時点)

- [x] **R1** dispatch chain (korb_dispatch_call / korb_funcall /
  korb_funcall_with_block / korb_dispatch_binop / korb_dispatch_to_method /
  korb_dispatch_visibility_raise / prologue_*) を RESULT 化
- [x] **R2** korb_yield / korb_yield_slow を RESULT 化、 ~120 site の
  caller を UNWRAP/CHECK/SINK_RESULT に migrate
- [x] **R3** korb_node_X_slow (binop / aref / aset 等 17 種) を RESULT 化
- [x] **R4a** named legacy cfunc 67 件を cfunc_r ABI に変換
  (exception/binding/file/symbol/string + builtins.c の `_*_disallowed`)
- [x] **R4b** nested GCC `({...})` 形式 cfunc 25 件 + restrict 型 4 件を
  cfunc_r に変換、 builtins.c の copy-method ループは
  `m->u.cfunc.func_r` か `func` か実行時 dispatch に
- [x] **R5a-l** SINK_RESULT 系全件除去: propagation pattern 68 件 →
  UNWRAP/CHECK、 swallow pattern (respond_to?-style) 16 件 → RESULT
  capture + state 分岐、 helper RESULT 化 (korb_to_int_or_raise /
  ary_try_to_int / ary_predicate_match / korb_const_lookup /
  korb_cmp_call / rng_cmp / ary_sort_compare / hash_min_or_max_by /
  int_coerce_dispatch / flt_coerce_dispatch / kwsplat_convert /
  exc_to_s_internal / str_concat_one / ary_combine/perm/rcombine/rperm
  etc.)、 ARO_ROOT_SCOPE 内 yield swallow 4 件 → `_final` accumulator
  pattern。 **builtins/*.c と object.c の SINK_RESULT は 0 件達成**。
- [x] **R5m-o** node.def / koruby_runtime.c の c->state 経由 raise
  propagation を _br RESULT 直参照に変更、 ObjectSpace ABI mismatch を
  修正。 koruby_run_ast の THROW→RAISE / SystemExit / unhandled exception
  経路を RESULT-driven に。
- [x] **R5p** 残 c->state setter / lift 全件解消: korb_raise を pure
  RESULT 化、 prologue_cfunc_inl 削除、 builtins/array.c の swallow
  pattern を `_tr.state` 直参照に、 builtins/proc.c proc_call 全面
  RESULT-native 化 (break/return/throw/retry を `_br.state` で分岐)、
  kernel_raise/throw/catch/exit/abort を pure RESULT 化、
  korb_inspect_dispatch / to_s_dispatch / hash_value / eql の SINK
  撤去 (silent swallow に変更)、 korb_eval_string /
  korb_eval_string_in_self / korb_require_file / korb_load_file の RESULT 化、
  korb_fiber_resume / korb_fiber_yield の RESULT 化 + swapcontext 跨ぎは
  `struct korb_fiber.result_state / result_exc` 経由。
- [x] **R5q** `c->state` / `c->state_value` field 削除完了。 LIFT_C_STATE /
  SINK_RESULT / LIFT_C_STATE_OR_OK / CHECK_FROZEN_RET macro 全削除。
  legacy void cfunc bridge `prologue_cfunc_inl` 削除、 ABI は cfunc_r
  単一に統一。

`CTX::state_target_frame` は state 値ではなく "非ローカル return がどの
method frame を unwind するか" の target ポインタなので RESULT には載らず、
CTX に残す (これは state ではない)。

#### 現状の問題

422 ヶ所の legacy call site (`korb_funcall` / `korb_yield` / `korb_dispatch_call`
等) が `c->state` 経由で例外を返す。 node.def の bridge は
`LIFT_C_STATE(c, ...)` で 43 ヶ所 / `LIFT_C_STATE_OR_OK(c, ret)` で 3
ヶ所 RESULT に lift しているが、 これらは bridge であり「state を消す」
には不十分。

#### 削除戦略 (5 ステップ)

**R1**: `korb_dispatch_call` / `korb_funcall` / `korb_funcall_with_block` /
`korb_dispatch_binop` / `korb_dispatch_to_method` 群を **すべて RESULT
返り値に in-place 変更**。 関数 signature を `VALUE foo(...)` → `RESULT
foo(...)` に直す。 既存呼出を全 sweep で書き換え (UNWRAP / CHECK 適用)。

**R2**: `korb_yield` / `korb_yield_slow` を RESULT 返り値に変更。 今は
"VALUE 返し + c->state に BREAK/RETURN/NEXT/REDO を載せる" 設計を、
"RESULT.state を直接返す" に。 builtin の各 iterator (ary_each,
range_each, hash_each 等) で yield 結果を RESULT で受ける。

**R3**: `korb_node_X_slow` (約 20 個の binop fallback) を RESULT 返り値に。
これは object.c の cold path なので影響範囲は小さい。

**R4**: legacy cfunc (~100 個、 cfunc_r ABI 未移行) を全 cfunc_r 化。
`prologue_cfunc` (legacy) の path 自体を削除する。

**R5**: `korb_raise` / `korb_raise_X` を「c->state 設定しない、 RESULT
返し only」に書換え。 これにより `c->state` を書く場所がなくなる。

最後: `CTX::state` / `CTX::state_value` field を削除し、 `LIFT_C_STATE`
macro 群を削除。

#### Step ごとの commit 粒度

- R1 は内部で「`korb_dispatch_call_r_native(c, ...)` を一旦 _r 名で実装、
  両方の version を共存」させ、 caller を 1 ヶ所ずつ切替えて最後に旧名を
  削除する。 一度に全 caller を書き換えるのは git diff が膨大で review
  不能になるため。
- R2-R4 も同様。
- R5 は最後の 1 step (caller が全部 RESULT 化されていれば、 korb_raise
  から c->state 書きを消すのは 1 line 修正)。

#### 段階の検証基準

各 step 後に:
- `make test` 全 24 suite が default / STRESS / STRESS+PURGE 全 mode で PASS
- rubyspec sweep が PASS counts を維持 (regression 0)
- `grep -c "c->state\b" *.c` が単調減少

#### 影響を受けないもの

- `c->state_target_frame` — 非ローカル return の target frame 識別、 残す
- `c->current_eval_binding` 等の周辺 CTX field — 例外伝搬には無関係
- `korb_raise` の signature 自体 (既に RESULT を返す)、 内部実装のみ変更
- node.def の break/next/return 終端 (既に RESULT を直接返す)
- prologue_cfunc_r_inl (既に RESULT-native)

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

### 7.1 ivar_cache / method_cache の GC generation tag

inline ivar_cache および method_cache は `(klass *, slot)` の組を AST node
ごとに保持する。 moving GC 下で 「クラス A が address X から移動 →
別クラス B が X に配置」 が起きうるため、 ポインタ同一性だけだと
cache hit が誤判定して別 class の slot/method を返してしまう。

対策として `cache.gen` (= `korb_g_gc_gen` snapshot) を比較し、 GC が一度
でも fire した後の cache は強制 miss にする。 `korb_g_gc_gen` は
`koruby_visit_roots` 冒頭で bump する。

これがないと、 親クラスを共有する 2 サブクラスが順次 instance を作って
ivar を読み書きするとき、 STRESS 下で @ivar 値が消える (= nil 化) 症状が
起きる。 同じパターンは send_cache (= builtins/object.c の global table)
にもあるので、 そちらも gen check を追加してある。

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

### 12.3 c->sp の同期 — 「alloc 前に sync する」の原則

#### 基本ルール

alloc を起こす関数 (= 内部で GC trigger ありうる) は、 自身の中で
`c->sp = sp` を行う。 **caller (bridge / EVAL_node body / parent) は
c->sp に触らなくてよい**。

理由:
- caller が pre-bump しても、 callee が更に深く alloc すると整合性
  維持の責任が caller↔callee で曖昧になる
- 「alloc 直前にやる」 と統一すれば、 sweep の規約は一点に集約される
- helper が `(c, sp)` を受け取れば、 sync が静的に強制できる

#### 実装パターン

**新-ABI cfunc** (Phase 4 — 移行中):

```c
static RESULT sym_to_s(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;                              /* ← alloc 前 sync */
    VALUE s = korb_str_new_cstr(korb_id_name(korb_sym2id(sp[-1])));
    return RESULT_OK(s);
}
```

**alloc helper** (Phase 7 — 計画):

helper 自身が `(CTX *c, VALUE *sp, ...)` を受け取り、 入り口で sync:

```c
VALUE korb_str_new_cstr(CTX *c, VALUE *sp, const char *str) {
    c->sp = sp;                              /* helper 側で sync — caller 不要 */
    return korb_str_alloc(strlen(str), str);
}
```

これが普及すると cfunc 側の `c->sp = sp;` も冗長になる:

```c
static RESULT sym_to_s(CTX *c, int argc, VALUE *sp) {
    return RESULT_OK(korb_str_new_cstr(c, sp, korb_id_name(korb_sym2id(sp[-1]))));
}
```

#### dispatcher bridge

旧 EVAL_node から新-ABI cfunc を呼ぶ bridge (`prologue_cfunc` 内) は、
**c->sp を bump しない**。 cfunc が自分で sync する前提:

```c
VALUE *sp = c->sp;
sp[0] = recv;
for (uint32_t i = 0; i < argc; i++) sp[1 + i] = c->current_frame->fp[ai + i];
/* c->sp は触らない — cfunc が alloc 前に sync する */
RESULT _rr = prologue_cfunc_r_inl(c, callsite, argc, sp + 1 + argc, ...);
```

ただし sp[0..argc] への書込みは parent c->sp の **上** の slot に対する
書込みなので、 bridge 内では GC は起こさない (immediate copy のみ)。

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
| 4 | 全 ~680 cfunc を新 signature に sweep | 完了 (Phase 8b で全 builtins/*.c sweep 済) |
| 5 | node.def の call 系 node を sp staging に | 完了 (commit 61f8103e, 041db4d4) |
| 6 | AST method prologue を sp 経由 args に | 完了 (実質 sp staging — caller の fp[arg_index..] が callee の new_fp[0..] に zero-copy 受け渡し) |
| 7 | C API helper (korb_eq / korb_str_new_cstr / korb_ary_new 等) を (c, sp, ...) 規約に | 完了 (commit da232e4f で残 alloc helper 全て sp 引数化) |
| 8a | 移行 macro 整備 + warn_unused_result + DROP_RESULT wrapping | 完了 (469 ヶ所 wrap、 build green) |
| 8b | builtins cfunc を全て新 ABI に migrate | 完了 (DROP_RESULT 469 → ~110、 残は AST 内部 / void helper / 意図的 dispatch discharge) |
| 8c | korb_vm global を KORB_VM(c) macro 経由に切替 | 進行中 (node.def 全 43 ref 完了 commit 354837c7、 object.c 残 34 ref は CTX を取らない low-level helper で API 改修待ち) |
| 8d | node.def AST nodes を RESULT 化 (EVAL macro 変更) | R1-R5 完了 (cfunc 全 RESULT 化 + c->state 削除) |
| 8e | super_forward / super で cfunc_r 経路に対応 | 完了 (commit 832011d6) |
| 8f | AST dispatcher で argv snapshot して zero-fill clobber 回避 | 完了 (commit 832011d6) |
| 9 | 動作確認 + 回帰 fix | 進行中: 全 24 test suite が default + STRESS + STRESS+PURGE 全 mode PASS。 test_class.rb の post-body snapshot SEGV は fr.self 経由に変えて修正済。 c->sp を c->sp_top に rename し alloc 関数のみが書き換える design rule を明確化。 cfunc prologue cleanup は dispatcher API (korb_funcall/yield 等) の sp 持ち回りが前提なので一旦保留。 **ARO_ROOT_SCOPE_START を全廃 (初期 ~50 件 → 0、実コード参照ゼロ)。** alloc helper 呼出の sp 引数は「sp + N」明示形式 (user 指示)。 caller 側で redundant な `c->sp_top = sp + N` (= 直後の alloc helper が自前で行う) は削除 (user 指示)。 移行先 pattern: AST node body は `sp[0]=...; c->sp_top=sp+N; ...` で staging、 cfunc/runtime helper (korb_raise/korb_inspect_inner 等) は `VALUE *sp = c->sp_top; sp[0]=...; c->sp_top = sp + N` 形。 rubyspec は今 session で Struct 130→184、 Range 458→526、 Array/Hash 系で多数の PASS 改善。 |

Phase 7 完了後は cfunc 側の `c->sp = sp;` も不要になる (helper が
責任を引き受ける)。 移行期は cfunc 側で sync しておけば安全。

### 12.7 互換性 bridge

Phase 2-4 の移行期間中、 新 `cfunc_r` と旧 `cfunc` は両立する:
- `korb_method.u.cfunc.func_r` が non-NULL → 新 ABI
- `korb_method.u.cfunc.func` (旧 field) のみ → 旧 ABI
- `korb_dispatch_call_cached` と `korb_dispatch_to_method` 両方に bridge:
  新 ABI cfunc は dispatch 時に sp に self/args を stage して呼び、
  返り RESULT を c->state + VALUE に変換して upstream に返す。

すべて新 ABI に sweep 完了したら、 legacy field と bridge を撤廃する。

### 12.8 Subclass.new での initialize dispatch (2026-05-29)

CRuby semantics: `class S < String; def initialize(...); ...; end; end;
S.new(...)` で S#initialize が dispatch される必要がある。 我々の C
{ary,hash,str}_class_new が初期実装では allocate して直接結果を返していた
ため、 subclass の override が無視されていた。

修正後の流れ:
1. allocate empty obj of self's class (`korb_str_new(c, c->sp, "", 0)` 等)
2. retag `basic.klass = self` if subclass
3. **stage `sp[0] = obj, sp[1..argc] = argv[i]` on sp**
4. **bump `c->sp = sp + 1 + argc`** so the AST dispatcher's
   `[prev_sp, new_sp)` zero-fill on return doesn't clobber `sp[0]`
5. `korb_funcall_r(c, obj, :initialize, argc, sp + 1)` で dispatch
6. **`obj = sp[0]`** で re-read (GC が dispatch 中に obj を移動した可能性)
7. restore `c->sp = prev_sp` & return `obj`

ステップ 4 は caller 側の workaround。 本来は callee
(`korb_dispatch_to_method`) が argv ポインタから new_fp 位置を算定すべき
だが、 試みた centralized fix は他のコールパスを壊した。 caller-side
bump で当面安定。

### 12.9 Generic ivar 側 table (2026-05-29)

T_OBJECT 以外の heap 型 (T_STRING / T_ARRAY / T_HASH / T_RANGE / ...) は
`struct korb_xxx` に `ivars[]` field を持たない。 これを足すと size と
visit_roots の対応が大変なので、 `korb_vm->generic_ivars` という Hash の
side table を導入:

- outer hash: `obj pointer (as VALUE) → inner Hash`
- inner hash: `Symbol → VALUE`
- outer は `compare_by_identity = true` で pointer 一致判定
- libc-allocated 型は move しないため pointer key で安定
- `korb_ivar_set/get/_ic_slow` で T_OBJECT 以外は side table に fallback
- `koruby_visit_roots` に `&korb_vm->generic_ivars` を追加して GC tracking

これで `class S < String; def initialize; @x = 1; end; end; S.new.instance_variable_get(:@x)`
が動く。

### 12.10 AST dispatcher の argv-snapshot (2026-05-29)

`korb_dispatch_to_method` の AST 経路は `new_fp = c->sp + 1` から
`[c->sp, new_sp) を zero-fill する。 caller が argv を sp 上に staging
した場合 (cfunc_r ABI の標準) は argv が clobber される。

修正: AST 経路の入口で argv を `VALUE saved_argv[argc]` (VLA) に snapshot
してから zero-fill。 caller がどこに argv を置いていても安全。 同様の
snapshot を cfunc_r 経路にも適用 (consistency)。
