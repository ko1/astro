# baruby_precise GC ランタイム (sample 固有部分)

> GC backend のアルゴリズム / データ構造 / アルゴリズム解説は
> root の [`../../../docs/gc_runtime.md`](../../../docs/gc_runtime.md)
> に集約済 (= 全 sample 共通)。 本 doc は **baruby_precise 固有の部分**
> (= root 集合の表現、 VALUE encoding、 GCHeader sample-level layout、
> WB API の使い方) のみを残置。
>
> backend ごとの仕組み / 用語 (mark/sweep/young/tenured/Cheney/Immix
> 等) / 16 backend 一覧 / 設計空間俯瞰は root を参照:
> - 用語: root §1.2
> - sample → backend 対応: root §1.3.1
> - 各 backend (mark / mark_gen / copy / immix / ...): root §2 〜 §3
> - common framework API: root §4
> - 設計判断 FAQ: root §6

技術的により深い実装メモは [runtime.md §5](runtime.md) を参照。 ベンチ
結果と勝者分布は [perf.md §2](perf.md) を参照。

## 1. baruby_precise が共通で持つもの (sample 固有)

backend が違っても、 baruby_precise の **alloc API と root 表現は同じ**。
これらは backend を差し替えても変わらない:

### 1.1 Root: 共有 `c->env..c->sp` の VALUE 配列

```
c->env  ─ stack 底
 ├ frame 0 locals     ─┐
 │  ...                ├─ GC が scan する range
 ├ frame 1 locals     │
 │  ...                │
 ├ frame N locals     │
 │  scratch slots     ─┘
c->sp   ─ stack 頂上 (mutator が次に push する位置)
```

dispatcher 関数が再帰呼び出しを進めるたびに sp を伸ばし、 frame を積む。
GC は collect 時に `c->env..c->sp` を順に walk し、 各 slot を VALUE と
して扱う。 ヒープポインタか即値かは VALUE の LSB tag で判定する。

framework hook `AROH_VISIT_ROOTS` 実装は `main.c` の
`aro_baruby_visit_roots` を参照。

### 1.2 VALUE 表現 (LSB-tagged)

```
[ ...62 bit... | 00 ]   ← ヒープへのポインタ (8-aligned なので下 2 bit 必ず 0)
[ ...62 bit... | 01 ]   ← 小整数
[ ...62 bit... | 11 ]   ← 特殊値 (true / false / nil 等)
```

GC は LSB を見て pointer かどうかを判定 (`IS_PTR(v)`)。 即値は無視する。
これが **precise scanning** (= conservative ではない) の根幹。

### 1.3 GCHeader の sample 側 layout

backend ごとの GCHeader 構成 / size は root §2.x.3 (= 各 backend
データ構造) を参照。 sample 側で扱う bit / field は以下の通り。

ヒープオブジェクトは payload の **すぐ前** に GCHeader を持ち、
sample は `h+1` を payload 起点として読み書きする:

```
[ GCHeader ][ payload ... ]
            ^
            VALUE はここを指す (h+1)
```

iter 31 で **全 backend が flags byte packing に統一**。 `kind`/`marked`/
`old`/`dirty` の各 bool / enum field が単一 `flags` byte (uint8_t) に
ビット詰めされる (= header サイズが大幅に縮む。 e.g., 24 B → 16 B、
immix は 16 B → 8 B)。

sample が触る field:

| Field | 型 | 意味 |
|---|---|---|
| `flags` | uint8_t | **packed 状態 byte**。 bit 0-2: `kind`、 bit 3-5: `marked` / `old` / `dirty` のうち backend で必要な分。 `HDR_KIND(h)` / `HDR_MARKED(h)` / `HDR_OLD(h)` / `HDR_DIRTY(h)` のマクロでアクセス |
| `kind` (bit 0-2) | 3 bits | オブジェクトの種類タグ。 `KIND_OBJ_ARRAY` / `KIND_OBJ_STRING` / `KIND_PAYLOAD_VAL` (= VALUE[]、 BaArray.items の中身) / `KIND_PAYLOAD_BYTE` (= char[]、 BaString.bytes の中身) / `KIND_FREE` (freelist 上の slot)。 5 種類 → 3 bit。 mark phase が outgoing 参照を辿るとき、 payload を何として解釈するか判定するのに使う |
| `size` | uint32_t | payload バイト数 (アライメント前の logical size) |

backend ごとに追加で持つ bit (`marked` / `old` / `dirty` / `fwd` /
`mark_epoch` / `_pad` 等) と、 backend → header size の早見は root
§2 各 subsection を参照。

iter 31 packing 前後のサイズ比較 (主な backend):
- `mark`: 16 B → **8 B** (-50%)
- `mark_gen` / `mark_gen_inc`: 24 B → **16 B** (-33%)
- `copy` / `copy_gen` / `copy_gen_inc`: 24 B → **16 B** (-33%)
- `mark_compact*` / `mark_bump_gen`: 24 B → **16 B** (-33%)
- `immix` / `immix_gen`: 16 B → **8 B** (-50%)

GCHeader が小さいほど slot に占める割合が減るので、 特に short payload
(BaArray の 24 byte 等) の **密度が上がる**。 mark の 8 B header は
class 32 に BaArray (24 B + 8 B header = 32 B) がぴったり収まる効果が
大きい (旧 16 B header 時は class 48 / 64 に逃げて 25-40% waste)。
詳細は perf.md hash_chain の数値比較。

### 1.4 Write barrier API

```c
void ARO_STORE     (CTX *c, void *holder, VALUE *slot, VALUE v);
void ARO_STORE_BULK(CTX *c, void *holder, VALUE *dst, const VALUE *src, size_t n);
```

世代別 / incremental backend では、 **ヒープから別ヒープへのポインタ書込**
は必ず `ARO_STORE` を経由する。 非 gen backend では `ARO_STORE` は
`*slot = v` に inline 化 (zero cost)。

backend ごとの WB 実装と remset 設計は root §2 各 backend の「アルゴリズム
詳細」 + 「promote-time WB」 を参照。

### 1.5 Per-instance state (iter 62)

GC instance は heap 上の `struct ASTroGC` (= per-backend struct で、
**first field は必ず `AroGcCommonState common`**) で表される。 各 backend
の `aro_gc_init(c)` が `calloc` して `c->astro_gc` に bind する。
process-scope の global 変数は無い (複数 instance を 1 process に
co-exist させられる設計)。

共通 header (gc.h で定義) は:
```c
typedef struct AroGcCommonState {
    AroGcStats stats;
    bool       stress;
    int        time_depth;     /* re-entrant collect の最外 1 回だけ計測 */
    struct timespec time_t0;
} AroGcCommonState;
```

`ASTRO_GC_COMMON(c)` macro が `(AroGcCommonState *)c->astro_gc` で
type-safe にアクセス。 stat reader (`aro_gc_total_bytes(c)` 等) は
全部 CTX を引数に取る。

shutdown 時は `aro_gc_fini(c)` が backend ごとの resource (mmap 領域 +
linked list の large object など) を release + `c->astro_gc` を free。
main.c の `return 0` 直前で呼ばれる。 multi-instance / leak-sanitizer
clean run のため (iter 65)。

## 2. workload と backend 選択 (baruby_precise bench の観察)

ベンチ結果 (perf.md §2 の勝者分布) と合わせると、 baruby_precise で
観察されている傾向:

| Workload pattern | おすすめ |
|---|---|
| 短命大量 alloc + 長寿命少 (e.g. `string_concat`, `cons_list`) | `copy_gen`、 `mark_compact_gen`、 `mark_bump_gen`、 `immix_gen` |
| 長寿命優位 (`binary_trees`) | `bump`、 `copy`、 `mark_compact`、 `immix` |
| Hash table 系 (`hash_chain`) | `mark_bitmap_gen` (24 B → 8 B header の density 効果)、 `bump`、 `immix_gen` |
| 構造 + churn mix (`gc_combined`, `list_alloc`) | `mark_compact_gen` 安定して上位 |
| Mutator-bound (`fannkuch`, `nqueens`) | どれでも近い (GC 比率小) |
| Latency 重視 | `*_inc` 系 (incremental mark で pause 短) |

汎用 default は `copy` (Cheney)、 GC を完全に抜いた baseline 比較は `none` /
`bump`、 worker memory が制約な systems では `mark_bitmap_gen` (8B header) が
有利、 等。

各 backend の algorithm 詳細 / 用語 / 設計判断は root の
[`../../../docs/gc_runtime.md`](../../../docs/gc_runtime.md) を参照。
