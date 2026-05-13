# ASTro 統一 GC 設計案

ASTro framework のサンプル横断で使える pluggable な precise GC 基盤の設計案。
**実装は未着手**、選択肢を狭めず議論継続中の段階のメモ。

関連: `idea.md` §8.2 (未踏項目), `code_store_quirks.md` (AST NODE 不動制約の出処),
`perf.md` (CTX hot member lift 議論), `sample/baruby/` (本案の最初の testbed)。

---

## 0. 概観

### 0.1 現状: サンプルごとに GC がバラバラ

ASTro framework には現在 19 個ほどのサンプル言語実装があり、 値の lifetime
管理は **完全にサンプルごとに別もの** になっている:

| 現状 | サンプル | 性格 |
|---|---|---|
| GC なし | `calc`, `naruby`, `pascalast`, `castro`, `aforth`, `wastro` | int / static 型のみ |
| `libgc` 直叩き (conservative) | `koruby`, `pystro`, `asom`, `astr`, `baruby` 等 | 動的言語、 ヒープ多用 |
| 自前 mark&sweep | `luastro` | NaN-box + weak table |
| CRuby GC ホスト | `abruby`, `arjsv` | C 拡張、 host VM に委譲 |
| arena / region | `arcel` | activation 単位で reset |

5 種の memory management が乱立している状態を、 **1 つの framework 機構**
に統合したい、 というのが本案の出発点。

### 0.2 目指す形と、 統一しない部分

| 統一する | 統一しない |
|---|---|
| Allocation / mark / safepoint / barrier の **API 形** | 値の表現 (LSB tag / NaN-box / Flonum など) |
| **Root 列挙の mechanism** (frame descriptor) | 言語ごとの値の構造体定義 |
| AST NODE の扱い (= 絶対動かさない) | node.def の BODY |
| `node.def` declarative codegen の文化 | サンプル固有の builtin / runtime |

Algorithm (non-moving / semispace / generational / realtime) は **backend として
差し替え可能** にする。 同じ言語実装が `make GC=semispace` で moving に切り替わる、
というのを最終的な姿に置く。

### 0.3 ASTro 特有の制約 2 つ

1. **AST NODE は移動不可**。 Code Store が `SD_<hash>.so` 内に NODE \* を
   ポインタ literal として焼き込んでいるため、 moving GC backend を選んでも
   AST NODE だけは固定アドレスでなければ壊れる
2. **node.def の BODY テキストは触らない**。 これは `idea.md` の根本主張で、
   GC を入れる時も BODY を侵襲しない手段で WB / safepoint を仕込む必要がある

この 2 つが、 「世の中の GC interface 設計を転用するだけでは足りない」 ASTro 固有
の課題になっている。

### 0.4 提案の骨子

統一 GC を可能にするために framework が提供するのは、 すべて **declarative**
(言語側がデータとして宣言 → ASTroGen が必要な C を吐く) に揃えた **5 つの
機構**:

1. **値の型宣言** (`value.def` — 採否未決、 §1.1)
2. **ヒープの kind 分離** (HEAP_IMMORTAL / VALUE / PAYLOAD / FINALIZABLE / LARGE、 §1.2)
3. **Root 列挙** (`@roots(...)` / `@root_array(...)` 注釈 + frame descriptor 自動生成、 §1.4)
4. **Write barrier 経路** (`@ref(value)` setter / `WB` macro、 §1.5)
5. **Safepoint 配置** (default `@noalloc` opt-out、 §1.6)

backend (algorithm) は **compile-time** で 1 個選ぶ (`make GC=...`)。 `GC=none` を
選ぶと全部の hook が `(void)0` / identity に潰れて現行コードと等価になる、 を
**ゼロコスト性の gate** にする。

### 0.5 この doc の読み方

- **§0** で全体像を掴む
- **§1** で framework が提供する各インターフェースの詳細と理由を読む
  (1.1 〜 1.8)
- **§2** で `sample/baruby` を題材にした使い方を見る

`baruby` を testbed に選んだ理由は、 値表現がシンプル (Array + String + LSB tag)
で、 現状 libgc 保守的スキャンで動いているため「同じ node.def から保守的 backend
と precise backend の両方が出せる」 という抽象化の正しさを最小コストで検証できる
ため。 `sample/baruby/README.md` 自身がこれを宣言している。

---

## 1. フレームワークが提供するインターフェース

各機構について **「何 / なぜ / どう書く」** の 3 点で説明する。 すべての機構は
`node.def` の declarative 宣言から ASTroGen が C コードを生成する、 という共通
原則に従う。

### 1.1 値の型宣言 (`value.def` — 採否未決)

#### 何

各サンプルの値のメモリレイアウト (共通ヘッダ + kind 別フィールド) を宣言する
DSL。 `node.def` と同居させる。

```
VALUE_DEF baruby_obj
{
    OBJ_ARRAY   => ref_payload(VALUE) items; uint32_t len; uint32_t capa;
    OBJ_STRING  => atomic_payload(char) bytes; uint32_t len; uint32_t capa;
}
```

ASTroGen はこの宣言から:

- `KORB_ALLOC_<KIND>(...)` — kind 別 type-specialized allocator
- `KORB_MARK_<KIND>(obj)` — precise marker (`ref` / `ref_payload` を walk)
- `KORB_FORWARD_<KIND>(obj)` — moving backend 用 forward fixup
- `KORB_SET_<field>(obj, val)` — write barrier 込み setter

を生成する。

#### なぜ

abruby が既に `register_gen_task :mark` で marker を `node_mark.c` として
生成しているのが先例で、 これを framework 標準に格上げするのが筋。
moving / generational / SATB realtime のいずれの backend を選んでも、 値の
構造を 1 箇所で宣言できれば boilerplate を集約できる。

ただし **採否は未決**。 理由:

- 言語ごとに値表現が極端に違う (LSB tag / NaN-box / pointer-only / tagged
  union)。 これを 1 つの DSL に押し込むと表現力 vs 抽象性のトレードオフで詰まる
- `abruby` / `arjsv` は CRuby `VALUE` を変えられない、 `luastro` は NaN-box を
  変えられない (`feedback_no_nan_boxing`)
- ASTroGen が「root の場所」だけ知っていれば、 marker は **言語側手書きで足りる
  可能性が十分ある**

§1.4 〜 1.7 (root 列挙 / barrier / safepoint / operand 注釈) は `value.def`
**なしでも成立する** ように設計してある。 まず §1.4 〜 1.7 だけ採用して 2-3
サンプル試し、 必要性が見えてから DSL 化に進む、 が現実的。

#### 注釈

| 注釈 | 意味 |
|---|---|
| `ref(T)` | trace 対象、setter に write barrier を自動挿入 |
| `ref_payload(T)` | 二層オブジェクトの可変長 ref 配列。 別ヒープ、 一括 move 可 |
| `atomic_payload(T)` | ref を含まない bytes。 deep mark 不要、 moving 可 |
| `immortal` | 不動 (AST NODE / SD\_\<hash\>.so 内シンボル等) |
| `finalizer F` | 解放時 F 呼出し |
| `ref_weak(T)` | 弱参照、 mark queue に積まない |

### 1.2 ヒープの kind 分離

#### 何

backend 非依存に「このオブジェクトはどう扱うか」 を 1 軸に切る分類:

```c
typedef enum {
    HEAP_NONE,             // GC 機構を使わないサンプル用 (calc, naruby 等)
    HEAP_VALUE,            // 通常オブジェクト。 backend が moving なら動く
    HEAP_REF_PAYLOAD,      // 二層 ref 配列
    HEAP_ATOMIC_PAYLOAD,   // bytes、 deep mark なし
    HEAP_IMMORTAL,         // 不動・永続。 AST NODE はここ
    HEAP_LARGE,            // 単独 mmap (move しても利得薄い)
    HEAP_FINALIZABLE,      // finalizer 持ち
} astro_heap_kind_t;

astro_heap_t astro_gc_heap(astro_heap_kind_t k);
```

#### なぜ

**`HEAP_IMMORTAL` を framework 規約として固定するのが最大の動機**。 Code Store
が `SD_<hash>.so` 内に NODE \* を焼き込む問題は、 AST NODE をこのヒープへ強制
配置することで全 backend で一様に解消できる。 moving backend に切り替えても
AST NODE は動かないことが保証される。

ヒープ kind を分けると同時に、 backend ごとの実装責務がきれいに分かれる:

- `HEAP_VALUE` の moving は backend が処理
- `HEAP_REF_PAYLOAD` は別アロケータで一括 move (cache 局所性が立つ)
- `HEAP_ATOMIC_PAYLOAD` は marker が deep scan しない (高速)
- `HEAP_FINALIZABLE` だけ別キュー処理

#### どう書く

`value.def` を採用するならその field annotation (§1.1) から自動推定。 採用しない
場合は言語側の allocator 関数を kind ごとに分けて書く:

```c
VALUE baruby_ary_new(uint32_t capa) {
    // 二層オブジェクト: header は HEAP_VALUE、 items 配列は HEAP_REF_PAYLOAD
    BaArray *a = astro_gc_alloc(HEAP_VALUE, sizeof(BaArray));
    a->items = capa ? astro_gc_alloc_payload(HEAP_REF_PAYLOAD,
                                              sizeof(VALUE) * capa) : NULL;
    ...
}
```

### 1.3 アロケーション API

#### 何

```c
// kind は value.def の type id (採用しない場合は言語側 enum)、
// size は payload 込み。 LSB tagging は言語の魂なので戻り値はタグなし。
void *astro_gc_alloc(astro_heap_t h, uint32_t kind, size_t size);

// 二層オブジェクトの payload。 attr で atomic / movable を指定
void *astro_gc_alloc_payload(astro_heap_t h, size_t size,
                             astro_payload_attr_t a);
```

#### なぜ

戻り値は **タグなし** にする。 言語ごとの tag scheme (naruby の生 int64 /
baruby の LSB tag / koruby の Flonum tag / luastro の NaN-box) が違うので、
framework が tag を被せると言語の魂と衝突する。

#### どう書く

サンプル側は `value.def` 由来の生成 wrapper か、 自前 wrapper 経由で呼ぶ:

```c
// value.def 採用時:
VALUE v = (VALUE)KORB_ALLOC_OBJ_ARRAY(c, /*capa*/8);  // 言語側で LSB tag を被せる

// 採用しない場合:
BaArray *a = astro_gc_alloc(astro_gc_heap(HEAP_VALUE), OBJ_ARRAY, sizeof(BaArray));
VALUE v = (VALUE)a;  // baruby は LSB=0 = ptr なのでそのまま
```

### 1.4 Root 列挙: frame iterator (precise の核)

#### 何

precise GC の心臓部。 言語側は **frame iterator を 1 個実装** すれば、 各
NODE_DEF の root は `@roots(...)` / `@root_array(...)` 注釈から ASTroGen が
自動生成する frame descriptor 経由で列挙される:

```c
struct astro_frame_desc_t {              // 各 dispatcher で 1 回 static const
    uint16_t  size;
    uint16_t  n_refs;
    uint16_t  ref_offsets[/* n_refs */];   // F[] 内オフセット
    struct {                               // VALUE * + count_var 形式の可変長
        uint16_t base_off;
        uint16_t count_off;
    } ref_array;                           // .count_off==0 なら無し
};

#define ASTRO_FRAME_ENTER(c, desc, frame_ptr)  /* on-stack chain push */
#define ASTRO_FRAME_LEAVE(c)                   /* pop */

// 言語が 1 回だけ実装
void <lang>_gc_iter_roots(astro_root_visitor_t *v);
```

NODE_DEF 側で書ける注釈:

| 注釈 | 意味 |
|---|---|
| `@roots(name1, name2, ...)` | BODY 内のローカル `VALUE name` を frame descriptor の ref_offsets に追加。 ASTroGen は BODY を frame ENTER/LEAVE で包む |
| `@root_array(base, count)` | `VALUE *base` と `size_t count` を可変長 root 列として扱う。 `base` / `count` はローカル変数でも **共通引数** でもよい (baruby の `VALUE *fp` のように `common_param_count` で渡される frame pointer も対象にできる) |

ASTroGen は `@roots(r)` を見ると EVAL\_\<name\> ラッパをこう書き換える:

```c
static inline VALUE
EVAL_node_call1(CTX *c, NODE *n, VALUE *fp, NODE *recv, /*...*/)
{
    struct { VALUE r; } _f;
    ASTRO_FRAME_ENTER(c, &SD_node_call1_FD, &_f);
#define r (_f.r)              // BODY のテキストを変えずに root 化
    VALUE r = UNWRAP(EVAL_ARG(c, recv));
    VALUE a = UNWRAP(EVAL_ARG(c, arg));
    VALUE _ret = baruby_call1(c, r, a);
#undef r
    ASTRO_FRAME_LEAVE(c);
    return _ret;
}
```

`#define name (_f.name)` を BODY 直前に挟むことで、 **BODY のテキストは 1 文字も
変えずに** 当該変数を frame slot に上げる、 という仕掛け。

frame descriptor は SD ごとに `static const`:

```c
static const astro_frame_desc_t SD_<hash>_FD = {
    .size = sizeof(struct { VALUE r; }),
    .n_refs = 1,
    .ref_offsets = { offsetof(struct { VALUE r; }, r) },
};
```

#### なぜ

precise GC の rooting で「local VALUE をどう scan するか」 が常に問題になる。
ASTro はこれを **CTX hot member lift と同じ mechanism** (frame chain on CTX)
で実装する:

- preemptive ではなく cooperative (safepoint で flush)
- signal-based を避けることで wasm との両立、 koruby の setjmp/longjmp
  不使用方針との整合 を確保
- frame layout を SD レベルで一意にすることで、 JIT / AOT どちらでも同じ
  mechanism が効く

GC 起動時の root 列挙は:

```
<lang>_gc_iter_roots() →
  c->fp_chain を辿る → 各 frame の desc.ref_offsets / ref_array を visit
```

global root (function table、 symbol table 等) は言語が visitor に直接渡す。

### 1.5 Write barrier

#### 何

```c
// holder = 書き込まれる側のヘッダ、 val = 新値
#define ASTRO_WB_PTR(holder, field, val) do {                        \
    astro_gc_pre_wb((holder), (void **)&(holder)->field);            \
    (holder)->field = (val);                                         \
    astro_gc_post_wb((holder), (void *)(val));                       \
} while (0)
```

`value.def` 採用時はその field setter `KORB_SET_<field>(obj, val)` が自動的に
この経路を踏む。 採用しない場合は BODY で `WB(obj, field, val)` macro を明示的
に呼ぶ。

#### なぜ

backend ごとに pre/post の中身が違うが、 macro 経由にしておけば backend 切替で
すべて消える / 入れ替わる:

| Backend | `pre_wb` | `post_wb` |
|---|---|---|
| non-moving M&S | no-op | no-op |
| semi-space | no-op | no-op (STW) |
| generational | no-op | card mark / remset |
| realtime (SATB) | 旧値を mark queue | no-op |
| realtime (incremental update) | no-op | 新値が white なら shade |

`pre_wb` / `post_wb` は inline → no-op backend では C コンパイラが完全に消す。

#### どう書く

`@ref(value)` operand への書き込みは自動で WB を経由。 BODY 内の手書き書込みは
`WB(obj, field, val)` 経由にする:

```c
// before:
a->items[ii] = v;
// after:
WB(a, items[ii], v);
```

### 1.6 Safepoint 配置

#### 何

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
- loop back-edge (while/for) — `@safepoint` で明示指定
- call 境界の入口 (再帰許容のため)
- 例外ハンドラ境界

**CTX を取らない C helper 内での safepoint**: `baruby_ary_push(VALUE av,
VALUE x)` のように CTX を引数で受け取らない C ヘルパが内部で allocate する
場合、 そこで safepoint を打つ手段が必要。 framework が提供する
`astro_gc_alloc` / `astro_gc_realloc_payload` 等の **alloc API は内部で
safepoint poll を含む**、 を framework 規約として固定する。 これにより
helper は signature 不変のまま precise GC に対応できる。 内部の現在 CTX は
TLS 経由 (`astro_gc_current_ctx()`) で取得する想定 (single-thread 前提では
グローバル 1 個でよく、 multi-thread に拡張するときに TLS 化)。

NODE_DEF レベルの注釈:

| オプション | 意味 |
|---|---|
| `@noalloc` | BODY + 全 transitive children が allocate しないことを保証。 leaf node (literal / 変数参照系) で使う。 ASTroGen は frame ENTER/LEAVE を省略し、 safepoint も入れない |
| `@safepoint` | BODY 末尾に safepoint poll を強制挿入。 allocation を含まない長い loop back-edge で使う |

#### なぜ

`EVAL_ARG` で子を評価する非 leaf ノードは原理的に transitive に allocate しうる
ため、 **default は「allocate しうる」 保守側に倒す**。 opt-in 注釈 `@allocates`
は持たない (= 全部に書く羽目になり意味がない)。 leaf の opt-out として
`@noalloc` だけを残す。

`@noalloc` の違反は CI で検出可能 — ASTroGen が BODY を grep して
`KORB_ALLOC_*` / `EVAL_ARG` / `astro_gc_alloc` の有無を確認すれば足りる。

preemptive ではなく cooperative にする理由:

- signal-based は wasm と相性が悪い
- koruby の setjmp/longjmp 不使用方針と整合
- `@safepoint` の明示指定で「allocate しない長い loop」 にも対応可

### 1.7 Operand 注釈の拡張

#### 何

既存の `<type> <name>@ref` (struct 内 inline 格納 + hash skip) に GC 関連の
サブ注釈を足す:

| 注釈 | 意味 | 使用例 |
|---|---|---|
| `@ref` | 既存。 inline 格納 + hash skip。 **GC は touch しない** (mutable な metadata) | `struct ic *cache@ref` |
| `@ref(value)` | `@ref` かつ中身が `VALUE` (or VALUE 配列)。 marker は mark する | `VALUE last_recv@ref(value)` |
| `@imm` | この operand は `HEAP_IMMORTAL` 上の永続オブジェクトを指す。 marker は touch せず recurse のみ。 default は AST NODE \* | `NODE *body@imm` (実質 default) |
| `@weak` | 弱参照 | `VALUE key@weak` |
| `@atomic` | bytes 列、 deep mark 不要 | `const char *name@atomic` |

#### なぜ

abruby の `node_mark.c` を framework 標準化するために必要な情報粒度。
operand の意味論 (mutable な inline cache か / VALUE を含むキャッシュか /
不動 AST 参照か / 弱参照か) を declarative に書ければ、 marker は自動生成
できる。

例: `node_call_2(... struct callcache *cc@ref ...)` の `callcache` は
`{state_serial_t serial; struct Node *body;}` で、 中身は `serial` (atomic) と
`body` (AST NODE = immortal) のみ。 これは GC touch 不要なので `@ref` のまま
(中身に VALUE が混じれば `@ref(value)` に格上げ)。

#### どう書く

baruby の現状 `struct callcache *cc@ref` は注釈不要 (default `@ref` で OK)。
将来的に inline cache に `VALUE last_recv` を入れるなら `@ref(value)` に
変える。

### 1.8 Backend 切替

#### 何

```
make GC=none           # 全 hook が (void)0 / identity — 既存挙動
make GC=conservative   # libgc 保守的スキャンを framework 経由で wrapping
make GC=marksweep      # precise non-moving M&S
make GC=semispace      # copying / moving
make GC=generational   # young/old + remset
make GC=realtime       # SATB or incremental update
make GC=cruby          # CRuby host VM 委譲 (abruby / arjsv 専用)
```

#### なぜ

切替粒度は **compile-time** で固定する。 `astro_gc.h` 内の `inline` で全 hook を
確定し、 no-op はコンパイラが消す。 性能上ここがゼロコスト化の鍵で、 ランタイム
dispatch は持たない。

#### どう書く

サンプルの Makefile で `GC` 変数を取り、 対応する backend C ソースをリンク。
node.def の中身は変更不要。

---

## 2. sample/baruby に当てはめる

`sample/baruby` を題材に、 §1 の各機構を実際にどう書くかを示す。 baruby は
GC testbed として設計された最小 Ruby サブセット (Array + String + LSB-tagged
fixnum、 OO なし、 ~770 行の node.def) で、 現状は libgc 保守的スキャンで動く。

### 2.1 baruby の現状

値表現 (`context.h`):

```c
typedef intptr_t VALUE;
// LSB == 1 → fixnum、 raw == 0/2/4 → false/true/nil、 LSB == 0 (それ以外) → heap ptr

typedef struct ObjectHeader { uint32_t type; uint32_t flags; } ObjectHeader;
typedef struct BaArray   { ObjectHeader hdr; uint32_t len, capa; VALUE *items; } BaArray;
typedef struct BaString  { ObjectHeader hdr; uint32_t len, capa; char   *bytes; } BaString;
```

frame: `VALUE *fp` を `common_param_count=3` で全 NODE_DEF に第 3 引数として
渡す。 各 frame は parser が決めた連続 VALUE 配列で、 `node_scope` の
`fp + envsize` で base を進める。

allocation site (`node.c`):

- `baruby_ary_new(capa)` — `malloc(BaArray)` + `malloc(VALUE * capa)`
- `baruby_ary_push(av, x)` — 必要なら `realloc(items, capa*2)`
- `baruby_str_new(bytes, len)` — `malloc(BaString)` + `malloc(len+1)`
- `baruby_str_concat / repeat / append / to_s` — 各種 `malloc`

これらが `context.h` の `#define malloc GC_MALLOC` 経由で全部 libgc に飛ぶ。

NODE_DEF の典型:

```c
NODE_DEF
node_call_aset(CTX *c, NODE *n, VALUE *fp, NODE *recv, NODE *idx, NODE *val)
{
    VALUE r = UNWRAP(EVAL_ARG(c, recv));
    VALUE i = UNWRAP(EVAL_ARG(c, idx));
    VALUE v = UNWRAP(EVAL_ARG(c, val));
    if (IS_ARY(r)) {
        BaArray *a = VAL2ARY(r);
        intptr_t ii = VAL2INT(i);
        if (ii < 0) ii += a->len;
        if (ii < 0) return RESULT_OK(VAL_NIL);
        while ((uint32_t)ii >= a->len) baruby_ary_push(r, VAL_NIL); // ← 内部で realloc
        a->items[ii] = v;                                            // ← 素の書込み
        return RESULT_OK(v);
    }
    ...
}
```

ここで GC の観点での問題は:

1. `r`, `i`, `v` の 3 つのローカル VALUE が `baruby_ary_push` (= 潜在的に
   allocate) を **またいで生存**。 保守的 GC ならスタック上にあれば自動で
   scan されるが、 precise だと root 化が必要
2. `a->items[ii] = v` が **書き込みバリアなし**。 generational backend に
   切り替えると old → young 参照が記録されない
3. `while` 内の `baruby_ary_push` 呼び出しは allocation site の直前なので
   safepoint poll が必要

### 2.2 1 backend で十分なら何も書かなくていい

`make GC=conservative` (libgc を framework 経由で wrap) で動かす限りは
node.def に注釈を 1 つも追加しなくてよい。 これが **ゼロコスト性の gate**:
既存の baruby が 1 文字も変えずに新 framework に乗る、 を最初の milestone
にする。

### 2.3 precise backend に切り替えるための注釈

`make GC=marksweep` を有効にするときに足す注釈を §1.4-1.7 から拾うとこうなる。

#### 2.3.1 値の型宣言 (`value.def` を採用するなら)

`node.def` の冒頭に追加:

```
VALUE_DEF baruby_obj @header=ObjectHeader @kind_field=type
{
    OBJ_ARRAY   => ref_payload(VALUE) items; uint32_t len; uint32_t capa;
    OBJ_STRING  => atomic_payload(char) bytes; uint32_t len; uint32_t capa;
}
```

ASTroGen はこれから `KORB_ALLOC_OBJ_ARRAY` / `KORB_ALLOC_OBJ_STRING` /
`KORB_MARK_*` / `KORB_FORWARD_*` を生成。 `baruby_ary_new` / `baruby_str_new`
は `KORB_ALLOC_*` 経由に書き換え:

```c
VALUE baruby_ary_new(uint32_t capa) {
    BaArray *a = KORB_ALLOC_OBJ_ARRAY(capa);   // ref_payload も同時 alloc
    a->hdr.flags = 0;
    a->len = 0;
    a->capa = capa;
    return (VALUE)a;
}
```

採用しない場合は `baruby_ary_new` を従来通り書き、 mark 関数だけ自分で書く
(abruby の `node_mark.c` と同じ流儀)。

#### 2.3.2 NODE_DEF のローカル VALUE root 化

`@roots(name, ...)` を追加。 例えば `node_call_aset` は `r`, `i`, `v` が
`baruby_ary_push` をまたぐ:

```c
NODE_DEF @roots(r, v)
node_call_aset(CTX *c, NODE *n, VALUE *fp, NODE *recv, NODE *idx, NODE *val)
{
    VALUE r = UNWRAP(EVAL_ARG(c, recv));   // i / v 評価 / push 呼出を生存
    VALUE i = UNWRAP(EVAL_ARG(c, idx));    // push の前に死ぬので不要
    VALUE v = UNWRAP(EVAL_ARG(c, val));    // a->items[ii] = v 書込で生存
    if (IS_ARY(r)) {
        BaArray *a = VAL2ARY(r);
        ...
        while ((uint32_t)ii >= a->len) baruby_ary_push(r, VAL_NIL);
        WB(a, items[ii], v);               // 素の代入 → WB macro 経由
        return RESULT_OK(v);
    }
    ...
}
```

`i` は intptr_t に変換した時点で死ぬので root 不要 (どうせ fixnum なので
heap ref を含まない)。 `v` は最後の WB まで生存するので root 必須。

leaf 系には `@noalloc`:

```c
NODE_DEF @noalloc
node_num(CTX *c, NODE *n, VALUE *fp, int32_t num) { return RESULT_OK(INT2VAL(num)); }

NODE_DEF @noalloc
node_lget(CTX *c, NODE *n, VALUE *fp, uint32_t index) { return RESULT_OK(fp[index]); }

NODE_DEF @noalloc
node_true (CTX *c, NODE *n, VALUE *fp) { return RESULT_OK(VAL_TRUE); }
```

#### 2.3.3 frame 全体を root_array で宣言

baruby の `VALUE *fp` は **NODE_DEF 共通の第 3 引数** で渡される frame
pointer。 これを GC で scan するには 2 通りある:

**案 A**: CTX 上で frame chain を持ち、 frame iterator が辿る (汎用)

```c
// 言語が 1 個実装
void baruby_gc_iter_roots(astro_root_visitor_t *v) {
    for (astro_frame_t *f = c->fp_chain; f; f = f->prev) {
        astro_visit_frame(v, f->desc, f->data);
    }
}
```

CTX に `fp_chain` を追加し、 各 NODE_DEF の EVAL ラッパで自動 push/pop する
(ASTroGen が `@roots` から組み立てる)。 ただし baruby の `VALUE *fp` 自体は
node.def の operand ではなく **共通引数** なので、 これを frame iter から
直接見るには:

**案 B**: function call boundary でだけ `@root_array(fp, locals_cnt)` を宣言
(局所最適)

baruby は function call の節 (`node_call_*` の callee セットアップ部分) でしか
新しい frame を作らない (`node_scope` の `fp + envsize` も同じ親 frame の
拡張)。 つまり root_array を打つべきは **function 境界ノードだけ**:

```c
NODE_DEF @root_array(F, locals_cnt)
node_call_1(CTX *c, NODE *n, VALUE *fp, const char *name, uint32_t arg_index,
            uint32_t locals_cnt, struct callcache *cc@ref, NODE *a0)
{
    VALUE F[locals_cnt];     // VLA、 callee の新 frame
    F[0] = UNWRAP(EVAL_ARG(c, a0));
    ...
    return EVAL(c, cc->body, F);
}
```

`@root_array(F, locals_cnt)` で「`F` という `VALUE *` の `locals_cnt` 要素を
root として scan する」 を declarative に書ける。 frame_desc.ref_array の
`base_off` / `count_off` に対応。

トップレベルの起点だけは `main.c` で:

```c
// main.c の c->env を toplevel frame として登録
astro_gc_register_toplevel_frame(c->env, /*size*/ envsize);
RESULT r = EVAL(c, ast, c->env);
astro_gc_unregister_toplevel_frame();
```

#### 2.3.4 既存の `@ref` は触らなくてよい

baruby の `struct callcache *cc@ref` は中身が `{state_serial_t serial;
struct Node *body;}` で、 `body` は AST NODE = `HEAP_IMMORTAL`、 `serial` は
atomic。 GC が touch する必要なし → **default の `@ref` のまま**。

ただし `body` 経由で `OPTIMIZE` した結果に AST 以外のオブジェクト (例えば
class や Proc) が入るようになれば `@ref(value)` に変える必要が出る。 これは
将来の話。

#### 2.3.5 `baruby_ary_push` 内の realloc を safepoint 化

```c
void
baruby_ary_push(VALUE av, VALUE x)
{
    BaArray *a = VAL2ARY(av);
    if (a->len == a->capa) {
        ASTRO_SAFEPOINT(c);          // ← 追加。 alloc の直前
        uint32_t new_capa = a->capa ? a->capa * 2 : 4;
        a->items = (VALUE *)astro_gc_realloc_payload(
            astro_gc_heap(HEAP_REF_PAYLOAD), a->items,
            sizeof(VALUE) * new_capa);
        a->capa = new_capa;
    }
    WB(a, items[a->len], x);         // 素の代入 → WB
    a->len++;
}
```

ただし `baruby_ary_push` は CTX を取らないので、 `ASTRO_SAFEPOINT(c)` を
呼ぶには signature を変える必要がある。 これは **§1.6 で書き忘れていた
問題** → §1 にフィードバック (§2.4 で議論)。

### 2.4 §2 を書いて見えた、 §1 への戻り

§2 を書く過程で §1 で扱いきれていなかった 2 点が出てきた。 §1 側に反映済み:

**(1) CTX を取らない C helper 内での safepoint**: `baruby_ary_push(VALUE av,
VALUE x)` のような既存 helper の signature を変えずに precise GC に対応する
には、 framework の alloc API が **内部で safepoint poll を含む** 必要がある。
現在 CTX は TLS 経由で取得する (single-thread 前提では グローバル 1 個で
十分)。 → §1.6 末尾に追記済。

**(2) 共通引数 frame の root_array 化**: baruby の `VALUE *fp` は NODE_DEF の
ローカル変数ではなく `common_param_count=3` で渡される **共通引数**。
`@root_array(base, count)` の `base` / `count` がローカル変数だけでなく
共通引数も指せることを §1.4 で明確化。

ほかには大きな破綻は見つからなかった。 §1.1 (`value.def` 採否) は当初の
「未決」 のまま、 §1.4-1.7 だけで baruby precise 化が記述できることが
§2 で確認できた = §1.1 を入れなくても interface として閉じている、 を逆に
裏付けた形。

### 2.5 まとめ: baruby が precise backend に乗るまでに足すもの

| 場所 | 追加内容 | 規模 |
|---|---|---|
| `node.def` 冒頭 | `VALUE_DEF baruby_obj { ... }` (採用するなら) | ~5 行 |
| 各 `NODE_DEF` | leaf に `@noalloc`、 alloc/call をまたぐ root に `@roots(...)` | ~40 箇所 |
| 関数境界 ノード | `@root_array(F, locals_cnt)` | 4 箇所 (call\_0/1/2/3) |
| `node.c` | `baruby_ary_new` 等を `KORB_ALLOC_*` / framework alloc API 経由に | ~6 関数 |
| `node.c` | 素の `a->items[ii] = v` を `WB(a, items[ii], v)` に | ~10 箇所 |
| `main.c` | toplevel frame 登録 | 2 行 |
| `Makefile` | `GC=marksweep` でリンクする backend ソース指定 | 5 行 |

BODY のテキスト自体は **1 行も変更不要**。 NODE_DEF の header と `node.c`
ヘルパだけが変わる。 これが BODY 不触原則の物理的実現。

### 2.6 同じ node.def から 2 backend

ここまでの注釈は全部 `make GC=none` でも `make GC=conservative` でも
`make GC=marksweep` でも **同じソースから出る**。 backend を切り替えるのは
Makefile の 1 行のみ。

- `GC=none` — `@roots` / `@noalloc` 注釈は無視 (全 hook が `(void)0`)
- `GC=conservative` — frame_desc は生成するが scan は libgc 保守側に委譲
  (precise の正しさを少しずつ検証する移行段階で使う)
- `GC=marksweep` — 上記注釈が全部効く

これが「**1 個の node.def から 3 backend が同居する設計**」 のミニ証明になる。
moving / generational / realtime を載せるかは backend 実装の問題で、 言語側の
interface は不変。

---

## 議論ストック (要決事項 / 横展開)

### A. `value.def` 採否

§1.1 で「未決」 と明記。 折衷案として **`@ref(value)` / `@roots` / `@noalloc`
など §1.4-1.7 だけ採用、 `value.def` は当面なし** の路線を取り、 arcel + asml
で root mechanism を試した後で改めて判断する。

採用側の利得:
- abruby の `node_mark.c` を framework 標準化できる
- moving / generational の boilerplate を集約できる
- 「同じ DSL から CRuby backend と precise backend の両方の marker が出る」
  が抽象化の正しさの proof になる (§15 副産物)

不採用側の利得:
- 言語ごとの値表現の癖を DSL に押し込まない
- 実装規模が小さい (~250 行削減)
- backend 移植時の自由度が高い

### B. サンプル俯瞰 (どの backend を当てるか)

| sample | 推奨 default backend | 補助ヒープ |
|---|---|---|
| calc / naruby / pascalast / castro / aforth / wastro | `none` | — |
| asml / astocaml / ascheme | `semispace` | — |
| luastro | `marksweep` (precise) | weak ref subheap |
| asom | `generational` | escape 軽量解析 |
| baruby / koruby / pystro / jstro / astrogre | `generational` | `HEAP_IMMORTAL` (class/method/AST) |
| astr | `marksweep` + LARGE | `HEAP_LARGE` (mmap) |
| nuq | `generational` or `region` | パイプ単位 reset 候補 |
| arcel | `region` (arena 化) or `realtime` | 既存 arena を backend 化 |
| arjsv / abruby | `cruby` | host 委譲 |

### C. 展開ロードマップ

| Phase | サンプル | 検証項目 | gate |
|---|---|---|---|
| 0 | naruby, calc | API 導入のゼロコスト性 (`GC=none` で regression なし) | naruby ベンチ ±1% |
| 1 | baruby | conservative backend で wrap → precise backend に上げる | bench (sustained) |
| 2 | luastro | 既存自前 M&S を precise marker に置換 (値表現不変) | 既存テストグリーン |
| 3 | asml | semispace、 frame iterator + safepoint 本気検証 | bench |
| 4 | astrogre, asom | generational backend を 2 サンプル横展開 | regex selftest, SOM 完走 |
| 5 | koruby | optcarrot を gate に generational 化 | optcarrot 完走 ± 性能 |
| 6 | pystro, jstro | koruby と同 backend で済むことを確認 | 既存 PASS 数維持 |
| 7 | arcel | region / realtime backend の reference 実装 | 808-808 conformance + p99 |
| 8 | abruby, arjsv | CRuby backend、 `value.def` → CRuby `dmark` 変換確認 | rb_check |
| 9 | astr | LARGE_HEAP 経路で vector 系 R ベンチ | 既存テスト |

### D. 残るオープン項目

- (b) `astro_gc_alloc` 戻り値の tag → **タグなし** (本案で決定済)
- (c) JIT 生成 SD の frame descriptor → SD\_\<hash\>.c に `static const` 焼込
  (本案で決定済)
- (d) `HEAP_FINALIZABLE` semantics — BDW 風 topological がデフォルト、
  `@finalizer F @order=N` で override 可
- (e) `ref_array` 長さ表現 → `count_var` 単独 (begin/end は moving fixup が
  複雑になるので不採用)
- (f) `@linear` (nuq 線形性解析統合) → 当面保留
- (g) CTX を取らない C helper 内の safepoint → framework alloc/realloc API が
  内部で poll を含むこと (§2.4 でフィードバック済)
