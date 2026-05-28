# Precise GC migration quickstart

ある sample (= 例: libgc / leak-on-exit) を ASTro precise GC framework に
移行する手順を、 baruby_precise / ascheme_precise / koruby_precise の 3 つの
実 migration から抽出した cookbook。 詳細設計は `docs/gc_design.md`、 audit 機構の
理屈は `docs/gc_design.md §3.3` を参照。

## 全体像

precise GC への移行は **6 phase** に分けて漸進的に進めるのが安全 (= 各 phase
を audit run で抜けてから次へ):

| Phase | やること | 完了基準 |
|---|---|---|
| 1 | fork + structural migration | build pass + basic smoke |
| 2 | precise root tracking (AROH_VISIT_ROOTS) | audit run で root 漏れ catch なし |
| 3 | SCAN_EDGES per obj type | audit run で visit 漏れ catch なし |
| 4 | 特殊機構 (fiber / closure / class) | 関連 test pass |
| 5 | external resource (GMP / FILE * etc.) | finalizer 経路で leak / dangling なし |
| 6 | 全 backend × audit 検証 | 15 backend × ARO_GC_WB_AUDIT × STRESS × PURGE 全 pass |

## Phase 1: fork + structural migration

### 1.1 sample を fork

```sh
cp -r sample/baruby sample/baruby_precise        # 既存 conservative sample から
cd sample/baruby_precise
rm -rf .built_gc code_store binaries gcov_data    # 古い build artifacts 掃除
```

### 1.2 Makefile 改造

baruby_precise / ascheme_precise / koruby_precise の Makefile が参考。 必要な変更:

```makefile
# GC backend selector (= 15 種類から選択、 audit run 用 ARO_GC_WB_AUDIT もここ)
GC ?= copy
GC_NUM_none := 1
GC_NUM_mark := 2
# ... (全 15 backend を enum)
GC_NUM := $(GC_NUM_$(GC))

# precise_gc 関連 source
PRECISE_GC_DIR := $(RUNTIME)/precise_gc
GC_SRC := $(PRECISE_GC_DIR)/gc_$(GC).c
GC_COMMON_SRC := $(PRECISE_GC_DIR)/gc_common.c

# CFLAGS
CFLAGS += -I$(RUNTIME) -DBARUBY_GC=$(GC_NUM)
CFLAGS += $(if $(ARO_GC_WB_AUDIT),-DARO_GC_WB_AUDIT -Werror=discarded-qualifiers -Werror=cast-qual)

# Backend toggle rebuild marker (= GC=foo → GC=bar で必ず rebuild)
GC_VARIANT := $(GC)$(if $(ARO_GC_WB_AUDIT),-audit)
GC_MARKER := $(shell test -f .built_gc && cat .built_gc 2>/dev/null)
ifneq ($(GC_MARKER),$(GC_VARIANT))
$(shell echo "$(GC_VARIANT)" > .built_gc)
endif

# 既存 libgc を削除、 precise_gc を SRCS に追加
SRCS = main.c ... $(GC_COMMON_SRC) $(GC_SRC)
LIBS = ... # -lgc を削除
```

### 1.3 context.h に AroObjectHeader を統合

heap object base struct (= 例: `RBasic`、 `sobj` 等) の先頭 field を
`AroObjectHeader head` に変更:

```c
// context.h (sample 側)
#include "precise_gc/gc_types.h"   // AroObjectHeader 等の型定義 のみ

// VALUE 型が sample 固有 (= intptr_t / uintptr_t / int64_t 等) なら:
typedef uintptr_t VALUE;
#define ARO_GC_VALUE_TYPEDEFED 1   // gc.h の forward decl をスキップ

struct RBasic {
    AroObjectHeader head;          // 8 B、 framework controlled
    // sample 固有 field
    struct korb_class *klass;
};

// 旧 RBASIC(v)->flags は head.flags にアクセスする形に書換:
//   sed: s/RBASIC\(([^)]+)\)->flags/RBASIC(\1)->head.flags/g
//   sed: s/->basic\.flags/->basic.head.flags/g
// 旧 flags が VALUE 幅 (= 8 B) を仮定してた場合、 head.flags は 16 bit
//   なので flag 値が 16-bit 範囲に収まるか確認
```

### 1.4 CTX_struct に astro_gc field 追加

```c
typedef struct CTX_struct {
    struct ASTroGC *astro_gc;   // framework が ARO_GC_INSTANCE 経由でアクセス
    // ... 既存 field
} CTX;

#define ARO_GC_INSTANCE(c) ((c)->astro_gc)
```

### 1.5 contract macros stub 化 (= Phase 2-3 で実装)

context.h に以下を追加:

```c
/* Phase 1 stubs */
#define AROH_IS_GC_OBJECT(v) (!SPECIAL_CONST_P(v))   // sample の immediate check
#define AROH_VISIT_ROOTS(c, ctx, edge_visit)  ((void)0)    // Phase 2
#define AROH_SCAN_EDGES(payload, sz, ctx, fn) ((void)0)    // Phase 3
#define AROH_FINALIZE(payload)                ((void)0)    // Phase 5
#define AROH_INIT_PAYLOAD(payload, sz) \
    memset((char *)(payload) + sizeof(AroObjectHeader), 0, \
           (sz) - sizeof(AroObjectHeader))
#define AROH_INIT_BYTE_PAYLOAD(payload, sz)   ((void)0)
```

### 1.6 libgc API を stub 化

GC_INIT、 GC_disable / GC_enable、 GC_add_roots、 GC_register_finalizer、
GC_gcollect、 GC_get_heap_size 等を `#include <gc.h>` 含めて削除 or stub。

```c
// 例: object.c
#include "precise_gc/gc.h"   // 旧 <gc.h> を置換

// 旧 GC_INIT は不要 (= aro_gc_init を CTX init 時に呼ぶ)
// 旧 GC_disable/enable は Phase 4 で precise fiber tracking に置換、 stub:
//   /* Phase 1 stub: was GC_disable() */ (void)0;
```

### 1.7 aro_gc_init を CTX init に追加

```c
// koruby_setup_ctx() 等の CTX 構築時:
CTX *c = calloc(...);
c->stack_base = malloc(...);
c->sp = c->stack_base;
c->env = c->stack_base;     // root scan lower bound
aro_gc_init(c);             // ← ここで precise GC instance 初期化
```

### 1.8 build + smoke

```sh
make GC=copy 2>&1 | grep error:
./your_sample test.rb         # 簡単な test で smoke
```

Phase 1 段階では heap obj alloc は libc malloc のままで OK (= alloc は
libgc 経由だったのを libc に switch、 leak-on-exit は許容)。 aro_gc_alloc は
まだ呼ばれない。 Phase 2-3 で実 precise GC が走るようになる。

## Phase 2: precise root tracking

### 2.1 AROH_VISIT_ROOTS を実装

GC が root scan する範囲を sample が macro で提供:

```c
// context.h
#define AROH_VISIT_ROOTS(c, ctx, edge_visit) do {                   \
    /* (a) value stack: env..sp */                                  \
    for (VALUE *p = (c)->env; p < (c)->sp; p++) {                   \
        ARO_GC_VISIT_EDGE((ctx), (edge_visit), p);                  \
    }                                                                \
    /* (b) global state */                                          \
    if (korb_vm->main_obj) ARO_GC_VISIT_EDGE_PTR((ctx), (edge_visit), \
                                                  &korb_vm->main_obj); \
    /* (c) method cache / const table 等 */                         \
    ...                                                              \
} while (0)
```

### 2.2 sp[] spill 規律

C local に保持した heap pointer は GC 跨ぐと stale 化する。 GC が起こりうる
処理を跨ぐ場合は **sp[] に spill** してから処理を呼び、 後で **reload**:

```c
// 悪い例: stale local
VALUE l = EVAL_ARG(c, lv);          // GC 可能性あり
VALUE r = EVAL_ARG(c, rv);          // ここで l が stale 化
use(l + r);

// 良い例: sp[] spill + reload
VALUE *sp = c->sp;
sp[0] = EVAL_ARG(c, lv);
c->sp = sp + 1;                      // GC が sp[0] を root として見る
sp[1] = EVAL_ARG(c, rv);
c->sp = sp + 2;
use(sp[0], sp[1]);                   // sp[] 経由は GC で in-place forward 済
```

詳細は `sample/baruby_precise/docs/runtime.md §5.7` の二大ルール (= sp[] spill
+ helper は `VALUE *` で受ける) を参照。

## Phase 3: SCAN_EDGES per obj type

### 3.1 各 heap type の outgoing edges を AROH_SCAN_EDGES で列挙

```c
#define AROH_SCAN_EDGES(payload, payload_size, ctx, edge_visit) do { \
    AroObjectHeader *_h = (AroObjectHeader *)(payload);              \
    switch (_h->flags & T_MASK) {                                    \
      case T_ARRAY: {                                                \
          struct korb_array *_a = (struct korb_array *)(payload);    \
          /* klass field */                                          \
          ARO_GC_VISIT_EDGE_PTR((ctx), (edge_visit),                 \
                                (void **)&_a->basic.klass);          \
          /* items[] (= heap-allocated separately) */                \
          for (long i = 0; i < _a->len; i++) {                       \
              ARO_GC_VISIT_EDGE((ctx), (edge_visit), &_a->ptr[i]);   \
          }                                                           \
          break;                                                      \
      }                                                               \
      case T_HASH: { /* ... */ break; }                              \
      case T_STRING: { /* ... */ break; }                            \
      /* ... 全 type */                                              \
      default: break;                                                 \
    }                                                                 \
} while (0)
```

### 3.2 heap-ptr field に ARO_GC_EDGE qualifier を付ける

Phase 2a で導入した compile-time STORE 漏れ検出。 例:

```c
struct korb_array {
    struct RBasic basic;
    VALUE *ARO_GC_EDGE ptr;     // heap-ptr field、 直接代入は audit で error
    long len, capa;
};

// 書込: 必ず ARO_STORE 経由
ARO_STORE(c, a, &a->ptr, (VALUE)new_buf);
// 読出: 直接 cast でいい (= ARO_LOAD は将来の barrier injection 点)
struct korb_array *a = ...;
process(a->ptr);
```

### 3.3 検証

```sh
make ARO_GC_WB_AUDIT=1 GC=copy           # compile error で漏れ check
BARUBY_GC_PURGE=1 BARUBY_GC_STRESS=1 ./your_sample script.rb   # runtime audit
```

## Phase 4: 特殊機構

### 4.1 fiber (= 別 stack の co-routine)

libgc の `GC_add_roots(fib->stack, end)` は conservative scan で動いたが、
precise GC では **fiber stack を VALUE 型範囲として root に追加**:

```c
// fiber 専用の root visit
for (VALUE *p = fiber->stack_value_base; p < fiber->stack_value_top; p++) {
    ARO_GC_VISIT_EDGE((ctx), (edge_visit), p);
}
```

fiber の locals が VALUE 配列で並んでる必要あり。 conservative scan のように
任意 stack 領域を scan することはできない。

### 4.2 closure / proc env

closure 内に captured locals があれば、 SCAN_EDGES でその env array を walk:

```c
case T_PROC: {
    struct korb_proc *_p = ...;
    for (uint32_t i = 0; i < _p->env_size; i++) {
        ARO_GC_VISIT_EDGE((ctx), (edge_visit), &_p->env[i]);
    }
    break;
}
```

### 4.3 class hierarchy + const table + method cache

`korb_class` の super / includes / prepends / methods / constants / cvars
すべて SCAN_EDGES で walk。 method cache の entry も root として visit。

## Phase 5: external resource (GMP / FILE * etc.)

GMP bignum 等の **libc-malloc'd 外部 buffer** を持つ obj は、 framework の
finalizer 機構経由で解放:

```c
// alloc 時
VALUE korb_bignum_new(...) {
    struct korb_bignum *b = aro_gc_alloc(c, sizeof(*b));
    b->basic.head.flags = T_BIGNUM;
    mpz_init(b->mpz);
    /* External memory pressure 通知 (= 大きい GMP buffer は GC trigger を加速) */
    aro_gc_account_external(c, sizeof(mpz_t) + initial_limbs * sizeof(mp_limb_t));
    /* finalizer 登録: 解放時に AROH_FINALIZE(payload) が呼ばれる */
    aro_gc_finalize_register(c, b);
    return (VALUE)b;
}

// context.h の AROH_FINALIZE で type-dispatch:
#define AROH_FINALIZE(payload) do {                                  \
    AroObjectHeader *_h = (AroObjectHeader *)(payload);              \
    if ((_h->flags & T_MASK) == T_BIGNUM) {                          \
        struct korb_bignum *_b = (struct korb_bignum *)(payload);    \
        mpz_clear(_b->mpz);                                          \
        aro_gc_account_external(NULL, -(...));                       \
    }                                                                 \
    /* FILE * close 等も同様 */                                       \
} while (0)
```

詳細は `docs/gc_design.md §7.7.5` を参照。

## Phase 6: 全 backend 検証

```sh
for GC in mark_gen copy_gen mark_bitmap_gen mark_card_gen mark_gen_inc \
          mark_bump_gen mark_compact_gen immix_gen copy mark mark_compact \
          mark_freelist immix bump none; do
  echo "=== $GC ==="
  make GC=$GC && ./your_sample test/*.rb > /dev/null
  make ARO_GC_WB_AUDIT=1 GC=$GC && BARUBY_GC_STRESS=1 BARUBY_GC_PURGE=1 \
      ./your_sample test/*.rb > /dev/null
done
```

全 backend × audit × stress × purge で全 test pass = precise GC migration 完了。

## Common pitfalls

1. **VALUE typedef mismatch**: sample が `uintptr_t` を使うなら context.h で
   `#define ARO_GC_VALUE_TYPEDEFED 1` を定義して gc.h の forward decl を skip
2. **head.flags が 16-bit**: 旧 RBasic.flags が VALUE 幅 (= 64 bit) を仮定
   していた場合、 flag 値が 16-bit に収まるか check (= T_* / FL_* 全て)
3. **klass / pointer field の ARO_GC_EDGE**: Phase 3 で qualify するが
   Phase 1 段階では skip (= 初期化が直接代入で多数あるため、 一気に const 化
   すると compile error 爆発)。 SCAN_EDGES 実装と同時に qualify が良い
4. **alloca'd frame**: GC=none の leaf-call 最適化等で alloca した struct に
   ARO_GC_EDGE field があると WB が暴走、 `#if !defined(ARO_GC_WB_AUDIT)`
   で audit build では disable する例 (= ascheme_precise/node.h:140)
5. **`(uintptr_t)` 中継 cast**: framework macros (`ARO_STORE` 等) のみが const
   抜きの approved bypass。 sample dev が手で書くと audit を抜けるので避ける
6. **sp[] spill 規律**: GC 跨ぐ前に sp[] に park する `[[feedback_moving_gc_shadow_buf]]`
   memory も参照
7. **helper は VALUE * で受ける**: 内部で alloc する helper は VALUE 値ではなく
   sp[] slot pointer で受け、 alloc 後に `*ref` で再 deref。 詳細は
   `docs/gc_design.md §4 (B)`

## Migration 経験談

- **baruby_precise**: ~50 iter、 4 heap type (Array / String / ArrayItems / ByteData)、
  最小限の sample。 移行の reference 実装
- **ascheme_precise**: 9 phase migration、 ~10 heap type + call/cc + GMP + class
  + closure env。 詳細は `sample/ascheme_precise/docs/migration.md`
- **koruby_precise**: ~15 heap type + fiber + GMP + class hierarchy + method
  table + method cache。 Phase 1 (= 字面移行 + build pass) は実施済、 Phase 2-6
  が future work

### koruby_precise から得た追加教訓

1. **Bootstrap CTX の早期作成が必須**: class 階層 (BasicObject / Object / Class) を
   作るタイミングで current_ctx が NULL だと、 libc fallback で alloc されて
   GC heap 外に置かれる。 後で AROH_VISIT_ROOTS が libc-malloc'd 領域を GC heap
   obj として forward しようとして corruption。 修正は `aro_gc_init` を
   `korb_runtime_init` 冒頭 (= class 作成前) に繰り上げる
2. **Freshly-alloc'd obj は次 GC 前に root 化が必要**: STRESS=1 下では毎 alloc で
   GC 発火、 `T *o = aro_gc_alloc(c, sz);` の直後の GC で o は root に無いので
   即 sweep される。 sp[] に spill してから初期化する必要あり (= 移動 GC では
   さらに重要)
3. **AROH_VISIT_ROOTS / AROH_SCAN_EDGES の out-of-line 化**: 多 heap type を持つ
   sample では macro inline 展開すると framework 全 backend TU が肥大化。
   `koruby_visit_roots()` / `koruby_scan_edges()` を sample 側 .c に書いて
   macro で dispatch する pattern (= koruby_runtime.c で実証)
4. **aux struct (= libc-managed) からの edge walk**: korb_method / korb_const_entry /
   korb_method_table_entry 等の libc malloc auxiliary struct も heap obj から
   reachable。 SCAN_EDGES の中で linked list を辿って中の VALUE を visit する
   必要あり (= koruby_runtime.c の visit_method_table / visit_const_chain 等)
5. **VALUE typedef のばらつき**: sample が `uintptr_t` / `intptr_t` / `int64_t` を
   使い分け。 gc.h forward decl との衝突回避は `ARO_GC_VALUE_TYPEDEFED` macro guard

## audit run 体制 (= 完成後の routine)

```sh
# CI 用 audit run
make ARO_GC_WB_AUDIT=1 GC=copy             # compile-time STORE 漏れ
BARUBY_GC_STRESS=1 BARUBY_GC_PURGE=1 ./your_sample script.rb
# = 高頻度 GC + 64 GiB round-robin mprotect で stale ptr SEGV 確定化
```

audit 体制の理屈は `docs/gc_design.md §3.3`。 Phase 2a (const) + Phase 3
(round-robin PURGE) の 2 層で「字面のミス」 はほぼ catch される。 「holder
取り違え」 等の意味的 bug は code review 領域。
