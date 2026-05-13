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
3. **Root 列挙の mechanism** (`@roots` / `@root_array` 注釈と frame
   descriptor 自動生成) — §1.3
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

### 1.3 Root 列挙: frame iterator (precise の核)

precise GC を動かすには、 「いまどの局所変数が VALUE root か」 を GC が知れる
必要がある。 ASTro はこれを **`@roots(...)` 注釈** と **ASTroGen が自動生成
する frame descriptor** の組合せで実現する。

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

#### 1.3.2 `@roots(l)` で root を宣言する

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

これを見て ASTroGen は **2 つのもの** を生成する。

**(1) この node 専用の frame 構造体と、 その descriptor**:

```c
struct frame_node_add { VALUE l; };

static const astro_frame_desc_t FD_node_add = {
    .size        = sizeof(struct frame_node_add),
    .n_refs      = 1,
    .ref_offsets = { offsetof(struct frame_node_add, l) },
};
```

`FD_node_add` は per-NODE_DEF で 1 個だけ。 `.ref_offsets` を見れば「この
frame の先頭から 0 バイト目に VALUE root がある」 が分かる。

**(2) EVAL ラッパで frame の push/pop + ローカル名のリダイレクト**:

```c
static inline VALUE
EVAL_node_add(CTX *c, NODE *n, NODE *lv, /*...*/, NODE *rv, /*...*/)
{
    struct frame_node_add _f;
    ASTRO_FRAME_ENTER(c, &FD_node_add, &_f);   // c->fp_chain に push
#define l (_f.l)                                // BODY を変えずに l を frame slot に
    VALUE l = EVAL_ARG(c, lv);
    VALUE r = EVAL_ARG(c, rv);
    VALUE _ret = l + r;
#undef l
    ASTRO_FRAME_LEAVE(c);                       // pop
    return _ret;
}
```

`#define l (_f.l)` を BODY の直前に挟むことで、 **BODY のテキストは 1 文字も
変えずに** 当該変数を frame slot 経由のアクセスに変える、 というのが仕掛けの
中心。

#### 1.3.3 GC は frame chain を辿るだけ

`ASTRO_FRAME_ENTER` は `c->fp_chain` の先頭に `{desc=&FD_node_add, data=&_f}`
を push する。 nested に EVAL\_xxx が呼ばれれば chain が伸びる。 GC 起動時:

```c
void <lang>_gc_iter_roots(astro_root_visitor_t *v) {
    for (astro_frame_t *f = c->fp_chain; f; f = f->prev) {
        // f->desc->ref_offsets を見て f->data + offset 位置の VALUE を visit
        astro_visit_frame(v, f->desc, f->data);
    }
    // global root (関数テーブル、 トップレベル frame 等) は v に直接渡す
}
```

`<lang>_gc_iter_roots` は **言語が 1 個だけ実装** する。 ref_offsets /
ref_array を実際に walk する処理は backend に持たせる。

#### 1.3.4 拡張 1: 可変長 root 列 (`@root_array`)

`@roots(l, r)` は **固定個** の名前付きローカル用。 「`n` 個分の VALUE 配列を
全部 root にしたい」 ケースには `@root_array(base, count)` を使う:

```c
NODE_DEF @root_array(F, locals_cnt)
node_call_1(CTX *c, NODE *n, VALUE *fp, /*...*/, uint32_t locals_cnt, NODE *a0)
{
    VALUE F[locals_cnt];     // VLA、 callee の新 frame
    F[0] = UNWRAP(EVAL_ARG(c, a0));
    ...
}
```

これは frame_desc の `ref_array` フィールドに対応する:

```c
static const astro_frame_desc_t FD_node_call_1 = {
    .size      = sizeof(struct frame_node_call_1),
    .n_refs    = 0,
    .ref_array = {
        .base_off  = offsetof(struct frame_node_call_1, F_ptr),
        .count_off = offsetof(struct frame_node_call_1, F_count),
    },
};
```

ASTroGen は ENTER 時に `F` と `locals_cnt` を frame 構造体にコピーするコードも
出す。 `base` / `count` は **ローカル変数だけでなく共通引数** も指定でき、
NODE_DEF の `common_param_count` で渡される frame pointer もそのまま
root_array にできる。

#### 1.3.5 SD 内では frame を集約する (per-SD 1 個)

ここまで示した「EVAL\_\<name\> ごとに ENTER/LEAVE」 は **インタプリタ経路
(= DISPATCH\_\<name\>) の論理モデル**。 specialize 経路 (= SD\_\<hash\>) で
そのままやると tight loop で 4 メモリオペ/node が乗って遅すぎる。

SD は inline tree の全 NODE_DEF をひとつの関数に畳み込んだもの。 SPECIALIZE
時に ASTroGen が tree 内の `@roots(...)` をすべて拾い集めて **SD 関数ごとに
1 個のアグリゲート frame** を組み、 push/pop は **SD 関数の入口と出口で 1 回ずつ**
だけ行う:

```c
struct frame_SD_<hash> {
    VALUE n0_l;          // node_add の @roots(l)
    VALUE n1_r;          // 内側の別 node の @roots(r)
    VALUE n2_v;          // ...
};

static const astro_frame_desc_t FD_SD_<hash> = {
    .size        = sizeof(struct frame_SD_<hash>),
    .n_refs      = 3,
    .ref_offsets = { offsetof(struct frame_SD_<hash>, n0_l),
                     offsetof(struct frame_SD_<hash>, n1_r),
                     offsetof(struct frame_SD_<hash>, n2_v) },
};

VALUE SD_<hash>(CTX *c, NODE *n)
{
    struct frame_SD_<hash> _f;
    ASTRO_FRAME_ENTER(c, &FD_SD_<hash>, &_f);   // SD 入口で 1 回だけ
    /* inlined tree — 個別 ENTER/LEAVE は出さない、
       BODY 内の `l` / `r` / `v` は #define で _f.n0_l 等に展開 */
    ...
    ASTRO_FRAME_LEAVE(c);                        // SD 出口で 1 回だけ
    return _ret;
}
```

これでチェーン操作は **関数呼出し境界の頻度** に下がる (= 言語の call の頻度。
node 評価の頻度より 1〜2 桁低い)。 個別 EVAL\_\<name\> 内には ENTER/LEAVE を
出さず、 frame slot のリダイレクトだけ生成する。

| 経路 | frame の単位 | push/pop コスト |
|---|---|---|
| DISPATCH\_\<name\> (インタプリタ) | per-NODE_DEF | 1 push + 1 pop / node call |
| SD\_\<hash\> (specialize) | per-SD invocation | 1 push + 1 pop / SD 呼出 |

`SD_<hash>` 自体が他の `SD_<hash>` を呼び出す境界 (= 言語の call) で次の SD の
push が起きるので、 frame chain の深さは関数呼出しの深さに一致する。

#### 1.3.6 descriptor 型まとめ

```c
struct astro_frame_desc_t {
    uint16_t size;                      // frame 構造体のサイズ
    uint16_t n_refs;                    // ref_offsets[] の長さ
    uint16_t ref_offsets[/*n_refs*/];   // 固定個のローカル root (@roots 由来)
    struct {                            // 可変長 root 列 (@root_array 由来)
        uint16_t base_off;              //   VALUE * の offset
        uint16_t count_off;             //   length の offset
    } ref_array;                        // .count_off==0 なら無し
};

#define ASTRO_FRAME_ENTER(c, desc, frame_ptr)  /* on-stack chain push */
#define ASTRO_FRAME_LEAVE(c)                   /* pop */
```

注釈 → descriptor の対応:

| 注釈 | descriptor field |
|---|---|
| `@roots(l, r, ...)` | `n_refs` + `ref_offsets[]` |
| `@root_array(base, count)` | `ref_array.base_off` / `count_off` |
| 注釈なし | descriptor 生成しない (frame ENTER/LEAVE も無し) |

ENTER/LEAVE は安全点 (§1.5) でしか chain 整合性が保証されない cooperative
設計。 signal-based の preemptive GC は wasm との両立が悪いため不採用。

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

```c
#define ASTRO_SAFEPOINT(c) do {           \
    if (UNLIKELY(astro_gc_pending)) {     \
        astro_gc_flush_frame(c);          \
        astro_gc_handshake();             \
        astro_gc_reload_frame(c);         \
    }                                     \
} while (0)
```

挿入箇所 (ASTroGen 自動):

- allocation site の直前
- call 境界の入口
- 例外ハンドラ境界

NODE_DEF レベルの注釈:

| オプション | 意味 |
|---|---|
| `@noalloc` | BODY + 全 transitive children が allocate しないことを保証。 leaf node (literal / 変数参照系) で使う。 ASTroGen は frame ENTER/LEAVE を省略し、 safepoint も入れない |
| `@safepoint` | BODY 末尾に safepoint poll を強制挿入。 allocation を含まない長い loop back-edge で使う |

**default の挙動**: `EVAL_ARG` で子を評価する非 leaf ノードは原理的に transitive
に allocate しうるため、 default を「allocate しうる」 保守側に倒す。 opt-in
注釈 `@allocates` は持たない (= 全部に書く羽目になり意味がない)。 leaf の
opt-out として `@noalloc` だけを残す。

**CTX を取らない C helper 内での safepoint**: `baruby_ary_push(VALUE av, VALUE
x)` のように CTX を引数で受け取らない C helper が内部で allocate する場合、
framework が提供する `astro_gc_alloc` / `astro_gc_realloc_payload` 等の
**alloc API は内部で safepoint poll を含む**、 を framework 規約として固定する。
helper は signature 不変のまま precise GC に対応できる。 内部の現在 CTX は
TLS 経由 (`astro_gc_current_ctx()`) で取得 (single-thread 前提では グローバル 1 個)。

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

baruby の典型: `node_call_aset` で `r` と `v` が `baruby_ary_push` (= 潜在的
allocate) をまたぐ:

```c
NODE_DEF @roots(r, v)
node_call_aset(CTX *c, NODE *n, VALUE *fp, NODE *recv, NODE *idx, NODE *val)
{
    VALUE r = UNWRAP(EVAL_ARG(c, recv));   // i / v 評価 / push 呼出を生存
    VALUE i = UNWRAP(EVAL_ARG(c, idx));    // intptr に変換してすぐ死ぬ → root 不要
    VALUE v = UNWRAP(EVAL_ARG(c, val));    // WB まで生存
    if (IS_ARY(r)) {
        BaArray *a = VAL2ARY(r);
        intptr_t ii = VAL2INT(i);
        ...
        while ((uint32_t)ii >= a->len) baruby_ary_push(r, VAL_NIL);
        WB(a, items[ii], v);               // 素の代入 → WB macro 経由
        return RESULT_OK(v);
    }
    ...
}
```

leaf 系には `@noalloc`:

```c
NODE_DEF @noalloc
node_num(CTX *c, NODE *n, VALUE *fp, int32_t num) { return RESULT_OK(INT2VAL(num)); }

NODE_DEF @noalloc
node_lget(CTX *c, NODE *n, VALUE *fp, uint32_t index) { return RESULT_OK(fp[index]); }

NODE_DEF @noalloc
node_true(CTX *c, NODE *n, VALUE *fp) { return RESULT_OK(VAL_TRUE); }
```

判断基準: BODY 内に `EVAL_ARG` / `astro_gc_alloc` / `BA_ALLOC_*` / `WB` のいずれも
無ければ `@noalloc` を付けてよい。 違反は ASTroGen が BODY を grep して CI で
検出する。

### 2.4 関数境界の root_array

baruby は function call の節 (`node_call_*` の callee セットアップ部分) で
新しい frame (`VALUE F[locals_cnt]`) を作る。 ここに `@root_array` を付ける:

```c
NODE_DEF @root_array(F, locals_cnt)
node_call_1(CTX *c, NODE *n, VALUE *fp, const char *name, uint32_t arg_index,
            uint32_t locals_cnt, struct callcache *cc@ref, NODE *a0)
{
    VALUE F[locals_cnt];                 // VLA、 callee の新 frame
    F[0] = UNWRAP(EVAL_ARG(c, a0));
    return EVAL(c, cc->body, F);
}
```

トップレベルの起点だけは `main.c` で:

```c
astro_gc_register_toplevel_frame(c->env, /*size=*/ toplevel_size);
RESULT r = EVAL(c, ast, c->env);
astro_gc_unregister_toplevel_frame();
```

caller の `VALUE *fp` 自体は caller の NODE_DEF (= caller の EVAL ラッパ) で
既に root_array 化されているので、 各 node からは fp は触らなくてよい。

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
baruby_ary_push(VALUE av, VALUE x)
{
    BaArray *a = VAL2ARY(av);
    if (a->len == a->capa) {
        uint32_t new_capa = a->capa ? a->capa * 2 : 4;
        // astro_gc_realloc_payload が内部で safepoint poll を含む (§1.5)
        a->items = astro_gc_realloc_payload(astro_gc_heap(HEAP_REF_PAYLOAD),
                                            a->items, sizeof(VALUE) * new_capa);
        a->capa = new_capa;
    }
    WB(a, items[a->len], x);
    a->len++;
}
```

`baruby_ary_push` の signature は変えていない (CTX を取らない)。 framework
alloc API が TLS 経由で現在 CTX を見つけて safepoint を打つ。

### 2.7 frame iterator (言語側 1 関数)

```c
void
baruby_gc_iter_roots(astro_root_visitor_t *v)
{
    // 局所 frame chain
    for (astro_frame_t *f = global_c->fp_chain; f; f = f->prev) {
        astro_visit_frame(v, f->desc, f->data);
    }
    // global root: 関数テーブル (本体は AST = immortal、 走査不要)
    //              code repo / トップレベル frame 等は別 API で登録済
}
```

これで baruby 側が GC に提供する root 列挙は完了。

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
| 各 `NODE_DEF` header | leaf に `@noalloc`、 alloc/call をまたぐ root に `@roots(...)` | ~40 箇所 |
| 関数境界 ノード | `@root_array(F, locals_cnt)` | 4 箇所 (call_0/1/2/3) |
| `node.c` | `baruby_ary_new` 等を framework alloc API 経由に | ~6 関数 |
| `node.c` | 素の `a->items[ii] = v` を `WB(a, items[ii], v)` に | ~10 箇所 |
| `node.c` | `baruby_gc_iter_roots` を 1 個実装 | 1 関数 |
| `main.c` | toplevel frame 登録 | 2 行 |
| `Makefile` | `GC=...` で backend ソースを切替 | 5-10 行 |

NODE_DEF の BODY テキスト自体は **1 行も変更不要**。 header と `node.c` の
ヘルパだけが変わる。

---

## 未決事項

- `VALUE_DEF` 採否 (§1.7) — 採用すれば marker 自動生成、 不採用なら手書き。
  まず §1.3-1.6 だけで baruby を回し、 marker 手書きの工数が見えてから判断
- `HEAP_FINALIZABLE` の解放順序 — BDW 風 topological を default、
  `@finalizer F @order=N` で override 可、 を仮置き
- `@ref_array` の長さ表現 — `count_var` (単一 unsigned) のみ採用、
  begin/end ペアは moving fixup が複雑になるので不採用
- multi-thread での TLS 切替 — single-thread が動いてから検討
