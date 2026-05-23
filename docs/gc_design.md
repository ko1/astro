# ASTro GC 設計

ASTro framework が precise GC についてどんな抽象を提供し、 sample (= 言語実装)
側が何を差し込むかをまとめた設計ドキュメント。 実装は `runtime/precise_gc/`
配下に 16 backend、 reference sample は `sample/baruby_precise/`。

関連: `idea.md` §8.2、 `code_store_quirks.md` (AST NODE 不動制約)、
`sample/baruby/` (conservative libgc 比較対象)、
`sample/baruby_precise/docs/runtime.md` (実装詳細)。

---

## 0. 概要

ASTro の precise GC は **MMTk 流の VMBinding 抽象** を C macro で実現する形
を取る。 大きく 2 層に分離:

```
┌──────────────────────────────────────────────┐
│ sample (= 言語実装)                          │
│  - VALUE 表現                                │
│  - object struct (先頭に ASTroObjectHeader)  │
│  - object shape macro (SCAN_EDGES)           │
│  - root scan (= sp[] + globals 等)           │
│  - AstroGc instance の allocation + bind     │
└──────────────────────────────────────────────┘
                  ▲ contract macros
                  │ (compile-time inline)
                  ▼
┌──────────────────────────────────────────────┐
│ framework (= runtime/precise_gc/)            │
│  - GC algorithm (17 backend: copy / mark /   │
│    immix / immix_gen / copy_scramble / ...)  │
│  - mmap / region 管理                        │
│  - allocator API (aro_gc_alloc(c, size))     │
│  - ASTroObjectHeader 共通型 (gc_types.h)     │
└──────────────────────────────────────────────┘
```

framework は **object header の size と forwarding state しか見ない**。
VALUE の tagging 規約、 object の具体的 layout、 type tag、 root の在処
などは sample が contract macro で提供する。 framework は与えられた
macro を compile-time に inline 展開して動く (= MMTk の generic
monomorphization と等価)。

これにより:

- 同一 framework で異言語 sample が動く (baruby / koruby / pystro / etc)
- 同 sample 内で 16 GC backend を `-DBARUBY_GC=<n>` で切替可能
- 将来 multi-instance (= 同 process で複数 GC) 拡張も「**AstroGc** struct を
  複数 allocate するだけ」 で対応 (= framework は module-static 持たない)

### 0.1 設計の核心 4 つ

1. **統一 ASTroObjectHeader**: 全 GC object の先頭 8 B (or 16 B) に置く header。
   sample 用 `flags` (16b) と framework 用 `gc_flags` (16b) + `gc_size` (32b)
   + (mark_compact 系のみ) `gc_fwd` を持つ。 旧来の「framework GCHeader prefix
   + sample ObjectHeader」 の 2 層を 1 層に統合。
2. **contract macros**: sample が「VALUE 表現」「object shape」「root の場所」
   を C macro で提供。 framework は macro を inline 展開して使う。
3. **`struct ASTroGC`**: GC algorithm が必要とする process-scope state を
   1 つの struct にまとめる。 sample が instance を 1 つ (or 複数) 確保し、
   `aro_gc_init` で初期化。 framework module-static は **持たない**。
4. **edges 経由の object scan**: `ASTRO_GC_SCAN_EDGES(h, size, ctx, visit)`
   macro が outgoing reference を slot pointer で列挙、 mark / forward /
   update 全 phase が同 macro + 違う visit callback で済む (= MMTk 流)。

### 0.2 移行履歴 (iter 75)

- **Step A** (iter 75): `aro_gc_kind_of` dead code 削除、 backend が sample
  kind を直接認知する経路を切る前段整理
- **Step B** (iter 75): `aro_gc_alloc(c, kind, size)` から `kind` 引数を排除。
  sample が `head.flags` の type tag (e.g. `OBJ_ARRAY`) で全 dispatch を所有。
  `aro_gc_alloc_vals` / `AstroGcCategory` 等の framework-side 分類を全廃。
- **Step C** (iter 75): framework GCHeader prefix を廃止し、 統一
  `ASTroObjectHeader` を payload offset 0 に置く。 sample 構造体は
  `ASTroObjectHeader head` を先頭 field に持つ。
- **Step C+** (iter 75): Cheney 系 backend (= copy / copy_gen / copy_gen_inc /
  mark_bump_gen / immix_gen) の fwd ptr を payload[8..15] に overlay。
  ASTroObjectHeader が 16 B → 8 B に縮む。 mark_compact 系 (= phase 3 で fwd と
  sample data 両方アクセス) のみ dedicated `gc_fwd` field を残す。

---

## 1. Object graph 観点

GC は **object graph (= 頂点 = GC object、 辺 = inter-object pointer)**
しか見ない。 所有関係や semantics は知らず、 reachability で live を判定
する。

(注: ASTro の AST `Node` 型とは別概念。 本章で「object」 と呼ぶのは
`aro_gc_alloc` が返す GC-managed な連続メモリ領域のこと。)

### 1.1 1 GC object = 1 つの連続アロケーション

**用語**: 本ドキュメントでは「 **payload** 」 を **GC object のメモリ領域
全体** (= `ASTroObjectHeader head` を含む先頭から末尾まで) として一貫して
使う。 「head」 は payload offset 0 にある 8 B (non-moving) / 16 B (moving)
の framework-controlled 領域、 「post-head 領域」 は payload[8..] / payload[16..]
の sample-controlled 領域 (= iter 75 で framework GCHeader prefix 廃止、
ASTroObjectHeader を payload offset 0 に embed する設計)。

```c
void *aro_gc_alloc(CTX *c, size_t payload_size);
void *aro_gc_alloc_byte(CTX *c, size_t payload_size);   /* zero-fill 省略 */
```

`payload_size` は **head を含む** 全体サイズ (= `sizeof(struct sobj)`
そのまま)。 1 回の `aro_gc_alloc` 呼び出しが **1 つの GC object** を作る。
返り値は **payload 先頭 (= head の先頭、 sample 構造体の先頭)**。 sample
構造体は `head` を先頭 field に持ち、 残りに自身のデータを並べる:

```c
typedef struct BaArray {
    ASTroObjectHeader head;   /* byte offset  0,  size 8 B (non-moving) */
    uint32_t          len;    /* byte offset  8,  size 4 B */
    uint32_t          capa;   /* byte offset 12,  size 4 B */
    BaArrayItems     *items;  /* byte offset 16,  size 8 B */
} BaArray;                    /* total 24 B */
```

**不連続なメモリブロックは別々の GC object** にして pointer で繋ぐ。

例: baruby_precise の Array は **2 object 構造**:

```
┌─ BaArray (head.flags = OBJ_ARRAY) ──┐
│ ASTroObjectHeader head              │
│ uint32_t len                        │
│ uint32_t capa                       │
│ BaArrayItems *items ────────────────┼──┐
└─────────────────────────────────────┘  │
                                         ▼
                ┌─ BaArrayItems (head.flags = OBJ_VALUE_ARRAY) ─┐
                │ ASTroObjectHeader head                        │
                │ VALUE data[0]                                 │
                │ VALUE data[1]                                 │
                │ ...                                           │
                └───────────────────────────────────────────────┘
```

- BaArray header (= sample 視点の「Array」 入口) は **items pointer 1 個だけ持つ**
- BaArrayItems は head 直後に flex array で VALUE が並ぶ (= 全 slot が VALUE、
  scan 対象)
- GC mark で BaArray → items を辿り → 各 VALUE → 各要素 object に伝播

なぜ 2 分けるか:
- BaArray は **固定 size** (= head + len + capa + pointer)
- BaArrayItems は **可変 size** (= 要素数に依存)
- 1 object に統合すると `Array.push` の grow で外部 reference 無効化

同様に BaString は header (= head + len + capa + bytes pointer) + BaByteData
(= head + bytes[]) の 2 object 構造 (例外: SSO で小文字列だけ 1 object に inline)。

### 1.2 ASTroObjectHeader layout

`runtime/precise_gc/gc_types.h` 定義:

```c
typedef struct ASTroObjectHeader {
    uint16_t flags;        /* sample-controlled (type tag + sample bits) */
    uint16_t gc_flags;     /* framework-controlled (marked/old/dirty/fwd 等) */
    uint32_t gc_size;      /* allocation size in bytes */
#ifdef ASTRO_GC_HAS_FWD
    void    *gc_fwd;       /* mark_compact 系のみ (= phase 3 で fwd 必要) */
#endif
} ASTroObjectHeader;
```

| field | width | controller | 用途 |
|---|---|---|---|
| `flags` | 16b | sample | type tag (OBJ_ARRAY / OBJ_STRING / OBJ_VALUE_ARRAY / OBJ_BYTE_DATA 等) + sample-specific bits (SSO 等) |
| `gc_flags` | 16b | framework | mark / old / dirty / forwarded / free bits。 backend ごとに layout 異なる |
| `gc_size` | 32b | framework | sample が `aro_gc_alloc(c, sz)` で渡した size。 heap walk で次 object 位置算出 |
| `gc_fwd` | 64b | framework | mark_compact 系のみ。 forwarding pointer (= phase 2 で書き、 phase 3 / 4 で読む) |

**サイズ統計**: 14 / 16 backend が **8 B** (`ASTRO_GC_HAS_FWD` 未定義)、
mark_compact + mark_compact_gen のみ **16 B**。

Cheney 系 moving backend (copy / copy_gen / copy_gen_inc / mark_bump_gen /
immix_gen) は from-space を破棄する性質を利用して、 fwd ptr を payload
offset 8 (= 先頭 sample field の場所) に **overlay 配置** することで dedicated
field 不要に。 ASTroObjectHeader が 8 B のまま保てる:

```c
/* Cheney forward (gc_copy.c 等): */
static void *
forward_payload(ASTroGC *gc, void *old_payload)
{
    ASTroObjectHeader *oldh = (ASTroObjectHeader *)old_payload;
    if (oldh->gc_flags & HDR_FORWARDED) {
        return *(void **)((char *)oldh + sizeof(ASTroObjectHeader));  /* overlay */
    }
    /* memcpy old → new in to-space, then mark old + write overlay */
    void *new_payload = ...;
    memcpy(new_payload, old_payload, ALIGN8(oldh->gc_size));
    oldh->gc_flags |= HDR_FORWARDED;
    *(void **)((char *)oldh + sizeof(ASTroObjectHeader)) = new_payload;
    return new_payload;
}
```

**最低 payload size 制約**: overlay 方式は payload >= 16 B (= 8 B head + 8 B fwd
overlay 用) が必要。 baruby_precise の全 sample 型 (BaArray / BaString /
BaArrayItems(capa≥1) / BaByteData(len≥8)) は満たす。

mark_compact 系は **slide phase で sample data と fwd 両方を読む** ので
overlay 不可、 dedicated `gc_fwd` field が必要 (= head 16 B)。

### 1.3 gc_flags の bit layout (backend 別)

各 backend が必要とする state bit が異なるので、 layout は backend-local:

| backend | gc_flags bits | head size |
|---|---|---|
| none / bump | (なし、 GC せず) | 8 B |
| mark | MARKED / FREE | 8 B |
| mark_gen / mark_gen_inc | MARKED / OLD / DIRTY / FREE | 8 B |
| mark_freelist | MARKED / FREE | 8 B |
| mark_bitmap_gen / mark_card_gen | FREE のみ (mark/old/dirty は per-page bitmap) | 8 B |
| immix | mark_epoch (8b, low byte) | 8 B |
| immix_gen | mark_epoch (8b) + OLD / DIRTY / FORWARDED | 8 B |
| copy | FORWARDED / MARKED (large only) | 8 B |
| copy_gen / copy_gen_inc | OLD / DIRTY / FORWARDED | 8 B |
| mark_bump_gen | MARKED / OLD / DIRTY / FREE / FORWARDED | 8 B |
| mark_compact | MARKED + `gc_fwd` field | 16 B |
| mark_compact_gen | MARKED / OLD / DIRTY + `gc_fwd` field | 16 B |

`FORWARDED` bit は Cheney 系で「from-space から copy 済」 marker。
`MARKED` (large object) は copy.c で「移動しない large が live」 marker。

backend 内 file-local に `HDR_<X>` macro として定義。

---

## 2. sample が提供する contract

sample は `context.h` に以下を define する。 framework は macro を
compile-time inline 展開して使う。

### 2.1 VALUE 表現

```c
typedef intptr_t VALUE;   /* sample 固有の値表現 (LSB tag / NaN-box / etc) */

#define IS_PTR(v)   /* heap pointer (= 8-aligned non-singleton) 判定 */
```

framework は VALUE の tagging 戦略を知らない。 sample が `IS_PTR` で
「heap pointer かどうか」 を判定する macro を提供。 GC scan の visit
callback は `IS_PTR` 通過した slot のみ heap として扱う。

VALUE → ASTroObjectHeader* の変換は **plain cast** (= header は payload
offset 0 にいるので、 VALUE をそのまま `(ASTroObjectHeader *)v` できる)。

### 2.2 Object shape

```c
/* Outgoing reference の列挙。 visit callback は slot pointer を受ける
 * (= mark phase なら read のみ、 forward phase なら read + write back)。
 * sample が head.flags の type tag で switch して、 各 type の edges を
 * visit する。 */
#define ASTRO_GC_SCAN_EDGES(payload, payload_size, ctx, edge_visit) do {  \
    ASTroObjectHeader *_h = (ASTroObjectHeader *)(payload);                \
    switch (_h->flags & OBJ_TYPE_MASK) {                                   \
      case OBJ_ARRAY: {                                                    \
          BaArray *_a = (BaArray *)(payload);                              \
          edge_visit((ctx), (void **)&_a->items);                          \
          break;                                                            \
      }                                                                     \
      case OBJ_STRING: { /* ... */ break; }                                \
      case OBJ_VALUE_ARRAY: { /* iterate VALUE slots */ break; }           \
      case OBJ_BYTE_DATA: break;  /* raw bytes — no edges */               \
    }                                                                       \
} while (0)

/* 新 payload を scan-safe な値で初期化 (alloc 直後 GC が走っても安全)。
 * head は backend が init するので、 sample 側は post-head 領域のみ zero。 */
#define ASTRO_GC_INIT_PAYLOAD(payload, size_bytes)                          \
    memset((char *)(payload) + sizeof(ASTroObjectHeader), 0,                 \
           (size_bytes) - sizeof(ASTroObjectHeader))

/* byte payload (= scan 対象外) の初期化、 通常 no-op */
#define ASTRO_GC_INIT_BYTE_PAYLOAD(payload, size_bytes) ((void)0)
```

`SCAN_EDGES` の visit callback signature は `void (void *ctx, void **slot)`。
slot pointer なので visit 内部で deref / 書き換え両方できる。 これにより:

- **mark phase**: `mark_edge` は `*slot` を読んで grey push (slot は変えない)
- **forward phase** (moving): `forward_edge` は `*slot = forward(*slot)` で書き換え

3 phase とも **同じ SCAN_EDGES + 違う visit** で済む (= MMTk 流)。

`INIT_PAYLOAD` は言語の VALUE 規約に依存:
- baruby (`VAL_FALSE == 0`): `memset 0` で OK
- NaN-boxing: 0 が valid double だと scan 誤判定の危険 → `NIL_VALUE` で fill
- sample が用途に合った fill 方法を提供

### 2.3 Root scan

framework の `aro_gc_alloc` 内で GC が trigger される際、 caller が `c->sp`
を「現スレッドの scan 上限」 として set してから alloc を呼ぶ約束。
backend の root scan は `c->env..c->sp` を iterate する:

```c
for (VALUE *p = c->env; p < c->sp; p++) {
    mark_value(gc, *p);   /* IS_PTR チェック後に visit */
}
```

multi-thread 対応はこの walk 部分で thread list を iterate するだけ —
framework に手は入らない。

### 2.4 Instance accessor

#### 「instance」 とは

GC algorithm 実装 1 つに付随する **mutable state の独立した束** を「instance」
と呼ぶ。 具体例:

- semispace GC なら: from-space / to-space pointer + active 切替フラグ +
  stats + stress フラグ + ... の全部を 1 つにまとめた集合
- mark&sweep なら: page chain + freelist + gray queue + remset + ...

「1 instance == 1 collector の単一実体」 と読み替えてよい。 process 全体で
1 instance しか動かない場合 (= 今の baruby_precise) は単純に一意の state
セットが 1 つあるだけだが、 「instance」 という抽象を最初から立てるのは
multi-instance への拡張余地を framework 側で塞がないため。

#### CTX → instance の bind

```c
/* CTX → ASTroGC * (= GC instance pointer) */
#define ASTRO_GC_INSTANCE(c) ((c)->astro_gc)
```

`aro_gc_init(c)` が `c->astro_gc = calloc(sizeof(ASTroGC))` で 1 instance
を heap alloc して bind する。 backend 内の helper はすべて
`ASTroGC *gc = ASTRO_GC_INSTANCE(c)` (or `ASTroGC *gc` を引数受け取り)
して `gc->field` でアクセスする。 module-static な `g_astro_gc` を持たない
ので、 別 instance を別 CTX に bind しても干渉しない。

#### 「共通ヘッダ」 contract

stats / stress / timer の保管場所は backend 横断で gc.h 側からも触りたい
が、 各 backend の `ASTroGC` の中身は backend ごとに違う。 そこで:

- gc_types.h で `AroGcCommonState` 型 (stats + stress + time_depth + time_t0) を
  定義
- 各 backend の `struct ASTroGC` の **先頭 field** に `AroGcCommonState common`
  を置く約束

これで gc.h の inline helper (stat reader / timer) は
`(AroGcCommonState *)c->astro_gc` で安全に共通部分へアクセスできる (= C の
「first member のアドレスは struct のアドレスと一致」 の保証を使う)。

### 2.5 contract macros 一覧

| 分類 | macro / typedef | 役割 |
|---|---|---|
| **A. VALUE** | `VALUE` typedef | sample の値表現 |
| | `IS_PTR(v)` | heap pointer 判定 |
| **B. Shape** | `ASTRO_GC_SCAN_EDGES(payload, size, ctx, visit)` | children traversal |
| | `ASTRO_GC_INIT_PAYLOAD(p, n)` | scan-safe init (post-head 領域のみ) |
| | `ASTRO_GC_INIT_BYTE_PAYLOAD(p, n)` | byte init (通常 skip) |
| **C. Object header** | `ASTroObjectHeader` (gc_types.h) | sample 構造体先頭 field |
| | `head.flags`, `head.gc_flags`, `head.gc_size` | accessor は plain field アクセス |
| **D. Instance** | `ASTRO_GC_INSTANCE(c)` | CTX → ASTroGC * |
| **E. Common state** | `ASTRO_GC_COMMON(c)` | stats / stress / timer accessor |
| **F. Algorithm** | `-DBARUBY_GC=<n>` | backend 選択 |

---

## 3. framework が提供する API

framework は state-less (= module-static 持たない)。 全 state は `CTX *c` 引数
+ `c->astro_gc` 経由でアクセス。

```c
/* 初期化 */
void  aro_gc_init(CTX *c);

/* allocation (sample 構造体の sizeof をそのまま渡す)
 * 返り値は payload 先頭 (= ASTroObjectHeader head の位置)。
 * caller は head.flags に type tag を書き込んでから次の alloc に進む。 */
void *aro_gc_alloc(CTX *c, size_t payload_size);

/* byte payload (post-head zero-fill を省略) */
void *aro_gc_alloc_byte(CTX *c, size_t payload_size);

/* resize (scan-safe init: 増分は zero-fill) */
void *aro_gc_realloc_payload(CTX *c, void *old, size_t new_size);
/* resize (byte: 増分 init 省略) */
void *aro_gc_realloc_byte_payload(CTX *c, void *old, size_t new_size);

/* write barrier — caller writes via aro_gc_wb / aro_gc_wb_bulk for any
 * heap-pointer assignment.  Implementation depends on backend (see §3.2). */
void  aro_gc_wb     (CTX *c, void *holder, VALUE *slot, VALUE v);
void  aro_gc_wb_bulk(CTX *c, void *holder, VALUE *dst, const VALUE *src, size_t n);

/* size accessor (sample / framework 共通) */
size_t aro_gc_size_of(void *payload);

/* 明示 collect (主に test / stress 用) */
void  aro_gc_collect(CTX *c);

/* fini */
void  aro_gc_fini(CTX *c);
```

**c->sp 規約**: 各 API は内部で GC を発火し得る。 caller は alloc 直前に
`c->sp` を「自分が握ってる scratch top」 に set する責任を持つ。 backend は
`c->env..c->sp` を root scan 範囲として参照する。

### 3.1 Write barrier の inline pattern

`aro_gc_wb` は **3 つの実装パターン**を取る:

| パターン | 該当 backend | 形 |
|---|---|---|
| **collapse to store** | non-WB (none, bump, mark, copy, mark_compact, mark_freelist, immix) | `static inline { *slot = v; }` (= cost 0) |
| **inline fast-path + cold remember** | bit-in-head gen (mark_gen / copy_gen / copy_gen_inc / mark_compact_gen / mark_bump_gen / immix_gen) | `static inline { *slot=v; if (!(old && !dirty)) return; aro_gc_remember(c, h); }` |
| **fully extern** | bitmap-based / SATB (mark_bitmap_gen / mark_card_gen / mark_gen_inc) | `extern void aro_gc_wb(...)` (LTO 任せの inline) |

bit-in-head 系の fast path は `gc_types.h` の `ASTRO_GC_WB_OLD_MASK` /
`ASTRO_GC_WB_DIRTY_MASK` を使って:

```c
/* gc.h — inline fast path: */
static inline void
aro_gc_wb(CTX *c, void *holder, VALUE *slot, VALUE v)
{
    *slot = v;
    if (__builtin_expect(holder == NULL, 1)) return;
    ASTroObjectHeader *h = (ASTroObjectHeader *)holder;
    if (__builtin_expect(
        (h->gc_flags & (ASTRO_GC_WB_OLD_MASK | ASTRO_GC_WB_DIRTY_MASK))
            != ASTRO_GC_WB_OLD_MASK, 1)) return;
    aro_gc_remember(c, h);   /* ← WB 本体: cold + noinline */
}
```

cold 関数 `aro_gc_remember(c, h)` は各 backend の .c に置き、 `DIRTY` bit
set + remset push のみ実施。 disassembly で `baruby_ary_push` 等の hot
write 周辺に `addr32 call aro_gc_remember` が分散配置され、 cold 部分
は `.cold` section に move される (= i-cache 配置最適化)。

### 3.2 ASTroGC struct

backend ごとに「algorithm が必要とする process-scope state」 が違うので、
`ASTroGC` struct の中身は backend 依存:

```c
/* gc_copy.c 内 */
typedef struct ASTroGC {
    AroGcCommonState common;          /* MUST be first field */
    char *active_base, *active_top, *active_end;
    char *space0, *space1;
    int   active_idx;
    size_t bytes_since_gc;
    size_t gc_threshold;
    CTX *ctx;
    /* Cheney scratch */
    char *to_top, *to_base, *from_base_cur;
    VALUE *sp_high_water;
    LargeObj *large_head, *large_gray;
} ASTroGC;

/* gc_mark.c 内 (別 backend なので異なる) */
typedef struct ASTroGC {
    AroGcCommonState common;
    Page     *page_head[NUM_SIZE_CLASSES];
    FreeSlot *freelist[NUM_SIZE_CLASSES];
    LargeObj *large_head;
    size_t bytes_since_gc;
    /* ... */
} ASTroGC;
```

multi-instance シナリオは「異なる ASTroGC を別 CTX に bind する」 だけで
成立。

### 3.3 copy_scramble — XOR scramble audit backend

`copy_scramble` は mark/move 漏れの検出を目的とした debug backend。 構造は
plain copy (= Cheney semispace) と同一だが、 sample-visible な VALUE
storage を per-GC-cycle の乱数 `R` で XOR scramble する:

```
encoded_value = real_ptr ^ R     /* GC が slot に書く */
real_ptr      = encoded_value ^ R /* sample が ARO_OBJ() で復号 */
```

GC cycle ごとに `R` を rotate。 GC が見逃した slot は old-R 編成のまま残
り、 次回 sample 読み込み時に new-R で復号 → garbage addr → SEGV。

##### macro (gc.h)
- `ARO_OBJ(c, v)` = `(v ^ R)` — VALUE → ptr 復号 (sample-side deref)
- `ARO_VAL(c, raw)` = `(raw ^ R)` — raw ptr → VALUE 符号化 (sample-side store)
- 非 scramble backend では identity (= no-op)

##### root scan + edge visit
- `ASTRO_GC_VISIT_EDGE_PTR(ctx, fn, slot)` — raw typed-ptr slot (= 不変)
- `ASTRO_GC_VISIT_EDGE_VAL(ctx, fn, slot)` — VALUE slot (= scramble 時に
  `scramble_R_old` で decode → forward → `scramble_R` で re-encode)

##### audit knobs (= 直交する 2 つの env var)

iter 76 で `STRESS` と `PURGE` を分離 (= 旧 `STRESS` = 両方の役を兼ねていた):

| env var | 効果 | cost |
|---|---|---|
| `BARUBY_GC_STRESS=1` | GC trigger 点ごとに必ず GC 発火 (= threshold=0)。 from-space は再利用 (= space0/space1 alternation 維持) | 重い (= GC per alloc) |
| `BARUBY_GC_PURGE=1` | Cheney 系 backend (gc_copy / gc_copy_scramble) で from-space を munmap し to-space を fresh mmap (= stale heap ptr の deref が即 SEGV) | 中 (= per-GC mmap) |
| 両方 | 高頻度 GC + 確実 SEGV = 最強 audit (= 旧 STRESS と同等) | 最重 |
| なし | 通常実行、 GC 頻度は heap pressure 次第 | なし |

```
$ make GC=copy_scramble                                  # R rotate per GC のみ
$ BARUBY_GC_PURGE=1 ./baruby_precise script.rb            # 普通頻度 + from-space SEGV
$ BARUBY_GC_STRESS=1 ./baruby_precise script.rb           # 高頻度 GC、 再利用 (= 軽量 audit)
$ BARUBY_GC_STRESS=1 BARUBY_GC_PURGE=1 ./baruby_precise script.rb  # 最強 (= 旧 STRESS 相当)
```

PURGE は `gc_copy` / `gc_copy_scramble` のみ実効 (= from-space を持つ Cheney
backend のみ)。 他 backend では env を parse して flag は立てるが no-op。

##### 検出範囲
- (1) Root / AROH_SCAN_EDGES の visit 漏れ → 該当 slot が old-R のまま →
      sample read で SEGV (= scramble の R rotation 検出機構)
- (2) Sample 側 raw cast (= `ARO_LOAD` 忘れ) → scrambled bits をそのまま
      deref → SEGV (= scramble の構造強制)
- (3) PURGE 併用時: stale ptr が from-space 領域を指せば munmap 範囲 →
      確実 SEGV (= scramble の probabilistic 検出を deterministic に)

---

## 4. Sample 実装の規約 (Moving GC 必須二大パターン)

moving GC 系 backend (= copy / mark_compact / immix_gen compact phase) を使う
sample は、 NODE_DEF / C helper コードに **以下二大ルール** を守る必要がある。
mark&sweep だけで動かしてた時代には潜伏してた precise rooting バグが
moving に切替えた瞬間に一斉に表面化するので、 移行時に必須:

### (A) sp[] spill

**heap VALUE を 「GC が起こり得る処理」 を跨いで保持する場合、 C local
ではなく sp[] slot に置く**。

「GC が起こり得る処理」 とは: 子ノードの EVAL (= 子の中で alloc が
発生し得る) はもちろん、 ary_push / str_concat 等の helper、 さらには
明示的 `aro_gc_collect()` 呼び出しも含まれる。 つまり 「alloc を内部に
含む可能性のある関数呼び出し全般」。

そのような呼び出しの GC が走ると sp[] は in-place forward されるが、
C local 変数 (= register or stack) は更新されない:

```c
/* 悪い例: GC が走ると l が stale pointer になる */
NODE_DEF
node_add(CTX *c, NODE *n, VALUE *sp, NODE *lv, NODE *rv)
{
    VALUE l = EVAL_ARG(c, lv);     /* 子 eval が GC を発火するかも */
    VALUE r = EVAL_ARG(c, rv);     /* このとき l は古いアドレスを持ったまま */
    return RESULT_OK(l + r);
}

/* 良い例: l を sp[0] に spill */
NODE_DEF
node_add(CTX *c, NODE *n, VALUE *sp, VALUE lv@child, VALUE rv@child)
{
    /* @child operand は framework が sp[0..N-1] に snapshot してから渡す */
    return RESULT_OK(lv + rv);
}
```

ASTroGen の `@child` operand 機構が spill を自動化するので、 大半の NODE
は単に `@child` を付ければよい。

### (B) helper は VALUE * で受ける

**内部で alloc する helper は VALUE を値で受け取らず caller の sp[] slot へ
の pointer で受ける**。 alloc 後に `*ref` を再 deref して post-GC アドレスを
取り直す:

```c
/* 悪い例: source bytes が GC で move された時点で stale */
VALUE
baruby_str_concat(CTX *c, VALUE lhs, VALUE rhs) {
    BaString *l = VAL2STR(lhs);  /* GC 前のアドレス */
    /* ここで alloc → GC → l が stale */
    VALUE result = (VALUE)aro_gc_alloc(c, sizeof(BaString));
    BaString *r = VAL2STR(result);
    memcpy(r->bytes->data, l->bytes->data, l->len);  /* stale pointer access */
    /* ... */
}

/* 良い例: VALUE* で受けて post-alloc に再 deref */
VALUE
baruby_str_concat(CTX *c, VALUE *lhs_ref, VALUE *rhs_ref) {
    /* lhs_ref / rhs_ref は sp[] slot を指す。 GC 後も slot 経由で
     * 最新アドレスが取れる */
    VALUE result = (VALUE)aro_gc_alloc(c, sizeof(BaString));
    BaString *l = VAL2STR(*lhs_ref);   /* GC 後の正しいアドレス */
    /* ... */
}
```

このルールは baruby_precise で 5+ 回 rooting バグが発生して確立。 詳細
は [sample/baruby_precise/docs/runtime.md §5.7](../sample/baruby_precise/docs/runtime.md)。

### 副次的な注意点

- **stress mode**: `BARUBY_GC_STRESS=1` で毎 alloc に GC 発火。 stale pointer
  access が即 SIGSEGV になりやすく、 ルール遵守を強制検証する debug 手段。
- **AOT SD**: framework-generated SD chain でも (A)(B) は守られている (= ASTroGen
  が `@child` snapshot を sp[] に出力)。 手書き node_slowpath.c で抜けがちなので
  注意。

---

## 5. 試作: `sample/baruby_precise`

### 5.1 概要

`sample/baruby_precise/` は precise *moving* GC の testbed。 仕様は baruby と
同じ Ruby サブセット (= naruby fork 由来 + Array / String + LSB tag VALUE)。
GC backend を 17 個切替可能 (`make GC=<name>`):

```
copy, copy_gen, copy_gen_inc (= clone of copy_gen),
mark, mark_gen, mark_gen_inc,
mark_compact, mark_compact_gen,
mark_bump_gen, bump,
immix, immix_gen,
mark_bitmap_gen, mark_card_gen, mark_freelist, none,
copy_scramble  /* audit backend (= XOR scramble), §3.3 */
```

35 bench × 16 backend のマトリクス検証で動作確認 (`bench/matrix.rb`)。
詳細 perf は [sample/baruby_precise/docs/perf.md](../sample/baruby_precise/docs/perf.md)。

### 5.2 sample object 型

```c
/* sample/baruby_precise/context.h */

enum obj_type {
    OBJ_ARRAY       = 1,
    OBJ_STRING      = 2,
    OBJ_VALUE_ARRAY = 3,   /* BaArrayItems */
    OBJ_BYTE_DATA   = 4,   /* BaByteData */
};
#define OBJ_TYPE_MASK  0x07u    /* head.flags 低 3 bits */
#define OBJ_FLAG_SSO   0x08u    /* head.flags bit 3: BaString SSO */

typedef struct BaArrayItems {
    ASTroObjectHeader head;
    VALUE             data[];    /* flex array */
} BaArrayItems;

typedef struct BaByteData {
    ASTroObjectHeader head;
    char              data[];
} BaByteData;

typedef struct BaArray {
    ASTroObjectHeader head;
    uint32_t          len, capa;
    BaArrayItems     *items;
} BaArray;

typedef struct BaString {
    ASTroObjectHeader head;
    uint32_t          len, capa;
    union {
        BaByteData *bytes;       /* heap-allocated */
        char        small[8];    /* SSO inline */
    };
} BaString;
```

### 5.3 サイズ比較 (iter 75 前後)

gc_copy backend (= 16 B GCHeader prefix だった moving 系) での 1 BaArray
あたりサイズ:

| 設計 | overhead | BaArray | 削減率 |
|---|---|---|---|
| iter 74 まで | 16 B framework GCHeader prefix + 8 B sample ObjectHeader | 16 + 24 = **40 B** | (baseline) |
| iter 75 Step C (head 16 B) | 16 B ASTroObjectHeader (dedicated gc_fwd) | 32 B | −20% |
| iter 75 Step C+ (overlay) | 8 B ASTroObjectHeader (fwd overlay) | **24 B** | **−40%** |

mark / mark_gen 系 (= non-moving、 元から 8 B GCHeader) は overhead が
8 + 8 = 16 B → 8 B に縮む (−50%)。

### 5.4 256/256 verification

各 backend × 8 test (T_array / T_string / T_methods / ...) × { plain, stress }
の **256/256 PASS** を維持。 AOT bake (= code_store 経由 SD compile + dlopen)
も全 backend × stress mode で動作確認済み。

---

## 6. 移行計画 (= 完了状況)

### ✅ Step 1: contract 抽象化 PoC (iter 62)

gc_copy.c で `struct ASTroGC` + `ASTRO_GC_SCAN_EDGES` macro 化を実証。
他 backend は無修正のまま動作継続。

### ✅ Step 2: 全 backend を contract に port (iter 63-73)

15 個の他 backend を gc_copy.c と同パターンで refactor。 各 backend 単独で
oracle pass を確認しつつ port。

### ✅ Step 3: `runtime/precise_gc/` 切り出し (iter 74)

`gc_<algo>.c` 群を `sample/baruby_precise/` から `runtime/precise_gc/` に
移動。 `context.h` の contract macros を sample side に集約。

```
runtime/precise_gc/
  gc_types.h            # ASTroObjectHeader + AroGcCommonState + AroGcStats
  gc.h                  # public API (aro_gc_alloc / wb / collect / ...)
  gc_common.c           # default realloc_payload (header-aware)
  gc_inplace_mremap.h   # LargeObj realloc(3) / mremap(2) template
  gc_bump.c             # no-GC baseline
  gc_none.c             # libc malloc
  gc_copy.c             # Cheney semispace
  gc_copy_gen.c         # gen Cheney
  gc_copy_gen_inc.c     # placeholder (= copy_gen の clone)
  gc_mark.c             # mark+sweep
  gc_mark_gen.c         # gen mark+sweep
  gc_mark_gen_inc.c     # SATB infra
  gc_mark_compact.c     # mark+compact
  gc_mark_compact_gen.c # gen + compact tenured
  gc_mark_bump_gen.c    # gen + bump tenured
  gc_mark_bitmap_gen.c  # gen + page bitmap
  gc_mark_card_gen.c    # gen + page-level remset
  gc_mark_freelist.c    # freelist mark+sweep
  gc_immix.c            # Immix
  gc_immix_gen.c        # gen Immix

sample/baruby_precise/
  context.h             # contract macros (ASTRO_GC_SCAN_EDGES, ObjectType)
                        #   + sample struct (BaArray / BaString / ...)
  Makefile              # $(RUNTIME)/precise_gc/gc_$(GC).c を build に組み込む
  node.h                # #include "precise_gc/gc.h"
```

実装は **直接 `gc.c` umbrella を持たず**、 各 backend `.c` をそのまま
build に組み込む形 (= Makefile が `GC_SRC := $(PRECISE_GC_DIR)/gc_$(GC).c`
で選択)。 共通 utility (`aro_gc_realloc_payload` の default 実装) は
`gc_common.c` に集約。

### ✅ Step B (iter 75): framework から kind 排除

旧 `aro_gc_alloc(c, kind, size)` の `kind` 引数を排除。 sample が
`head.flags` の type tag で全 dispatch を所有。 framework は category
(SCAN / BYTE / FREE) すら知らず、 SCAN_EDGES の callback dispatch で完結。

### ✅ Step C (iter 75): 統一 ASTroObjectHeader

framework GCHeader prefix 廃止 → `ASTroObjectHeader` を payload offset 0
に置く統一 header に。 sample 構造体は `head` を先頭 field に持つ。
gc_types.h を新設し、 sample の context.h が directly include。

### ✅ Step C+ (iter 75): Cheney 系 fwd overlay

`ASTRO_GC_HAS_FWD` を mark_compact / mark_compact_gen のみに限定。 Cheney
系 5 backend で fwd ptr を payload[8..15] overlay 配置に変更。 14 / 16
backend で ASTroObjectHeader = **8 B**。

### Step 4 (未着手): 他 sample で採用

`koruby` / `pystro` / `abruby` 等で `runtime/precise_gc/` を使ってみる。
sample-specific な部分 (= VALUE 表現、 root layout、 object shape) を contract
で表現できるかを検証。 不足する macro があれば framework 側に追加。

各 sample で利用可能になれば、 各 sample が自前で書いてた libgc 経由 alloc /
mark&sweep ロジックを framework に統一できる。

---

## 7. 残課題

### 7.1 multi-thread 対応

現状 single-thread 前提。 parallel mutator を入れるなら:

- **TLAB (Thread-Local Allocation Buffer)**: per-thread bump area で lock
  contention 回避。 framework に optional な thread-local field を追加。
- **STW (Stop-the-World)**: GC 中に mutator を止める仕組み。 safepoint
  protocol を framework に入れる。
- **concurrent / incremental**: SATB or incremental update barrier。

これらは ASTro が parallel execution を採用する段で contract 拡張する。
TLAB は contract に optional macro として後付け可能、 STW は framework
内部実装で吸収できる見込み。

### 7.2 walker (sp_offset bake) の framework 化

`sample/baruby_precise/baruby_parse.c::walk_bake_sp_offset` は hand-write
で全 NODE kind を列挙 (~200 行)。 HASH 系と同じ仕組みで astrogen.rb に
per-kind child-walk callback の gen task を追加して自動生成に畳む。

これは GC とは独立した別 todo (iter 61 fp 削除に付随)。

### 7.3 `VALUE_DEF` 採否

ASTroGen に「`VALUE_DEF` で値の構造宣言」 を追加すれば、 contract macro
(特に SCAN_EDGES) を自動生成できる可能性。 ただし baruby_precise の現状
manual macro でも実用可能性が確認できたので、 価値判断は他 sample が
contract を採用する段で再評価。

### 7.4 finalizer / weak ref

ASTro が finalizable object や weak reference を必要とするかは現状未定。
必要になったら contract に `ASTRO_GC_FINALIZE_OBJECT` / `ASTRO_GC_WEAK_*`
を追加。 BDW 風の topological finalization が default 想定。

### 7.5 backend-local `HDR_*` macro の統一

各 backend が file-local に `HDR_MARKED` / `HDR_OLD` / `HDR_FORWARDED` 等を
define してる (= 旧 GCHeader 由来の HDR_ prefix)。 file-local なので衝突は
ないが、 命名が統一されておらず読みづらい。 将来 `GCF_*` (gc_flags) など
にまとめる余地。 cosmetic only。

### 7.6 fwd overlay 共通化

Cheney 系 5 backend が `fwd_overlay_get/set` inline helper を重複定義
(8 行 × 5 = 40 行)。 gc_types.h に集約する余地。 cosmetic only。

---

## 7.7 conservative GC からの migration: 教訓

`sample/ascheme_precise` (= ascheme の Boehm libgc → precise GC framework
migration) で得た知見。 同様の migration をやる sample 開発者向け。

### 落とし穴

conservative GC (libgc) で書かれた既存コードは **C local の VALUE を root
として暗黙に依存** している。 patterns:

```c
/* (1) C local が VALUE を保持 + alloc 跨ぎ */
VALUE v = read_form(c, &r);
*tail = scm_cons(c, v, SCM_NIL);  /* alloc → v が moving GC で迷子 */

/* (2) heap interior pointer + alloc 跨ぎ */
VALUE *tail = &SCM_PTR(*tail)->pair.cdr;   /* heap obj 内部を指す */
*tail = scm_cons(c, v, SCM_NIL);            /* alloc → tail が指す obj が move → stale */

/* (3) host-allocated buffer が GC heap 経由 */
char *src = (char *)aro_gc_alloc_byte(c, sz);  /* root に登録してない */
parse(src);                                      /* alloc → src の指す buffer が sweep */
```

libgc は C stack を保守的に scan するので (1)(2) は問題なし。 (3) は libgc
が heap chunk を scan するので host malloc 経由なら問題なし。

precise GC では:
- (1) → sp[k] に park し reload
- (2) → cons cell 自身を sp[k] に park、 update は cell の field access 経由
- (3) → libc malloc (= GC heap 外) で確保

### 検証戦略: stress + copy_scramble を Phase 1 から必須に

migration の各 phase で **「test 全 PASS」だけ**を成功判定にすると **gentle
testing** で gap が露呈せず、 後で massive な debug loop が必要になる
(= ascheme_precise でこれを経験)。

**Phase 1 から `BARUBY_GC_STRESS=1 GC=copy_scramble` で全 test を回す** こと:
- `stress` = GC trigger point ごとに必ず GC 発火 (= gap 露呈確率 max)
- `copy_scramble` = VALUE storage を XOR scramble、 stale ptr deref で即 SEGV
- 組合せ = root tracking gap を **即座に SEGV** で検出

普通の test (= GC 低頻度) では「動いてる」 状態のまま gap が隠れる。

### 推奨 migration 工程

```
Phase 1. ASTroObjectHeader 追加 + GC_malloc → aro_gc_alloc
         → stress + copy_scramble で 1 test PASS まで

Phase 2. precise root tracking (sp[] / sframe / VISIT_ROOTS)
         → stress + copy_scramble で全 test PASS

Phase 3. SCAN_EDGES (各 obj 型)
         → stress + copy_scramble で全 test PASS

Phase 4. 特殊機構 (call/cc / continuation / etc.)
         → stress + copy_scramble で全 test PASS

Phase 5. 外部 resource (GMP / FILE * / etc.)
         → finalizer hook + aro_gc_account_external、 stress で leak 検証

Phase 6. 全 17 backend で stress + 通常 mode 両方 PASS
```

各 phase で stress + scramble PASS を必須にする。 これを破ると後段で
返ってくる。

### finalizer / external resource 注意

GMP buffer のような外部 libc-malloc'd memory は:
1. libc malloc (= GC heap 外) で確保
2. owning obj (= OBJ_BIGNUM) を `aro_gc_finalize_register` で登録
3. `ASTRO_GC_FINALIZE` macro で sweep 時に free
4. `aro_gc_account_external(c, +sz / -sz)` で framework に外部 memory pressure
   を通知 (= さもないと bignum-heavy code で GC trigger せず leak)

GC heap 内 (= aro_gc_alloc_byte) に external resource を置くと、 owning
obj から SCAN_EDGES edge がないので sweep で free され、 owning obj の
内部 ptr が stale 化。 必ず外部 (= libc malloc)。

---

## 8. 参考

- [MMTk](https://www.mmtk.io/) — Memory Management Toolkit (Rust)。 ASTro の
  contract macro 抽象は MMTk の VMBinding trait と概念的に同じ (= callback
  semantics + compile-time monomorphization、 C では macro/inline で代替)
- [HotSpot Klass](https://wiki.openjdk.org/display/HotSpot/Class+Loading) —
  oop offset table の layout-table 派の例
- [sample/baruby_precise/docs/runtime.md](../sample/baruby_precise/docs/runtime.md)
  — semi-space (Cheney) GC の実装詳細、 §5.7 に必須二大パターンの code 例
- [sample/baruby_precise/docs/perf.md](../sample/baruby_precise/docs/perf.md)
  — 16 backend × 35 bench の matrix 実測
- [sample/baruby_precise/docs/todo.md](../sample/baruby_precise/docs/todo.md)
  — 個別 backend 改善の todo
