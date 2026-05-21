# ASTro GC 設計

ASTro framework が precise GC についてどんな抽象を提供し、 sample (= 言語実装)
側が何を差し込むかをまとめた設計ドキュメント。 実装は `sample/baruby_precise/`
配下に 16 backend の reference impl があり、 iter 62 で gc_copy.c に対して
contract macro 抽象化の PoC が完了している。

関連: `idea.md` §8.2、 `code_store_quirks.md` (AST NODE 不動制約)、
`sample/baruby/` (conservative libgc 比較対象)、
`sample/baruby_precise/docs/runtime.md` (実装詳細)。

---

## 0. 概要

ASTro の precise GC は **MMTk 流の VMBinding 抽象** を C macro で実現する
形を取る。 大きく 2 層に分離:

```
┌──────────────────────────────────────────────┐
│ sample (= 言語実装)                          │
│  - VALUE 表現 / object header                │
│  - object shape (= traversal / init policy)  │
│  - root scan (= sp[] + globals 等)           │
│  - AstroGc instance の allocation + bind     │
└──────────────────────────────────────────────┘
                  ▲ contract macros
                  │ (compile-time inline)
                  ▼
┌──────────────────────────────────────────────┐
│ framework (= runtime/precise_gc/)            │
│  - GC algorithm (16 backend: copy / mark /   │
│    immix / immix_gen / ...)                  │
│  - mmap / region 管理                        │
│  - allocator API (aro_gc_alloc 等)           │
└──────────────────────────────────────────────┘
```

framework は **graph (= GC object と pointer エッジ) しか見ない**。 VALUE
の tagging 規約、 object の具体的 layout、 root の在処などは sample が
contract macro で提供する。 framework は与えられた macro を compile-time
に inline 展開して動く (= MMTk の generic monomorphization と等価)。

これにより:

- 同一 framework で異言語 sample が動く (baruby / koruby / pystro / etc)
- 同 sample 内で 16 GC backend を `-DASTRO_PRECISE_GC_<algo>` で切替可能
- 将来 multi-instance (= 同 process で複数 GC) 拡張も「**AstroGc** struct を
  複数 allocate するだけ」 で対応 (= framework は module-static 持たない)

### 0.1 設計の核心 3 つ

1. **contract macros**: sample が「VALUE 表現」「object shape」「root の場所」
   を C macro で提供。 framework は macro を inline 展開して使う。
2. **`struct AstroGc`**: GC algorithm が必要とする process-scope state を
   1 つの struct にまとめる。 sample が instance を 1 つ (or 複数) 確保し、
   `aro_gc_init` で初期化。 framework module-static は **持たない**。
3. **edges 経由の object scan**: `ASTRO_GC_SCAN_EDGES(h, edge_visit)` macro
   が outgoing reference を slot pointer で列挙、 mark / forward / update
   全 phase が同 macro + 違う visit callback で済む (= MMTk 流)。

---

## 1. Object graph 観点

GC は **graph (= node = GC object、 edge = inter-object pointer)** しか見ない。
所有関係や semantics は知らず、 reachability で live を判定する。

### 1.1 1 GC object = 1 つの連続アロケーション

```c
void *aro_gc_alloc(AroGcKind kind, size_t payload_size, VALUE *sp_top);
```

1 回の `aro_gc_alloc` 呼び出しが「1 ノード」 を作る。 header + 連続 payload。
**不連続なメモリブロックは別々の GC object** にして pointer で繋ぐ。

例: baruby の Array は **2 object 構造**:

```
┌─ KIND_OBJ_ARRAY ────────────┐
│ GCHeader                    │
│ BaArray {                   │
│   uint32_t len;             │
│   uint32_t capa;            │
│   VALUE *items; ────────────┼──┐
│ }                           │  │
└─────────────────────────────┘  │
                                 ▼
                 ┌─ KIND_PAYLOAD_VAL ──────┐
                 │ GCHeader                │
                 │ VALUE[0]                │
                 │ VALUE[1]                │
                 │ ...                     │
                 └─────────────────────────┘
```

- BaArray header (= sample 視点の「Array」 入口) は **items pointer 1 個だけ持つ**
- VALUE[] payload は **全 slot が VALUE** (= scan 対象)
- GC mark で BaArray → items を辿り → 各 VALUE → 各要素 object に伝播

なぜ 2 分けるか:
- header は **固定 size** (= len/capa/pointer)
- payload は **可変 size** (= 要素数に依存)
- 1 object に統合すると `Array.push` の grow で外部 reference 無効化

同様に BaString は header (= len/capa/bytes pointer) + bytes[] (= KIND_PAYLOAD_BYTE)
の 2 object 構造 (例外: SSO で小文字列だけ 1 object に inline)。

### 1.2 GCHeader layout

framework が touch する header フィールドは **3 つだけ**:

| field | 用途 | 必要 backend |
|---|---|---|
| `size` | moving 系の region walk で次 object 位置に進む | 全 backend が realloc 等で使う |
| mark bit | live signal | non-moving header-bit 系 (mark / mark_freelist 等) |
| `fwd` | forwarding pointer | moving 系 (copy / mark_compact / immix compact phase) |

その他 (kind / class 情報 / age bits 等) は **sample 内部の事情**、 framework
からは opaque。 sample は header に好きなビット allocation で kind 等を埋める。

backend ごとに必要フィールドが違うので、 framework は `ASTRO_GC_NEEDS_FWD` /
`ASTRO_GC_NEEDS_HEADER_MARK` フラグで `struct GCHeader` を #ifdef 化する
(= moving は 16B、 non-moving slab は 8B など最適化可能)。

---

## 2. sample が提供する contract

sample は `#include "<framework gc.c>"` の前に以下の typedef + macro を
define しておく。 framework は macro を compile-time inline 展開して使う。

### 2.1 VALUE / pointer 表現

```c
typedef intptr_t VALUE;   // sample 固有の値表現 (LSB tag / NaN-box / etc)

#define ASTRO_GC_VALUE_IS_PTR(v)      /* heap pointer か判定 */
#define ASTRO_GC_VALUE_TO_HEADER(v)   /* VALUE → GCHeader * */
#define ASTRO_GC_HEADER_TO_VALUE(h)   /* GCHeader * → VALUE (forward 後 rewrite で使う) */
```

framework は VALUE 内部表現を知らない (= 任意 tagging 戦略 OK)。

### 2.2 Object shape

```c
/* Outgoing reference の列挙。 visit callback は slot pointer を受ける
 * (= mark phase なら read のみ、 forward phase なら read + write back)。
 * KIND ごとに何を edge とするかを sample 内 switch で記述。 */
#define ASTRO_GC_SCAN_EDGES(h, edge_visit) /* ... */

/* 新 payload を scan-safe な値で初期化 (alloc 直後 GC が走っても安全) */
#define ASTRO_GC_INIT_PAYLOAD(payload, size_bytes) memset(payload, 0, size_bytes)

/* byte payload (= scan 対象外) の初期化、 通常 no-op */
#define ASTRO_GC_INIT_BYTE_PAYLOAD(payload, size_bytes) ((void)0)
```

`SCAN_EDGES` の visit callback signature は `void (void **slot)`。 slot pointer
なので visit 内部で deref / 書き換え両方できる。 これにより:

- **mark phase**: `mark_edge` は `*slot` を読んで grey push (slot は変えない)
- **forward phase** (moving): `forward_edge` は `*slot = forward(*slot)` で書き換え

3 phase とも **同じ SCAN_EDGES + 違う visit** で済む (= MMTk 流)。

`INIT_PAYLOAD` は言語の VALUE 規約に依存:
- baruby (`VAL_FALSE == 0`): `memset 0` で OK
- NaN-boxing: 0 が valid double だと scan 誤判定の危険 → `NIL_VALUE` で fill
- sample が用途に合った fill 方法を提供

### 2.3 Root scan

```c
/* sample が「全 root を visit_value に渡す」 を 1 つの macro で記述。
 * thread 一覧 / globals / finalizer queue 等を全部 sample が iterate する。
 * framework は root の場所を知らない。 */
#define ASTRO_GC_SCAN_ROOTS(c, edge_visit, sp_top) do {                    \
    /* 例: 単一 thread の sp[] 走査 */                                     \
    for (VALUE *p = (c)->env; p < (sp_top); p++)                           \
        edge_visit((void **)p);                                            \
    /* 他に追加 root があればここで */                                     \
} while (0)
```

`sp_top` は GC trigger 時点の **現スレッドの scan 上限** (= alloc が呼ばれた
瞬間に caller が握ってる sp top)。 他 thread の sp は safepoint 時点の値を
sample 側で参照する。 multi-thread 対応はこの macro 内で thread list を
iterate するだけ — framework に手は入らない。

### 2.4 Instance accessor

```c
/* CTX → AstroGc * (= process-scope state へのアクセス経路) */
#define ASTRO_GC_INSTANCE(c) /* ... */
```

`(c)->astro_gc` のような形が一般的。 sample が CTX 構造を決め、 そこへの
アクセス経路を 1 つの macro で提供する。 multi-instance なら 1 CTX が 1
AstroGc instance に対応するように sample が wire する。

### 2.5 contract macros 一覧

| 分類 | macro / typedef | 役割 |
|---|---|---|
| **A. VALUE** | `VALUE` typedef | sample の値表現 |
| | `ASTRO_GC_VALUE_IS_PTR(v)` | heap pointer 判定 |
| | `ASTRO_GC_VALUE_TO_HEADER(v)` | VALUE → GCHeader * |
| | `ASTRO_GC_HEADER_TO_VALUE(h)` | GCHeader * → VALUE |
| **B. Shape** | `ASTRO_GC_SCAN_EDGES(h, visit)` | children traversal |
| | `ASTRO_GC_INIT_PAYLOAD(p, n)` | scan-safe init |
| | `ASTRO_GC_INIT_BYTE_PAYLOAD(p, n)` | byte init (通常 skip) |
| **C. Root** | `ASTRO_GC_SCAN_ROOTS(c, visit, sp)` | 全 root 走査 |
| **D. Instance** | `ASTRO_GC_INSTANCE(c)` | CTX → AstroGc * |
| **E. Header** | `ASTRO_GC_HEADER_SIZE(h)`, `_SET_SIZE` | header accessor (framework default あり、 sample override 可) |
| | `_GET_MARK / _SET_MARK` | header-bit backend のみ |
| | `_GET_FWD / _SET_FWD` | moving backend のみ |
| **F. Algorithm** | `-DASTRO_PRECISE_GC_<algo>` | backend 選択 |

---

## 3. framework が提供する API

framework は state-less (= module-static 持たない)。 全 state は `CTX *c` 引数
+ macro accessor 経由でアクセス。

```c
/* process 起動時 1 回。 共有 mmap region 確保、 lock 初期化 etc */
void aro_gc_process_init(AstroGc *gc);

/* allocation (scan-safe init 付き) */
void *aro_gc_alloc(CTX *c, AroGcKind kind, size_t size, VALUE *sp_top);

/* byte payload (init skip) */
void *aro_gc_alloc_byte(CTX *c, AroGcKind kind, size_t size, VALUE *sp_top);

/* resize: VALUE 系 (extension が ASTRO_GC_INIT_PAYLOAD で init) */
void *aro_gc_realloc_payload(CTX *c, void *old, size_t new_size, VALUE *sp_top);

/* resize: byte 系 (extension は INIT_BYTE_PAYLOAD = 通常 no-op) */
void *aro_gc_realloc_payload_byte(CTX *c, void *old, size_t new_size, VALUE *sp_top);

/* write barrier (non-gen は static inline `*slot = v`、 gen は remset push) */
void  aro_gc_wb(CTX *c, void *holder, VALUE *slot, VALUE v);

/* 明示 collect (主に test / stress 用) */
void  aro_gc_collect(CTX *c, VALUE *sp_top);
```

`sp_top` の意味は各 API で同じ:
- 現スレッドの VALUE stack top
- alloc 中に GC 走ったときの root scan 上限
- mutator が `sp_top` を渡す責任 (= 「ここから上は live」 という宣言)

### 3.1 AstroGc struct

backend ごとに「algorithm が必要とする process-scope state」 が違うので、
`AstroGc` struct の中身は backend 依存:

```c
/* gc_copy.c 内 */
typedef struct AstroGc {
    char *active_base, *active_top, *active_end;
    char *space0, *space1;
    int   active_idx;
    size_t bytes_since_gc;
    size_t gc_threshold;
    CTX *ctx;
    /* Cheney scratch */
    char *to_top, *to_base, *from_base_cur;
    VALUE *sp_high_water;
} AstroGc;

/* gc_mark.c 内 (別 backend なので異なる) */
typedef struct AstroGc {
    Page *page_head[NUM_SIZE_CLASSES];
    FreeSlot *freelist[NUM_SIZE_CLASSES];
    LargeObj *large_head;
    size_t bytes_since_gc;
    /* ... */
} AstroGc;
```

sample は 1 つ (or 複数) を allocate して `aro_gc_process_init(&gc)` に渡し、
各 CTX に instance pointer を持たせる。 multi-instance シナリオは「異なる
AstroGc を別 CTX に bind する」 だけで成立。

---

## 4. Sample 実装の規約 (Moving GC 必須二大パターン)

moving GC 系 backend (= copy / mark_compact / immix compact phase) を使う
sample は、 NODE_DEF / C helper コードに **以下二大ルール** を守る必要がある。
mark&sweep だけで動かしてた時代には潜伏してた precise rooting バグが
moving に切替えた瞬間に一斉に表面化するので、 移行時に必須:

### (A) sp[] spill

**heap VALUE を子ノード evaluation を跨いで保持する場合、 C local では
なく sp[] slot に置く**。

子の eval で GC が走ると sp[] は in-place forward されるが、 C local
変数 (= register or stack) は更新されない:

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
baruby_str_concat(VALUE lhs, VALUE rhs, VALUE *sp_top) {
    BaString *l = VAL2STR(lhs);  /* GC 前のアドレス */
    /* ここで alloc → GC → l が stale */
    VALUE result = aro_gc_alloc(KIND_OBJ_STRING, sizeof(BaString), sp_top);
    BaString *r = VAL2STR(result);
    memcpy(r->bytes, l->bytes, l->len);  /* stale pointer access */
    /* ... */
}

/* 良い例: VALUE* で受けて post-alloc に再 deref */
VALUE
baruby_str_concat(VALUE *lhs_ref, VALUE *rhs_ref, VALUE *sp_top) {
    /* lhs_ref / rhs_ref は sp[] slot を指す。 GC 後も slot 経由で
     * 最新アドレスが取れる */
    VALUE result = aro_gc_alloc(KIND_OBJ_STRING, sizeof(BaString), sp_top);
    BaString *l = VAL2STR(*lhs_ref);   /* GC 後の正しいアドレス */
    /* ... */
}
```

このルールは baruby_precise で 5+ 回 rooting バグが発生して確立。 詳細
は [sample/baruby_precise/docs/runtime.md §5.7](../sample/baruby_precise/docs/runtime.md)。

### 副次的な注意点

- **stress mode**: `BARUBY_GC_STRESS=1` で毎 alloc に GC 発火。 from-space
  を即 `mprotect(PROT_NONE) + MADV_DONTNEED` で永久 retire するので、 stale
  pointer access が即 SIGSEGV になる。 ルール遵守を強制検証する debug 手段。
- **AOT SD**: framework-generated SD chain でも (A)(B) は守られている (= ASTroGen
  が `@child` snapshot を sp[] に出力)。 手書き node_slowpath.c で抜けがちなので
  注意。

---

## 5. 試作: `sample/baruby_precise`

### 5.1 概要

`sample/baruby_precise/` は precise *moving* GC の testbed。 仕様は baruby と
同じ Ruby サブセット (= naruby fork 由来 + Array / String + LSB tag VALUE)。
GC backend を 16 個切替可能:

```
copy, copy_gen, copy_gen_inc (= clone of copy_gen),
mark, mark_gen, mark_gen_inc,
mark_compact, mark_compact_gen,
mark_bump_gen, bump,
immix, immix_gen,
mark_bitmap_gen, mark_card_gen, mark_freelist, none
```

35 bench × 16 backend のマトリクス検証で動作確認 (`bench/matrix.rb`)。
詳細 perf は [sample/baruby_precise/docs/perf.md](../sample/baruby_precise/docs/perf.md)。

### 5.2 iter 62 PoC: gc_copy.c の framework-style refactor

framework 化に向けた抽象化 PoC を gc_copy.c に適用 (commit `fc133656` on
branch `gc-abstraction-poc`)。 主要変更:

- **`struct AstroGc` 導入** — 9 個の module-static を 1 構造体に集約
  (`active_base/top/end`, `space0/1`, `active_idx`, `bytes_since_gc`,
   `gc_threshold`, `ctx`, `to_top/base`, `from_base_cur`, `sp_high_water`)
- **`ASTRO_GC_SCAN_EDGES` macro** — 旧 `process_object` の switch を
  slot pointer 列挙に統一、 mark / forward 両 phase で同 macro 利用
- **`ASTRO_GC_INSTANCE()` macro** — `g_astro_gc` への現在の (single-instance)
  アクセス。 将来 CTX 経由化で multi-instance 対応
- **header accessor macro** — `ASTRO_GC_HEADER_SIZE/_SET_SIZE/_GET_FWD/_SET_FWD`

### 5.3 PoC 検証結果

- **oracle**: copy backend × 35 bench × `-n 1` で plain + AOT 両方 0 FAIL /
  0 FATAL
- **perf**: iter 61 と同等 (= n=1 noise 範囲内、 binary_trees 0.70s / prime_count
  0.54s / dll_walk 0.14s 等、 改造前後で差なし)
- **設計検証**: 「module-static → struct 集約」 で perf 退化なし (gcc が
  フィールドを register に hoist)、 「switch → macro 統一」 で MMTk 流の
  callback ベース mark/forward が C で実装可能と確認

他 backend (15 個) は無修正のまま動作継続 — contract 抽象化は **漸進的に
1 backend ずつ port 可能** という性質も同時に確認。

---

## 6. 移行計画

### Step 1: 他 backend を contract に port

`gc_mark.c` / `gc_copy_gen.c` / etc を gc_copy.c と同じパターンで refactor。
各 backend で:

- module-static → `struct AstroGc` (backend ごとに layout 異なる)
- `process_object` / `mark_object` の switch → `ASTRO_GC_SCAN_EDGES` 経由
- `forward_payload` / `mark_payload` → edge_visit callback

各 backend 単独で oracle pass を確認しつつ port。 16 backend なので 1 日
数 backend ペースで進める想定。

### Step 2: contract macro を sample-side header に集約

`gc_copy.c` 内 inline 定義の contract macros を、 sample 共通の header
(`gc_user.h` ではなく、 user 議論で結論したように **`context.h` か新規 sample 側
ヘッダ**) に切り出す。 全 backend が同 macro を共有することで重複削減。

### Step 3: `runtime/precise_gc/` 切り出し

contract が固まったら、 `gc_<algo>.c` を `runtime/precise_gc/` に移動。
`gc.c` が共通 runtime として algorithm を `#include` する形に整える。

```
runtime/precise_gc/
  gc.h           # public API + 推奨 GCHeader 雛形
  gc.c           # ASTRO_PRECISE_GC_<algo> マクロで algo を #include
  gc_copy.c
  gc_mark.c
  ... (16 backend)

sample/baruby_precise/
  main.c         # contract macro 定義 → #include "../../runtime/precise_gc/gc.c"
```

### Step 4: 他 sample で採用

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

### 7.2 copy 系 backend の large_alloc 経路

sieve AOT で `copy` が `mark` の 2× 遅い問題 (= L1 miss 3.2×) の対策。
閾値 (= 4KB、 mark の size_class 最大値と整合) 以上の payload は **glibc malloc
直** + linked list で管理、 GC sweep で `free(p)`。 glibc が 128KB 以上を
自動 mmap/munmap してくれるので物理メモリ即解放。 details は
[sample/baruby_precise/docs/todo.md](../sample/baruby_precise/docs/todo.md)。

framework 化前に baruby_precise で実装 → contract に統合。

### 7.3 walker (sp_offset bake) の framework 化

`sample/baruby_precise/baruby_parse.c::walk_bake_sp_offset` は hand-write
で全 NODE kind を列挙 (~200 行)。 HASH 系と同じ仕組みで astrogen.rb に
per-kind child-walk callback の gen task を追加して自動生成に畳む。

これは GC とは独立した別 todo (iter 61 fp 削除に付随)。

### 7.4 `VALUE_DEF` 採否

ASTroGen に「`VALUE_DEF` で値の構造宣言」 を追加すれば、 contract macro
(特に SCAN_EDGES) を自動生成できる可能性。 ただし baruby_precise の現状
manual macro でも実用可能性が確認できたので、 価値判断は他 sample が
contract を採用する段で再評価。

### 7.5 finalizer / weak ref

ASTro が finalizable object や weak reference を必要とするかは現状未定。
必要になったら contract に `ASTRO_GC_FINALIZE_OBJECT` / `ASTRO_GC_WEAK_*`
を追加。 BDW 風の topological finalization が default 想定。

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
  — 個別 backend 改善の todo (large_alloc 経路、 etc.)
