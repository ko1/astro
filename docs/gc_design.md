# ASTro GC 設計案

ASTro framework が GC についてどういうサポートを提供するか、 そしてそれを
`sample/baruby` に適用するときに使うインターフェースは何か、 をまとめたもの。
実装はまだ着手していない設計段階のメモ。

関連: `idea.md` §8.2 (未踏項目), `code_store_quirks.md` (AST NODE 不動制約の
出処), `sample/baruby/` (本案の最初の testbed)。

---

## 0. 概要

ASTro framework は「インタプリタを node.def 宣言から自動生成する」 という
姿勢を取っているので、 GC についても **言語側 (node.def + ランタイム
ヘルパ) が宣言する → ASTroGen が必要な C を吐く → backend が algorithm を
差し替える** という 3 層で揃える。

framework が提供するもの:

1. **ヒープの kind 分離** (`HEAP_IMMORTAL` / `HEAP_VALUE` 等) — §1.1
2. **allocation API** (`astro_gc_alloc` / `astro_gc_alloc_payload`) — §1.2
3. **Root 列挙の mechanism** (`@roots` / `@scratch` 注釈で sp[] に spill) — §1.3
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

### 1.3 Root 列挙: 共有 stack 上の spill

precise GC を動かすには「いま live な VALUE root が **既知の連続領域** に
ある」 状態を作る必要がある。 ASTro は Lua / Python と同様の **stack-based**
モデルを採る:

- 各 NODE_DEF は `VALUE *sp` を共通引数で受け取る (= 共有 stack の top)
- root にすべき VALUE は **`sp[]` の slot に spill** する
- GC は `c->fp_base..c->sp` の VALUE 配列を flat scan するだけ
- **per-node の chain push/pop は無し**。 spill した VALUE は sp[] に置かれた
  まま親 frame に積み上がり、 関数 return で sp が戻ることで自然に解放される

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

precise GC ではここに問題がある: `l` は `EVAL_ARG(c, rv)` を生存する必要が
あるが、 ASTroGen は **「`l` が VALUE root だ」 と知る方法がない**。 普通の
C コンパイラから見れば `l` はただのローカル変数で、 中身が GC ヒープのポインタ
かどうか区別がつかない。

#### 1.3.2 `@roots(l)` で root を宣言 → ASTroGen が `sp[]` に spill する

言語側が「`l` は root です」 と教える:

```c
NODE_DEF @roots(l)
node_add(CTX *c, NODE *n, NODE *lv, NODE *rv)
{
    VALUE l = EVAL_ARG(c, lv);   // この時点から root
    VALUE r = EVAL_ARG(c, rv);   // ここで GC が走っても l は生きている
    return l + r;
}
```

ASTroGen は BODY を書き換えて、 `l` を **`sp[0]` (= 共有 stack の既知 slot)**
に格納する:

```c
static inline VALUE
EVAL_node_add(CTX *c, NODE *n, VALUE *sp,
              NODE *lv, dispatch_t _lv_d,
              NODE *rv, dispatch_t _rv_d)
{
    /* node_add の spill region: sp[0..0] (l 用 1 slot) */
    sp[0] = (*_lv_d)(c, lv, sp + 1);   // 子は sp + 1 を受け取る
    VALUE r = (*_rv_d)(c, rv, sp + 1);
    return sp[0] + r;
}
```

ポイント:

- `l` は C local ではなく **`sp[0]` (memory) に格納**。 子 evaluation で
  allocate しても `sp[0]` は memory 上に残り、 GC は root として scan できる
- `r` は最後の `+` でしか使わない (= 子 call をまたがない) ので spill 不要 →
  **C local のまま OK**。 `@roots` に含めない
- 子 evaluation には `sp + 1` を渡す。 子は **親の上に** 自分の spill 領域を
  積み上げる (Lua-style stack)

BODY 書き換えのルール (ASTroGen 側):

- `VALUE <name>` という宣言を **取り除く** (slot は sp[] 側に確保済)
- 残った `<name>` の参照を **`sp[<slot_idx>]` に置換**
- slot_idx は `@roots(...)` の列挙順

`@roots` の名前と BODY 中の `VALUE <name>` 宣言が対応していないと ASTroGen
が error で止まる (タイポ検出)。

#### 1.3.3 GC は sp[] を flat scan するだけ

GC 起動時:

```c
void <lang>_gc_iter_roots(astro_root_visitor_t *v) {
    for (VALUE *p = c->fp_base; p < c->sp; p++) {
        astro_visit_value(v, *p);
    }
    // global root (関数テーブル、 symbol テーブル等) は v に直接渡す
}
```

per-node の frame chain は **無い**。 spill した VALUE は sp[] に置かれた
まま親 frame に積み上がる。 hot path 中は何もしない。

#### 1.3.4 `c->sp` の update タイミング

GC は allocation 経由でしか起きないので、 **allocation site の直前で
`c->sp` を update** すれば足りる。 framework alloc API がこれを内部で行う:

```c
void *
astro_gc_alloc(astro_heap_t h, uint32_t kind, size_t size, VALUE *sp_top)
{
    CTX *c = astro_gc_current_ctx();
    c->sp = sp_top;                              // 1 store
    if (UNLIKELY(astro_gc_pending)) astro_gc_handshake();
    return /* freelist bump */;
}
```

NODE_DEF body から framework alloc を直接呼ぶ場合は `sp_top` を渡す:

```c
sp[0] = astro_gc_alloc(astro_gc_heap(HEAP_VALUE), OBJ_ARRAY,
                       sizeof(BaArray), sp + 1);   // ← sp + 1 = 自分の top
```

CTX を取らない C helper (`baruby_ary_push` 等) が内部で alloc する場合、
helper の signature に `VALUE *sp_top` を 1 引数追加して通す。

**hot path 中で `c->sp` を毎 node update する必要はない**。 alloc しない限り
GC が起きないため。 これが per-node chain push/pop と比べた本質的な利点。

#### 1.3.5 コスト分析

| 設計 | 1 node 評価あたりの hot path コスト |
|---|---|
| per-NODE_DEF frame chain (没案) | ~5 mem ops (chain push + pop) |
| **sp[] spill (本案)** | **spill する root 1 個につき 1 store / alloc 1 回につき c->sp 更新 1 store** |

具体例:

- 純 fixnum tight loop (`a + b` が fixnum-only): @roots が必要な root に対して
  spill 1 store/node。 c->sp の更新は alloc がないので 0。 root が最後の式
  だけなら spill すら不要 (= **完全ゼロコスト**)
- allocation 込み hot path: spill + c->sp 更新が alloc 1 回ごとに少々。 chain
  方式の 1/5 以下

per-node ENTER/LEAVE がない = SD 集約問題 (build time vs runtime の inline_idx
問題) も発生しない。 ASTroGen 時に `l` → `sp[0]` の書き換えで完結する。

#### 1.3.6 callee frame など可変長 root

baruby の `VALUE F[locals_cnt]` (callee の新 frame) のような可変長 root も
sp の延長で自然に表せる。 親の sp の上に `locals_cnt` 個の slot を確保する
だけ:

```c
NODE_DEF @scratch(locals_cnt)         // BODY 開始前に locals_cnt slot 消費
node_call_1(CTX *c, NODE *n, VALUE *sp, /*...*/, uint32_t locals_cnt, NODE *a0)
{
    /* sp[0..locals_cnt-1] が callee の locals 領域 (= 旧 F[]) */
    sp[0] = (*_a0_d)(c, a0, sp + locals_cnt);    // arg 評価は callee 領域の上
    return EVAL(c, cc->body, sp);                // callee は sp[0..locals_cnt-1] を見る
}
```

`@scratch(<expr>)` は「BODY が開始時に sp 上に確保する slot 数」 を指定する
注釈。 固定数 (例 `@scratch(3)`) と動的数 (例 `@scratch(locals_cnt)`) の両方
を許す。 GC scan は `c->fp_base..c->sp` の flat scan なので、 そこに居る限り
自動的に root として扱われる。

#### 1.3.7 cooperative である理由

c->sp の更新は alloc site のみで OK というのは、 GC が **cooperative** (=
alloc 経由でしか起きない) だからこそ成立する。 signal-based preemptive GC
だと、 任意のタイミングで GC が割り込んでくるので c->sp が毎 node 正確に
最新でないと壊れる。 cooperative の選択はゼロコスト性の前提条件。

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

### 2.3 NODE_DEF への root / safepoint 注釈

baruby は `common_param_count=3` で `VALUE *fp` を渡しているが、 sp[] モデル
では **`fp` を sp に置き換え** て (= 共有 stack の一部にする)、 各 NODE_DEF
は自分の spill 領域を sp の上に積む形にする。

`node_call_aset` の典型 (`r` と `v` が `baruby_ary_push` をまたぐ):

```c
NODE_DEF @roots(r, v)
node_call_aset(CTX *c, NODE *n, VALUE *sp, NODE *recv, NODE *idx, NODE *val)
{
    VALUE r = UNWRAP(EVAL_ARG(c, recv));   // ← ASTroGen により sp[0] に rewrite
    VALUE i = UNWRAP(EVAL_ARG(c, idx));    // i は intptr に変換してすぐ死ぬ → C local
    VALUE v = UNWRAP(EVAL_ARG(c, val));    // ← ASTroGen により sp[1] に rewrite
    if (IS_ARY(r)) {
        BaArray *a = VAL2ARY(r);
        intptr_t ii = VAL2INT(i);
        ...
        while ((uint32_t)ii >= a->len) baruby_ary_push(r, VAL_NIL, sp + 2);
        a = VAL2ARY(r);                    // 念のため reload (moving backend 対応)
        WB(a, items[ii], v);
        return RESULT_OK(v);
    }
    ...
}
```

ASTroGen 書き換え後:

```c
sp[0] = UNWRAP((*_recv_d)(c, recv, sp + 2));
VALUE i = UNWRAP((*_idx_d)(c, idx, sp + 2));
sp[1] = UNWRAP((*_val_d)(c, val, sp + 2));
if (IS_ARY(sp[0])) {
    BaArray *a = VAL2ARY(sp[0]);
    intptr_t ii = VAL2INT(i);
    ...
    while ((uint32_t)ii >= a->len) baruby_ary_push(sp[0], VAL_NIL, sp + 2);
    a = VAL2ARY(sp[0]);
    WB(a, items[ii], sp[1]);
    return RESULT_OK(sp[1]);
}
```

`baruby_ary_push` は `sp_top` を追加引数で受け、 内部の
`astro_gc_realloc_payload` に通す。 `c->sp` の更新は framework alloc API の
中で行われるので NODE_DEF 側からは見えない。

leaf 系には `@noalloc`:

```c
NODE_DEF @noalloc
node_num (CTX *c, NODE *n, VALUE *sp, int32_t num) { return RESULT_OK(INT2VAL(num)); }
NODE_DEF @noalloc
node_lget(CTX *c, NODE *n, VALUE *sp, uint32_t index) { return RESULT_OK(/*fp[index]*/); }
NODE_DEF @noalloc
node_true(CTX *c, NODE *n, VALUE *sp) { return RESULT_OK(VAL_TRUE); }
```

判断基準: BODY 内に `EVAL_ARG` / `astro_gc_alloc` / `BA_ALLOC_*` / `WB` のいずれも
無ければ `@noalloc` を付けてよい。 違反は ASTroGen が BODY を grep して CI で
検出する。

### 2.4 関数境界: callee frame は sp の上に確保

baruby は現状 `VALUE F[locals_cnt]` で C スタック VLA に callee frame を
取っているが、 sp[] モデルでは **sp の上に locals_cnt 個の slot を確保**
することで callee frame が自動的に GC scan 対象になる:

```c
NODE_DEF @scratch(locals_cnt)
node_call_1(CTX *c, NODE *n, VALUE *sp, const char *name, uint32_t arg_index,
            uint32_t locals_cnt, struct callcache *cc@ref, NODE *a0)
{
    /* sp[0..locals_cnt-1] が callee の locals 領域 */
    sp[0] = UNWRAP(EVAL_ARG(c, a0));                  // arg を locals[0] に置く
    return EVAL(c, cc->body, sp);                     // callee は sp[0..] を見る
}
```

ASTroGen は `@scratch(locals_cnt)` を見て「BODY 開始時に sp 上に動的サイズ
の領域を確保」 することを認識する。 サイズが parser から正しく渡されている
ことを assert で check。

C VLA を捨てる利点: GC が `c->fp_base..c->sp` を flat scan するだけで callee
frame まで自動的に含まれる。 別途 `@root_array` で範囲を declare する必要なし。

トップレベルの起点だけは `main.c` で:

```c
c->fp_base = c->env;      // stack の最下端を確定
c->sp      = c->env;      // 初期 top
RESULT r = EVAL(c, ast, c->env);
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
| 各 `NODE_DEF` header | leaf に `@noalloc`、 alloc/call をまたぐ root に `@roots(...)` | ~40 箇所 |
| 関数境界 ノード | `@scratch(locals_cnt)` (callee frame を sp に置く) | 4 箇所 (call_0/1/2/3) |
| `node.c` | `baruby_ary_new` 等を framework alloc API 経由に | ~6 関数 |
| `node.c` | CTX 取らない C helper に `sp_top` 引数を追加 | ~4 関数 |
| `node.c` | 素の `a->items[ii] = v` を `WB(a, items[ii], v)` に | ~10 箇所 |
| `node.c` | `baruby_gc_iter_roots` を 1 個実装 (sp[] flat scan) | 1 関数 |
| `main.c` | `c->fp_base = c->sp = c->env` の初期化 | 2 行 |
| `Makefile` | `GC=...` で backend ソースを切替 | 5-10 行 |

NODE_DEF の BODY テキスト自体は **1 行も変更不要**。 ASTroGen が `@roots` から
`l` → `sp[0]` 等の書き換えを自動で行う。 header と `node.c` の helper だけが
変わる。

---

## 未決事項

- `VALUE_DEF` 採否 (§1.7) — 採用すれば marker 自動生成、 不採用なら手書き。
  まず §1.3-1.6 だけで baruby を回し、 marker 手書きの工数が見えてから判断
- ASTroGen の BODY 書き換え範囲 — `@roots` で挙げた名前以外も「子 call を
  またぐ VALUE」 を自動検出して spill する live-range 解析を入れるか、
  `@roots` 明示のみで行くか。 まず明示のみで実装し、 typo を error 検出するに
  留めるのが安全
- `HEAP_FINALIZABLE` の解放順序 — BDW 風 topological を default、
  `@finalizer F @order=N` で override 可、 を仮置き
- moving backend での pointer reload (`a = VAL2ARY(sp[0])` の自動挿入) —
  ASTroGen が必要箇所を解析して reload を挟むか、 手書きで対応するか
- multi-thread での TLS 切替 — single-thread が動いてから検討
