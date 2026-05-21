# ASTro GC 設計案

ASTro framework が GC についてどういうサポートを提供するか、 そしてそれを
`sample/baruby` に適用するときに使うインターフェースは何か、 をまとめたもの。
**`§3` に MVP 試作 (`sample/baruby_precise`) の実装と実測ベンチ結果あり**。

関連: `idea.md` §8.2 (未踏項目), `code_store_quirks.md` (AST NODE 不動制約の
出処), `sample/baruby/` (conservative libgc の比較対象), `sample/baruby_precise/`
(本案の MVP 試作)。

---

## 0. 概要

ASTro framework は「インタプリタを node.def 宣言から自動生成する」 という
姿勢を取っているので、 GC についても **言語側 (node.def + ランタイム
ヘルパ) が宣言する → ASTroGen が必要な C を吐く → backend が algorithm を
差し替える** という 3 層で揃える。

framework が提供するもの:

1. **ヒープの kind 分離** (`HEAP_IMMORTAL` / `HEAP_VALUE` 等) — §1.1
2. **allocation API** (`astro_gc_alloc` / `astro_gc_alloc_payload`) — §1.2
3. **共有 stack `sp[]` + `@locals` 属性** — `@locals(l)` 宣言で ASTroGen が
   `#define l sp[sp_cnt + 0]` を emit。 user は普通の C 変数名で root を扱える。
   GC は sp[] を flat scan するだけ — §1.3
4. **Write barrier macro** (`WB`) — §1.4
5. **Safepoint 配置** (default は保守側、 leaf は `@noalloc` opt-out) — §1.5
6. **Operand 注釈** (`@ref(value)` / `@imm` / `@weak`) — §1.6
7. **値の型宣言** (`VALUE_DEF` — 任意採用) — §1.7
8. **Backend の compile-time 切替** (`make GC=...`) — §1.8

backend (algorithm) は `make GC=none / conservative / marksweep / semispace /
generational / realtime / cruby` のいずれか 1 個を compile-time に選ぶ。
node.def の宣言は backend に対して中立で、 同じ node.def から複数 backend
を出せることが framework の設計目標。

**ASTro 特有の制約**:

- AST NODE は Code Store (`SD_<hash>.so`) がポインタを literal で焼き込むため
  **絶対に動かせない**。 どの backend を選んでも AST NODE は `HEAP_IMMORTAL`
  に強制配置する
- node.def の BODY テキストは触らない。 GC を入れるときも BODY を侵襲しない
  手段で root / WB / safepoint を仕込む

---

## 1. ASTro が提供する GC サポート

### 1.1 ヒープの kind 分離

backend 非依存に「このオブジェクトをどう扱うか」 を 1 軸で切る:

```c
typedef enum {
    HEAP_NONE,             // GC を使わないサンプル用
    HEAP_VALUE,            // 通常オブジェクト (backend が moving なら動く)
    HEAP_REF_PAYLOAD,      // 二層オブジェクトの ref 配列
    HEAP_ATOMIC_PAYLOAD,   // bytes、 deep mark なし
    HEAP_IMMORTAL,         // 不動・永続 (AST NODE はここ)
    HEAP_LARGE,            // 単独 mmap (move しない)
    HEAP_FINALIZABLE,      // finalizer 持ち
} astro_heap_kind_t;

astro_heap_t astro_gc_heap(astro_heap_kind_t k);
```

ヒープを分けることで:

- AST NODE を `HEAP_IMMORTAL` に強制 → Code Store の literal 焼込みと両立
- ref 配列を `HEAP_REF_PAYLOAD` に分離 → moving backend で一括 move、 キャッシュ
  局所性が立つ
- `HEAP_ATOMIC_PAYLOAD` の marker は deep scan しない (高速)
- `HEAP_FINALIZABLE` だけ別キュー処理

### 1.2 アロケーション API

```c
// kind は言語側の type id、 size は payload 込み。
// LSB tagging は言語の魂なので戻り値はタグなし。
void *astro_gc_alloc(astro_heap_t h, uint32_t kind, size_t size);

// 二層オブジェクトの payload。 attr で atomic / movable を指定
void *astro_gc_alloc_payload(astro_heap_t h, size_t size,
                             astro_payload_attr_t a);

// realloc — 内部で safepoint poll を含む (§1.5 参照)
void *astro_gc_realloc_payload(astro_heap_t h, void *p, size_t new_size);
```

戻り値は **タグなし**。 言語ごとの tag scheme (LSB tag / NaN-box / Flonum tag 等)
が違うので、 framework が tag を被せると言語側と衝突する。 サンプル側で wrap
する。

### 1.3 Root 列挙: 共有 stack `sp[]` + `@locals` 属性

precise GC を動かすには「いま live な VALUE root が **既知の連続領域** に
ある」 状態を作る必要がある。 ASTro は **Lua VM と同じ stack-based** モデル
を採る:

- 各 NODE_DEF は `VALUE *sp` と `int sp_cnt` を共通引数で受ける
- root にしたい VALUE は **`@locals(name1, name2, ...)` 属性** で宣言
- ASTroGen は BODY 直前に `#define name sp[sp_cnt + IDX]` を emit するだけ
- 子 evaluation には `sp_cnt + 自分の locals 数` を渡す
- GC は `sp[0..c->sp_cnt]` の VALUE 配列を flat scan するだけ
- **per-node の chain push/pop は無し**

Lua の `lua_pushvalue` / `lua_tovalue` を NODE_DEF 内で書く代わりに、 `@locals`
を ergonomic な sugar として与え、 user は普通の C 変数名で読み書きできる。

#### 1.3.1 まず一番素朴な NODE_DEF を見る

```c
NODE_DEF
node_add(CTX *c, NODE *n, NODE *lv, NODE *rv)
{
    VALUE l = EVAL_ARG(c, lv);
    VALUE r = EVAL_ARG(c, rv);
    return l + r;
}
```

precise GC では `l` が `EVAL_ARG(c, rv)` の allocation を生存する必要があるが、
ASTroGen には「`l` が VALUE root」 だと知る手段がない。 普通の C コンパイラ
から見れば `l` はただのローカル変数で、 GC ヒープのポインタかどうか区別が
つかない。

#### 1.3.2 `@locals(l)` で宣言 → ASTroGen が `#define` を emit

`l` を共有 stack 上の slot として宣言する:

```c
NODE_DEF @locals(l)
node_add(CTX *c, NODE *n, VALUE *sp, int sp_cnt, NODE *lv, NODE *rv)
{
    /* ASTroGen emits at BODY 直前: #define l sp[sp_cnt + 0] */
    l = EVAL_ARG(c, lv, sp_cnt + 1);   // 子は sp_cnt + 1 から
    VALUE r = EVAL_ARG(c, rv, sp_cnt + 1);
    return l + r;
    /* ASTroGen emits at BODY 直後: #undef l */
}
```

ポイント:

- `l` は **`sp[sp_cnt + 0]`** の別名 (= memory 上の slot)。 alloc しても残る
- user は `l = ...` / `... l ...` と **普通の C 変数のように** 書ける。
  `sp[0]` のような index 直書きは不要
- `VALUE l = ...` のような宣言は書かない (`@locals` が宣言済み扱い)
- 子 evaluation には `sp_cnt + 1` を渡す (= `@locals` の slot 数だけ進める)
- `r` は最後の `+` でしか使わない (= 子 call をまたがない) → C local OK、
  `@locals` には含めない

ASTroGen の責務は **`#define`/`#undef` を BODY の前後に emit するだけ**。
BODY のテキスト解析・書き換えは不要。

#### 1.3.3 SD specialize 時の slot 衝突は runtime で解決される

`node_add` が SD で 2 重に inline されたとき、 内側の `node_add` の `l` と
外側の `node_add` の `l` は別 slot でなければならない。 これは **`sp_cnt` が
runtime parameter** であることで自然に解ける:

- 外側 `node_add` 呼出: 呼び出し元から `sp_cnt = 0` で渡る → `l` は `sp[0]`
- 外側が `EVAL_ARG(c, lv, sp_cnt + 1)` で内側を呼ぶ → 内側の `sp_cnt = 1`
- 内側 `node_add` 内では `l` = `sp[1 + 0]` = `sp[1]`

`#define l sp[sp_cnt + 0]` の `0` は per-NODE_DEF の定数なので **build-time に
固定で OK**。 inline 位置による slot 衝突は runtime の sp_cnt 値で自動的に
解ける。 ASTroGen が inline_idx を採番する必要なし。

#### 1.3.4 GC は sp[] を flat scan するだけ

GC 起動時:

```c
void <lang>_gc_iter_roots(astro_root_visitor_t *v) {
    for (int i = 0; i < c->sp_cnt; i++) {
        astro_visit_value(v, c->sp[i]);
    }
    // global root (関数テーブル等) は v に直接渡す
}
```

per-node の frame chain は無い。 hot path 中は何もしない。

#### 1.3.5 `c->sp_cnt` の update タイミング

GC は allocation 経由でしか起きないので、 **allocation site の直前で
`c->sp_cnt` を update** すれば足りる。 framework alloc API が内部で行う:

```c
void *
astro_gc_alloc(astro_heap_t h, uint32_t kind, size_t size, int sp_cnt_top)
{
    CTX *c = astro_gc_current_ctx();
    c->sp_cnt = sp_cnt_top;                       // 1 store
    if (UNLIKELY(astro_gc_pending)) astro_gc_handshake();
    return /* freelist bump */;
}
```

NODE_DEF から alloc を呼ぶ場合は `sp_cnt + 自分の locals 数` を渡す:

```c
NODE_DEF @locals(a)
node_str_lit(CTX *c, NODE *n, VALUE *sp, int sp_cnt, const char *bytes, uint32_t len)
{
    a = astro_gc_alloc(astro_gc_heap(HEAP_VALUE), OBJ_STRING,
                       sizeof(BaString), sp_cnt + 1);
    ...
    return a;
}
```

CTX を取らない C helper (`baruby_ary_push` 等) も `int sp_cnt_top` 引数を 1 つ
増やして framework に通す。

**hot path 中で `c->sp_cnt` を毎 node update する必要はない**。 alloc しない
限り GC が起きないため。

#### 1.3.6 コスト分析

| 設計 | 1 node 評価あたりの hot path コスト |
|---|---|
| per-NODE_DEF frame chain (没案) | ~5 mem ops (chain push + pop) |
| **`@locals` + sp_cnt (本案)** | **`@locals` 1 個につき 1 store + 1 load (read 時) / alloc 1 回につき c->sp_cnt 更新 1 store** |

- 純 fixnum tight loop で `@locals` が空: **完全ゼロコスト** (C local のまま、
  register に居る)
- `@locals(l)` 1 個: store/read で 1 store + 1 load。 modern OoO は L1 hit +
  write coalescing + load-store forwarding で多くを並列実行に隠せる
- `sp[sp_cnt + IDX]` の indexed access は `mov [reg+reg*8], reg` 1 命令、
  sp_cnt は register に居る前提

実測値は §3 (baruby_precise を題材にした testbed) を参照。

#### 1.3.7 callee frame など可変長 root

baruby の `VALUE F[locals_cnt]` (callee の新 frame) のような可変長 root は
sp の延長で表せる:

```c
NODE_DEF
node_call_1(CTX *c, NODE *n, VALUE *sp, int sp_cnt,
            /*...*/, uint32_t locals_cnt, NODE *a0)
{
    /* sp[sp_cnt..sp_cnt + locals_cnt - 1] が callee の locals 領域 */
    sp[sp_cnt + 0] = EVAL_ARG(c, a0, sp_cnt + locals_cnt);   // arg eval は上で
    return EVAL(c, cc->body, sp, sp_cnt + locals_cnt);       // callee は base が変わる
}
```

callee 側の NODE_DEF の `@locals` slot は callee の `sp_cnt + locals_cnt` 以降
を占めるので、 callee の locals (= sp[sp_cnt..sp_cnt + locals_cnt - 1]) と衝突
しない。 GC scan は `sp[0..c->sp_cnt]` の flat scan なので callee frame も自動
的に含まれる。

#### 1.3.8 cooperative である理由

c->sp_cnt の更新は alloc site のみで OK というのは、 GC が **cooperative**
(= alloc 経由でしか起きない) だからこそ成立する。 signal-based preemptive GC
だと c->sp_cnt が毎 node 正確に最新でないと壊れる。 cooperative の選択は
ゼロコスト性の前提条件。

### 1.4 Write barrier

```c
// holder = 書き込まれる側のヘッダ、 val = 新値
#define WB(holder, field, val) do {                                  \
    astro_gc_pre_wb((holder), (void **)&(holder)->field);            \
    (holder)->field = (val);                                         \
    astro_gc_post_wb((holder), (void *)(val));                       \
} while (0)
```

backend ごとに pre/post の中身が違う:

| Backend | `pre_wb` | `post_wb` |
|---|---|---|
| non-moving M&S | no-op | no-op |
| semi-space | no-op | no-op (STW) |
| generational | no-op | card mark / remset |
| realtime (SATB) | 旧値を mark queue | no-op |
| realtime (incremental update) | no-op | 新値が white なら shade |

`pre_wb` / `post_wb` は inline → no-op backend では C コンパイラが完全に消す。
BODY からは `WB(obj, field, val)` macro 経由でしか書き込まない、 を原則にする。

### 1.5 Safepoint 配置

§1.3.4 で示した通り、 GC は cooperative で **alloc site でのみ poll** する。
明示的な safepoint macro は alloc を含まない長 loop の back-edge 用:

```c
#define ASTRO_SAFEPOINT(c, sp_top) do {       \
    if (UNLIKELY(astro_gc_pending)) {         \
        (c)->sp = (sp_top);                   \
        astro_gc_handshake();                 \
    }                                         \
} while (0)
```

挿入箇所:

- allocation site の直前 → framework alloc API が **内部で自動 poll** (§1.3.4)
- allocation を含まない長 loop back-edge → BODY 内で `ASTRO_SAFEPOINT(c, sp+N)`
  を明示

NODE_DEF レベルの注釈:

| オプション | 意味 |
|---|---|
| `@noalloc` | BODY + 全 transitive children が allocate しないことを保証。 leaf node (literal / 変数参照系) で使う。 ASTroGen は spill / c->sp 更新を一切省略 |
| `@safepoint` | BODY 末尾に safepoint poll を強制挿入。 allocation を含まない長 loop back-edge で使う |

**default の挙動**: `EVAL_ARG` で子を評価する非 leaf ノードは transitive に
allocate しうるため、 default は「allocate しうる」 保守側に倒す。 opt-in
注釈 `@allocates` は持たない。 leaf の opt-out として `@noalloc` だけを残す。

**CTX を取らない C helper 内での safepoint**: `baruby_ary_push(VALUE av, VALUE
x, VALUE *sp_top)` のように CTX を取らない C helper も、 framework が提供
する alloc / realloc API (`astro_gc_alloc`, `astro_gc_realloc_payload`) を
経由する限り **API 内部で poll される**。 helper は `sp_top` 引数を 1 つ
増やしてそれを framework に通すだけ。 現在 CTX は TLS 経由
(`astro_gc_current_ctx()`) で取得 (single-thread 前提では グローバル 1 個)。

### 1.6 Operand 注釈の拡張

既存の `<type> <name>@ref` (struct 内 inline 格納 + hash skip) に GC 関連の
サブ注釈を足す:

| 注釈 | 意味 |
|---|---|
| `@ref` | 既存。 inline 格納 + hash skip。 **GC は touch しない** (mutable な metadata 用) |
| `@ref(value)` | `@ref` かつ中身が `VALUE` (or VALUE 配列)。 marker は mark する |
| `@imm` | この operand は `HEAP_IMMORTAL` 上の永続オブジェクトを指す。 marker は touch せず recurse のみ。 default は AST NODE \* |
| `@weak` | 弱参照 |
| `@atomic` | bytes 列、 deep mark 不要 |

operand の意味論を declarative に書くことで、 marker は自動生成できる。

例: `node_call_2(... struct callcache *cc@ref ...)` の `callcache` の中身が
`{state_serial_t serial; struct Node *body;}` であれば、 `serial` (atomic) と
`body` (AST NODE = immortal) だけなので GC touch 不要 → default の `@ref` で OK。
中身に VALUE が混じれば `@ref(value)` に格上げ。

### 1.7 値の型宣言 (`VALUE_DEF` — 任意採用)

各言語の値のメモリレイアウト (共通ヘッダ + kind 別フィールド) を宣言する DSL。
`node.def` の中に書く。 **採用は任意** — 採用すれば marker / setter /
allocator が ASTroGen から自動生成される。 採用しなければ言語側で marker を
手書きする (`abruby/node_mark.c` のスタイル)。

```
VALUE_DEF baruby_obj @header=ObjectHeader @kind_field=type @prefix=BA
{
    OBJ_ARRAY   => ref_payload(VALUE) items; uint32_t len; uint32_t capa;
    OBJ_STRING  => atomic_payload(char) bytes; uint32_t len; uint32_t capa;
}
```

ASTroGen が `@prefix=BA` の指定から、 言語固有 prefix 付きの macro を生成する:

- `BA_ALLOC_<KIND>(...)` — kind 別 type-specialized allocator
- `BA_MARK_<KIND>(obj)` — precise marker
- `BA_FORWARD_<KIND>(obj)` — moving backend 用 forward fixup
- `BA_SET_<field>(obj, val)` — write barrier 込み setter

field 注釈:

| 注釈 | 意味 |
|---|---|
| `ref(T)` | trace 対象、 setter に write barrier を自動挿入 |
| `ref_payload(T)` | 二層オブジェクトの可変長 ref 配列。 別ヒープ、 一括 move 可 |
| `atomic_payload(T)` | bytes。 deep mark 不要、 moving 可 |
| `immortal` | 不動 (AST NODE 等) |
| `finalizer F` | 解放時 F を呼ぶ |
| `ref_weak(T)` | 弱参照 |

### 1.8 Backend の compile-time 切替

```
make GC=none           # 全 hook が (void)0 / identity — ゼロコストの gate
make GC=conservative   # libgc 保守的スキャンを framework 経由で wrapping
make GC=marksweep      # precise non-moving M&S
make GC=semispace      # copying / moving
make GC=generational   # young/old + remset
make GC=realtime       # SATB or incremental update
make GC=cruby          # CRuby host VM 委譲
```

`astro_gc.h` 内の `inline` で全 hook を確定し、 no-op はコンパイラが消す。
runtime dispatch は持たない。 切替は Makefile の 1 行のみで、 node.def は変更
不要。

---

## 2. baruby への適用

### 2.1 baruby の値表現

`sample/baruby/context.h` で:

```c
typedef intptr_t VALUE;
// LSB == 1                 → fixnum (signed int63)
// raw == 0 / 2 / 4          → false / true / nil
// LSB == 0 (それ以外)       → heap object pointer (8-byte aligned)

typedef struct ObjectHeader { uint32_t type; uint32_t flags; } ObjectHeader;
typedef struct BaArray  { ObjectHeader hdr; uint32_t len, capa; VALUE *items; } BaArray;
typedef struct BaString { ObjectHeader hdr; uint32_t len, capa; char   *bytes; } BaString;
```

frame: `VALUE *fp` を `common_param_count=3` で全 NODE_DEF に第 3 引数として
渡す。 各 frame は parser が決めた連続 VALUE 配列で、 `node_scope` の
`fp + envsize` で base を進める。

### 2.2 値の型宣言 (採用するなら §1.7)

`node.def` 冒頭に追加:

```
VALUE_DEF baruby_obj @header=ObjectHeader @kind_field=type @prefix=BA
{
    OBJ_ARRAY   => ref_payload(VALUE) items; uint32_t len; uint32_t capa;
    OBJ_STRING  => atomic_payload(char) bytes; uint32_t len; uint32_t capa;
}
```

ASTroGen は `BA_ALLOC_OBJ_ARRAY` / `BA_ALLOC_OBJ_STRING` / `BA_MARK_*` /
`BA_FORWARD_*` / `BA_SET_*` を生成。 採用しない場合は marker を `baruby_mark.c`
等に手書きする。

### 2.3 NODE_DEF を `@locals` + sp_cnt スタイルに書き換える

baruby は `common_param_count=3` で `VALUE *fp` を渡しているが、 これを
`VALUE *sp, int sp_cnt` の 2 引数に置き換える (common_param_count=4)。 root が
必要な VALUE は `@locals(...)` で宣言する。

`node_call_aset` の典型 (`recv` と `val` が `baruby_ary_push` をまたぐ):

```c
NODE_DEF @locals(r, v)
node_call_aset(CTX *c, NODE *n, VALUE *sp, int sp_cnt,
               NODE *recv, NODE *idx, NODE *val)
{
    /* ASTroGen emits: #define r sp[sp_cnt + 0]; #define v sp[sp_cnt + 1] */
    r = UNWRAP(EVAL_ARG(c, recv, sp_cnt + 2));   // 子は sp_cnt + 2 から
    VALUE i = UNWRAP(EVAL_ARG(c, idx, sp_cnt + 2));   // intptr に変換して死ぬ → C local
    v = UNWRAP(EVAL_ARG(c, val, sp_cnt + 2));
    if (IS_ARY(r)) {
        intptr_t ii = VAL2INT(i);
        ...
        while ((uint32_t)ii >= VAL2ARY(r)->len) {
            baruby_ary_push(r, VAL_NIL, sp_cnt + 2);
        }
        BaArray *a = VAL2ARY(r);   // 再 fetch (moving backend 対応)
        WB(a, items[ii], v);
        return RESULT_OK(v);
    }
    ...
}
```

`baruby_ary_push` は `int sp_cnt_top` 引数を追加で受け、 内部の
`astro_gc_realloc_payload` に通す。 `c->sp_cnt` の更新は framework alloc API の
中で行われるので NODE_DEF 側からは見えない。

leaf 系は `@locals` を付けず素朴な BODY のまま:

```c
NODE_DEF
node_num (CTX *c, NODE *n, VALUE *sp, int sp_cnt, int32_t num)
{ return RESULT_OK(INT2VAL(num)); }

NODE_DEF
node_lget(CTX *c, NODE *n, VALUE *sp, int sp_cnt, uint32_t index)
{ return RESULT_OK(/* function frame の locals 領域から index 番目 */); }

NODE_DEF
node_true(CTX *c, NODE *n, VALUE *sp, int sp_cnt)
{ return RESULT_OK(VAL_TRUE); }
```

node_lget のように function locals を読む node は別系統 (parser が決定する
offset)。 `@locals` は **NODE_DEF scope の scratch root** を宣言する場で、
function scope の lvar とは別。

### 2.4 関数境界: callee frame は sp の上に確保

baruby は現状 `VALUE F[locals_cnt]` で C スタック VLA に callee frame を
取っているが、 sp[] モデルでは **sp_cnt の上に locals_cnt 個の slot を確保**:

```c
NODE_DEF
node_call_1(CTX *c, NODE *n, VALUE *sp, int sp_cnt,
            const char *name, uint32_t arg_index, uint32_t locals_cnt,
            struct callcache *cc@ref, NODE *a0)
{
    /* sp[sp_cnt..sp_cnt + locals_cnt - 1] が callee の locals 領域 */
    sp[sp_cnt + 0] = UNWRAP(EVAL_ARG(c, a0, sp_cnt + locals_cnt));
    return EVAL(c, cc->body, sp, sp_cnt + locals_cnt);   // callee の sp_cnt を進める
}
```

callee は自分の `sp_cnt` を `(親 sp_cnt) + locals_cnt` として受け取り、 自分の
`@locals` slot を `sp[sp_cnt + IDX]` で取ることで親の locals 領域と衝突しない。

C VLA を捨てる利点: GC が `sp[0..c->sp_cnt]` を flat scan するだけで callee
frame まで自動的に含まれる。

トップレベルの起点だけは `main.c` で:

```c
c->sp      = c->env;       // VALUE stack の先頭
c->sp_cnt  = toplevel_locals_cnt;  // top-level frame 分を確保
RESULT r = EVAL(c, ast, c->sp, c->sp_cnt);
```

### 2.5 既存 `@ref` operand

baruby の `struct callcache *cc@ref` は中身が `{state_serial_t serial;
struct Node *body;}` で、 `body` は AST NODE = `HEAP_IMMORTAL`、 `serial` は
atomic。 GC が touch する必要なし → **default の `@ref` のまま**。

将来 inline cache に VALUE を追加するなら `@ref(value)` に格上げ。

### 2.6 node.c の allocator を framework alloc 経由に

```c
VALUE
baruby_ary_new(uint32_t capa)
{
    // 採用なし版: framework alloc を直接呼ぶ
    BaArray *a = astro_gc_alloc(astro_gc_heap(HEAP_VALUE), OBJ_ARRAY,
                                sizeof(BaArray));
    a->items = capa ? astro_gc_alloc_payload(astro_gc_heap(HEAP_REF_PAYLOAD),
                                              sizeof(VALUE) * capa,
                                              ASTRO_PAYLOAD_REF) : NULL;
    a->hdr.flags = 0;
    a->len = 0;
    a->capa = capa;
    return (VALUE)a;
}

// VALUE_DEF 採用版なら:
//   BaArray *a = BA_ALLOC_OBJ_ARRAY(capa);   // header + payload 一括
//   ...
```

`baruby_ary_push` 内の realloc:

```c
void
baruby_ary_push(VALUE av, VALUE x, VALUE *sp_top)
{
    BaArray *a = VAL2ARY(av);
    if (a->len == a->capa) {
        uint32_t new_capa = a->capa ? a->capa * 2 : 4;
        // astro_gc_realloc_payload は内部で c->sp = sp_top + poll を行う (§1.3.4)
        a->items = astro_gc_realloc_payload(astro_gc_heap(HEAP_REF_PAYLOAD),
                                            a->items,
                                            sizeof(VALUE) * new_capa,
                                            sp_top);
        a->capa = new_capa;
    }
    WB(a, items[a->len], x);
    a->len++;
}
```

`baruby_ary_push` の signature は `sp_top` を 1 つ追加するだけ (CTX は取らない
まま)。 framework alloc API が `c->sp = sp_top` を書いてから GC handshake する。

### 2.7 frame iterator (言語側 1 関数)

```c
void
baruby_gc_iter_roots(astro_root_visitor_t *v)
{
    // sp[] flat scan: fp_base から現在 top まで
    for (VALUE *p = global_c->fp_base; p < global_c->sp; p++) {
        astro_visit_value(v, *p);
    }
    // global root: 関数テーブル (本体は AST = immortal、 走査不要)
    //              code repo / 定数等は別 API で登録済
}
```

per-NODE_DEF chain を辿る必要なし。 `c->fp_base..c->sp` を flat scan するだけ。
これが sp[] モデルの一番の利点。

### 2.8 Makefile

```
GC ?= conservative

OBJS = main.o node.o
ifeq ($(GC),conservative)
  OBJS += $(ASTRO_RUNTIME)/gc_conservative.o
  LIBS += -lgc
endif
ifeq ($(GC),marksweep)
  OBJS += $(ASTRO_RUNTIME)/gc_marksweep.o
endif
# ...

baruby: $(OBJS)
	$(CC) -DASTRO_GC_$(shell echo $(GC) | tr a-z A-Z) -o $@ $(OBJS) $(LIBS)
```

`GC=conservative` (現状互換、 libgc を framework 経由で wrap) と `GC=marksweep`
(precise non-moving) を **同じ node.def から** ビルドできる、 を最初の milestone
にする。

### 2.9 全体まとめ: baruby に追加するもの

| 場所 | 追加内容 | 規模 |
|---|---|---|
| `node.def` 冒頭 | `VALUE_DEF baruby_obj` (採用するなら) | ~5 行 |
| 共通引数 | 第 3 引数を `VALUE *fp` → `VALUE *sp` に rename / 意味変更 | baruby_gen.rb 1 行 |
| 各 `NODE_DEF` BODY | root にしたい VALUE を C local → `sp[i]` に書き換え | ~20 箇所 |
| 各 EVAL_ARG | 第 3 引数 `sp_top` を渡す | ~30 箇所 |
| `node.c` | `baruby_ary_new` 等を framework alloc API 経由に | ~6 関数 |
| `node.c` | CTX 取らない C helper に `sp_top` 引数を追加 | ~4 関数 |
| `node.c` | 素の `a->items[ii] = v` を `WB(a, items[ii], v)` に | ~10 箇所 |
| `node.c` | `baruby_gc_iter_roots` を 1 個実装 (sp[] flat scan) | 1 関数 |
| `main.c` | `c->fp_base = c->sp = c->env` の初期化 | 2 行 |
| `Makefile` | `GC=...` で backend ソースを切替 | 5-10 行 |

**注釈ベースの自動化はしない**。 root の置き方は全部 user が BODY で書く。
ASTroGen 側は sp を引数で通すだけ。 「裏で何が起きてるか分かりにくい」 を
避けるトレードオフで、 user の書く量は増える。

---

## 3. 試作: `sample/baruby_precise`

§2 の案を `astrogen.rb` 不変・ベタ書きで実装した testbed。
`sample/baruby_precise/` に `sample/baruby/` (conservative libgc) を copy
してから書き換えてある。 比較用に conservative 版もそのまま残る。
初期は precise mark&sweep で書いたが、 **moving GC への切替えを試す
場として現在は semi-space (Cheney) 実装**。

### 3.1 実装した範囲

- 共通引数: `common_param_count=4` で `(CTX *c, NODE *n, VALUE *fp, VALUE *sp)`
  を全 NODE_DEF に通す
- `BARUBY_EVAL_ARG(c, n, sp_new)` macro: child eval に新しい sp を渡す
- NODE_DEF (sed で機械的に sig 拡張、 BODY は手書き):
  - `node_call_<N>` / `node_pg_call_<N>`: C 上の VLA `VALUE F[locals_cnt]` を
    廃止、 callee frame を `sp[0..locals_cnt-1]` (= 共有 VALUE stack 上) に配置
  - heap-VALUE を持ち越す node (`node_eq` / `_add` / `_ary_push` 等) は
    `VALUE l = EVAL_ARG(...)` ではなく `sp[0] = BARUBY_EVAL_ARG(..., sp+N)`
    で root spill する
  - 各 allocator helper (`baruby_ary_new`, `baruby_str_new` 等) に `sp_top`
    引数を追加、 内部の `baruby_gc_alloc` に通す
  - 内部で alloc する helper (`baruby_ary_plus`, `_repeat`, `_push`,
    `baruby_str_concat`, `_repeat`, `_append`) は VALUE を値で受け取らず
    **`VALUE *ref`** で受ける。 alloc 後に `*ref` 再 deref で post-GC
    アドレスに更新
- `gc.c` / `gc.h` に semi-space (Cheney) copying GC:
  - `mmap` で 512 MiB region を 2 本確保 (`PROT_READ|PROT_WRITE`、 lazy
    page allocation で物理メモリは触ったぶんだけ)
  - `GCHeader { kind, size, fwd }` を payload 直前に置く。 forwarding
    pointer は `fwd` フィールド
  - 通常モードは 2 region を交互に swap。
    **`BARUBY_GC_STRESS=1`** で stress mode: 毎 alloc で GC、 旧 from-space
    は `mprotect(PROT_NONE)` + `madvise(MADV_DONTNEED)` で恒久 retire
    (仮想空間は使い捨て)、 新 to-space は毎回 `mmap` 取り直し。 stale ptr
    deref が即 SIGSEGV になる
- `astrogen.rb` は **無修正**。 `@locals` / `@scratch` などの sugar は使わない。
  user が手で `sp[0..N-1]` に root を置き、 `BARUBY_EVAL_ARG(c, n, sp+N)` で
  sp を進める

### 3.2 ASTRO_ASSERT / ASTRO_DEBUG

framework 共通の compile-time gated assertion macro を `runtime/astro_debug.h`
に新設:

```c
#if ASTRO_DEBUG
#  include <assert.h>
#  define ASTRO_ASSERT(expr) assert(expr)
#else
#  define ASTRO_ASSERT(expr) ((void)0)
#endif
```

baruby_precise では `ASTRO_DEBUG=1` がデフォルト (`context.h`)。
`make ASTRO_DEBUG=0` で release-shape build に切替え可能。 gc.c の
内部不変条件 (alloc 時 kind validity、 stress mode の PRE-MARK 範囲
check、 forward 時の from/to space 範囲 check) は全て `ASTRO_ASSERT`
経由で書かれている。

### 3.3 Moving GC で必須の二大パターン

mark&sweep 時代には潜伏していたバグが semi-space 化で一斉に表面化。
NODE_DEF / C helper を書くときの **絶対ルール** が二つ確立した:

**(A) sp[] spill** — heap VALUE を子ノード eval を跨いで保持する場合、
C local ではなく sp[] slot に置く (子の eval で GC が走ると sp[] は
in-place forward されるが、 C local は更新されない)

**(B) helper は VALUE* で受ける** — 内部で alloc する helper は VALUE を
値で受け取らず caller の sp[] slot への pointer で受ける (alloc 後に
`*ref` を再 deref して post-GC アドレスを取り直す)

詳細とコード例は [sample/baruby_precise/docs/runtime.md §5.7](../sample/baruby_precise/docs/runtime.md)。

### 3.4 動作確認

```sh
$ make
$ ./baruby_precise --plain test.ba.rb           # fib(20) — fixnum only
10946
Result: 10946, node_cnt:22

$ BARUBY_GC_STRESS=1 ./baruby_precise --plain test_eq.ba.rb   # 毎 alloc で GC
true
false
... (Array/String の `==`, `!=`, concat 等を網羅)
Result: true, node_cnt:213
```

stress mode を通したことが moving GC 正しさの強い証拠になっている
(rooting 漏れ・ helper 内 stale C local が即 SEGV するモードを通せた)。

### 3.5 ベンチ結果 (実測、 plain mode、 3 run 中央値)

| Bench | conservative (libgc) | precise (semi-space) | precise vs cons. |
|---|---:|---:|---|
| `binary_trees` | 0.907 s | **0.544 s** | **0.60×** ⬇40% (precise が速い) |
| `list_alloc` | 1.085 s | 1.152 s | 1.06× ⬆6% |
| `string_concat` | 0.968 s | 1.160 s | 1.20× ⬆20% |
| `fib_pair` | 1.127 s | 1.271 s | 1.13× ⬆13% |
| `substr_churn` | 1.361 s | 1.594 s | 1.17× ⬆17% |
| `gc_combined` | 1.079 s | 1.231 s | 1.14× ⬆14% |

**観察**:

- **binary_trees は precise の方が 40% 速い** — libgc の conservative scan
  (stack + data segment 全走査) が小オブジェクト大量生成シナリオで効く
- 他は +6〜20% で precise が遅い (sp[] spill / zero-init / sp register
  pressure / copy collector の copy コスト の合計)
- 全体 geomean ~ +7%。 §1.3.6 のコストモデル「spill 1 store/root + alloc 時に
  c->sp 更新 1 store」 がほぼ実測で観察された

`baruby_str_concat` を ref pattern に書き直したことで string_concat は
1.468 s → 1.160 s (-21%) と短縮。 旧版は malloc/memcpy/free で source bytes
をバッファコピーしていた回避コードが入っていた (helper が VALUE を値で
受けていたため)。

### 3.6 既知の問題

- **AOT mode は moving GC 移行後に未検証** — SD bake 経路で precise rooting
  が成立するかを再 audit する必要あり
- **toplevel sp の hardcode 64** (`main.c::create_context`)。 大きい
  toplevel フレームを持つプログラムでは scratch 領域が不足する
- **REGION_BYTES = 512 MiB が固定** — live set がこれを超えると OOM

### 3.7 次の段階で試したいこと

- AOT mode の再検証
- toplevel locals_cnt を parser から取って main.c で正しい sp を設定
- `astrogen.rb` 拡張で `@locals` を機械化 (手書きの sp[] spill 漏れを
  根本的に防ぐ — 過去の rooting バグ群は全部これで消えた)
- 世代別 GC backend を同じ interface に乗せる (`gc_combined` ベンチで
  効くはず)

---

## 未決事項

- `VALUE_DEF` 採否 (§1.7) — 採用すれば marker 自動生成、 不採用なら手書き。
  まず §1.3-1.6 だけで baruby を回し、 marker 手書きの工数が見えてから判断
- 将来的に注釈ベースの自動 spill (live-range 解析) を入れるかは保留。
  まず手書き sp[] で書いて、 同じパターンが多数のサンプルで繰り返されるなら
  ASTroGen に持ち上げる判断
- `HEAP_FINALIZABLE` の解放順序 — BDW 風 topological を default、
  `@finalizer F @order=N` で override 可、 を仮置き
- moving backend での pointer reload (`a = VAL2ARY(sp[0])` を allocate 後に
  挿入) は user が手で書く。 ASTroGen の自動化は保留
- multi-thread での TLS 切替 — single-thread が動いてから検討

---

## 4. Framework 化のための抽象化方針 (2026-05-21 議論)

`sample/baruby_precise/gc.{h,c}` + 16 backend を `runtime/precise_gc/` に
切り出して他 sample (koruby / pystro / asml / 等) でも使えるようにする際
の抽象化方針を整理。 設計の参考は MMTk の VMBinding (= callback semantics
+ compile-time monomorphization)。 C では **macro / `static inline` で
compile-time inline** を実現するのが等価。

### 4.1 ファイル構成

```
runtime/precise_gc/
  gc.h             # public API + 推奨 GCHeader 雛形 (extern 宣言)
  gc.c             # 共通 runtime: ASTRO_PRECISE_GC_<algo> マクロで
                   # gc_<algo>.c を #include
  gc_copy.c        # 詳細実装: 各 backend
  gc_mark.c
  gc_mark_gen.c
  ... (16 backend)

sample/<lang>/
  main.c           # 以下の順で書く:
                   #   1. sample 自身の VALUE / GCHeader / kind 定義
                   #   2. ASTRO_GC_* contract macro 群を define
                   #   3. ASTRO_PRECISE_GC_<algo> を define
                   #   4. #include "../../runtime/precise_gc/gc.c"
                   # main.c が 1 つの translation unit として framework
                   # runtime を取り込み、 LTO で SD chain に inline される
  gc.h (薄い)      # 他 TU 用、 `aro_gc_alloc` 等の extern 宣言のみ
```

algorithm 選択は **`-DASTRO_PRECISE_GC_COPY` 等の build flag** で。
sample の Makefile が現状の `GC=copy` を `-DASTRO_PRECISE_GC_COPY` に変換。

### 4.2 sample が提供する contract macro

framework (`runtime/precise_gc/gc.c` + 各 `gc_<algo>.c`) は以下の macro を
**sample が `#include "../../runtime/precise_gc/gc.c"` する前に define
してある** ことを前提に書く。

#### VALUE / header

| macro / typedef | 役割 |
|---|---|
| `typedef ... VALUE` | sample 固有の値表現 (LSB-tag intptr_t、 NaN-boxing、 等) |
| `struct GCHeader` | header layout (推奨雛形を framework が提供、 sample 拡張可) |
| `ASTRO_GC_VALUE_IS_PTR(v)` | VALUE が heap pointer か判定 (root scan / WB で使う) |
| `ASTRO_GC_VALUE_TO_HEADER(v)` | VALUE → GCHeader * (= `(GCHeader *)(v) - 1` 等) |
| `ASTRO_GC_HEADER_TO_VALUE(h)` | GCHeader * → VALUE (forwarding 後の rewrite で使う) |

#### Object shape (= 「graph traversal」 の記述)

GC は object を「graph のノード + pointer のエッジ」 として扱う。 不連続な
メモリブロック (例: BaArray header + items[] が 2 alloc) は **別々の GC
object として alloc し、 pointer で繋ぐ**。 GC は kind の意味を知らず、
sample 提供の macro で children を traverse する。

| macro | 役割 |
|---|---|
| `ASTRO_GC_HEADER_KIND(h)` | header から kind 取り出し (mark/sweep 内の switch で使う) |
| `ASTRO_GC_SCAN_OBJECT(h, visit)` | children traversal、 各子に `visit(child_header)` を呼ぶ |
| `ASTRO_GC_FORWARD_OBJECT(h, fwd_func)` | moving 後の internal pointer rewrite |
| `ASTRO_GC_IS_OPAQUE_BYTES(h)` | scan 対象外 (= byte payload 等) なら true |
| `ASTRO_GC_INIT_PAYLOAD(payload, size)` | scan-safe 初期化 (baruby は memset 0、 NaN-boxing は NIL fill 等) |
| `ASTRO_GC_INIT_BYTE_PAYLOAD(payload, size)` | byte payload 初期化 (普通 skip) |
| `ASTRO_GC_OBJECT_SIZE(h)` | payload bytes 取得 (realloc / next-object walk で使う) |

#### Root scan

`c->env..c->sp` の flat-scan は **baruby_precise 固有の root layout**。
複数 thread / globals registry / finalizer queue 等を持つ sample もある
ので、 framework は root の場所を知らず、 sample が `SCAN_ROOTS` macro
で記述する。

| macro | 役割 |
|---|---|
| `ASTRO_GC_SCAN_ROOTS(visit_value, current_sp_top)` | 全 root scan。 thread stack / globals / 等を全部歩いて各 VALUE を visit |

`current_sp_top` は GC trigger 時点の現スレッド sp top (= alloc が呼び出された瞬間
の scan 上限)。 他 thread の sp は safepoint 時点の値を sample 側で参照する
(future 課題: 現状 single-thread なので current_sp_top のみで十分)。

### 4.3 backend が立てるフラグ (header layout の条件化)

backend ごとに header の必要フィールドが違う:

| backend group | NEEDS_FWD | NEEDS_HEADER_MARK | NEEDS_SIZE |
|---|---|---|---|
| moving (copy / copy_gen / mark_compact / immix compact) | ✓ | ✗ | ✓ |
| non-moving slab (mark / mark_gen / mark_freelist) | ✗ | ✓ | △ slab class で導出可 |
| non-moving region (bump / mark_freelist) | ✗ | ✓ | ✓ |
| side-bitmap (mark_bitmap_gen) | ✗ | ✗ (別領域に mark bit) | △ |

各 `gc_<algo>.c` 冒頭で `#define ASTRO_GC_NEEDS_FWD` 等を立て、 framework
の GCHeader が #ifdef で field を出し入れする:

```c
struct GCHeader {
    uint32_t flags;        // kind (sample 定義) + (framework 用 bit)
    uint32_t size;         // (NEEDS_SIZE backend なら)
#ifdef ASTRO_GC_NEEDS_FWD
    void *fwd;
#endif
};
```

これで non-moving の `fwd` 8B 無駄を回避、 small object が slab class
にぴったり収まる (= baruby BaArray 24B + header 8B = 32B = slab class 32)。
現状 baruby_precise の `sizeof(GCHeader) = 16` は moving と non-moving を
両立させた最大公約数。 framework 化を機に backend ごと最適化できる。

### 4.4 framework が提供する API

sample 側コードから呼ぶ public API (= `runtime/precise_gc/gc.h` で
extern 宣言):

| API | 役割 |
|---|---|
| `aro_gc_init(void)` | 初期化 (root range 引数は取らない、 sample の SCAN_ROOTS が独立) |
| `aro_gc_alloc(size, sp_top)` | object alloc、 sp_top は GC trigger 時 scan 上限 |
| `aro_gc_alloc_byte(size, sp_top)` | byte payload alloc (scan skip / init skip) |
| `aro_gc_realloc_payload(old, new_size, sp_top)` | resize (moving 対応の安全な realloc) |
| `aro_gc_wb(holder, slot, v)` | write barrier (gen backend は remset / dirty card 更新、 non-gen は inline `*slot = v`) |
| `aro_gc_collect(sp_top)` | 明示 collect 要求 (主に test / stress 用) |

backend 内部関数 (= gc_<algo>.c で defined) は static inline で main.c に
取り込まれる。 LTO で SD chain に inline される。

### 4.5 移行計画 (PoC)

framework に切り出す前に **baruby_precise 内で 「抽象化のみ」 完了** させる
のが安全:

1. **Step 1**: baruby_precise の `gc.{h,c}` + 各 `gc_<algo>.c` を contract
   macro 経由に書き換え。 ファイル位置はそのまま `sample/baruby_precise/`
   配下。 動作 / perf が変わらないことを 15 backend × 35 bench oracle で確認。
2. **Step 2**: `runtime/precise_gc/` に移動。 baruby_precise の main.c は
   `#include "../../runtime/precise_gc/gc.c"` に切替。 再度 oracle 確認。
3. **Step 3**: 2 つ目の sample (例: koruby か abruby) で `runtime/precise_gc/`
   を使ってみる。 contract macro 不足が出たら framework 側に戻す。
4. **Step 4**: 残り sample に展開、 各 sample の自前 GC (= libgc / 専用
   semi-space 等) を framework 経由に統一できるところは統一。

Step 1 だけで「抽象化が動く」 ことの証明になるので、 工数小さく valuable。
Step 2 以降は徐々に。

### 4.6 残課題 / 別議論

- **current thread context の取り出し**: multi-thread で「現スレッドの
  CTX を引っ張る」 API (TLS or pthread_self ベース) — single-thread 動作後
  に検討
- **header packing の per-backend optimization**: §4.3 で触れた `NEEDS_FWD`
  等の #ifdef 化、 framework 化に同伴して進める
- **walker の framework 化** (= `sample/baruby_precise/baruby_parse.c::
  walk_bake_sp_offset` を astrogen.rb の per-kind callback で自動生成):
  GC とは独立した別 todo、 [baruby_precise/docs/todo.md](../sample/baruby_precise/docs/todo.md) 参照
- **copy 系 backend の large_alloc 経路** (= bump + malloc ハイブリッド):
  framework 化前に baruby_precise で実装 → contract macro が必要な拡張を
  framework 化時に持ち上げる
- **`VALUE_DEF` 採否** (§1.7): 採用すれば SCAN_OBJECT / FORWARD_OBJECT
  macro を ASTroGen で自動生成できる可能性。 framework 化と同タイミングで
  再検討する価値あり
