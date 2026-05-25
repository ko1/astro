# ASTro precise GC runtime: backend ガイド

ASTro `runtime/precise_gc/` には 17 個の GC backend が同居しており、 build 時に
`-DBARUBY_GC=<n>` で切替する。 すべて同じ framework API (= `aro_gc_alloc`,
`aro_gc_wb`, `aro_gc_collect`, `AROH_VISIT_ROOTS`, `AROH_SCAN_EDGES`,
`aro_gc_finalize_*`) を実装し、 sample 側は backend 切替に透明。

このドキュメントは各 backend の動作を 1 か所にまとめた reference。 性能比較は
`docs/perf.md` と `sample/*/docs/perf.md` を、 framework の設計思想は
`docs/gc_design.md` を参照。

## 1. 概要

### 1.1 サポートしている GC アルゴリズム一覧

| # | 名称 | 特長 |
|---|------|------|
| 1 | `none` | 解放しない。 malloc + leak。 GC overhead = 0 の baseline |
| 2 | `mark` | mark&sweep。 9 size class slab + bitmap。 generational なし |
| 3 | `mark_gen` | mark&sweep + 2-gen + N-survive (= age 0..3, promote on 4th survival) |
| 4 | `mark_gen_inc` | `mark_gen` + incremental marking (SATB barrier) |
| 5 | `copy` | Cheney semi-space copying。 8 B fwd overlay (= no `gc_fwd` field) |
| 6 | `copy_gen` | Cheney + 2-gen + 4-面 layout (= 2 young halves + 2 tenured halves) + N-survive |
| 7 | `copy_gen_inc` | `copy_gen` placeholder (= 同実装、 incremental は未実装) |
| 8 | `mark_compact` | Lisp-2 sliding compactor (mark + fwd-addr + update-ptr + slide) |
| 9 | `mark_compact_gen` | nursery 4-面 + tenured Lisp-2 slide。 N-survive |
| 10 | `bump` | bump allocator のみ。 解放しない (= `none` strictly faster baseline) |
| 11 | `mark_bump_gen` | 4-面 nursery + bump tenured + linear sweep。 N-survive |
| 12 | `immix` | Immix line/block mark-region。 32 KiB block × 128 B line。 非 moving |
| 13 | `immix_gen` | 4-面 nursery + Immix tenured。 N-survive |
| 14 | `mark_bitmap_gen` | `mark_gen` と意味同等、 metadata を per-page bitmap 化 (= 8 B header)。 first-survival promote (= N-survive 未対応、 debug 中) |
| 15 | `mark_card_gen` | `mark_bitmap_gen` + per-page card-dirty flag。 first-survival |
| 16 | `mark_freelist` | mark&sweep、 size-class freelist。 generational なし |
| 17 | `copy_scramble` | `copy` + per-cycle XOR mask R で VALUE storage を撹乱。 audit / debug backend |

### 1.2 用語

- **young / nursery** — 新規 alloc 領域 (= 短命オブジェクト向け、 minor GC のみで回収)
- **tenured / old** — 長命オブジェクト領域 (= 通常 major GC のみで回収)
- **promote** — young → tenured への昇格。 first-survival (= 1 回生き残ると即 promote) と N-survive (= N 回生き残ると promote) の 2 方式
- **N-survive** — `age` bits を header に持ち、 marked young の age を minor 毎に inc。 age >= `PROMOTE_AGE` (= 3) で promote
- **minor GC / major GC** — minor は young のみ、 major は heap 全体を対象とする GC
- **WB (write barrier)** — heap-pointer 書込で remset に holder を追加する hook。 `aro_gc_wb(c, holder, slot, val)`
- **remset (remembered set)** — tenured→young pointer を持つ tenured obj の集合。 minor mark phase で remset entry を scan することで young 子孫を到達可能にする
- **SATB (snapshot-at-the-beginning)** — incremental marking で overwrite 直前の旧値を mark する WB 方式
- **dirty bit** — tenured obj が remset に居る印。 WB で set、 minor の remset scan 後に clear
- **mark bit** — mark phase で訪問済の印。 sweep 後に clear (= incremental では epoch 方式で再利用)
- **sticky mark** — major でも mark bit を残し、 minor では「mark bit が無い young が dead」という判定に使う方式 (= `mark_gen` 系)
- **Cheney** — semi-space copying GC アルゴリズム。 from-space を to-space に forward しながら scan
- **Lisp-2 slide** — non-copying sliding compactor。 mark → forward-addr 計算 → 全 pointer 更新 → 一括 memmove
- **Immix line / block** — Immix の領域単位。 block (= 32 KiB) ごとに line (= 128 B) の mark bit を持ち、 line 単位で hole alloc
- **FORWARDED bit** — moving GC で 「この obj は new addr に copy 済」 マーク。 詳細位置は backend ごと
- **fwd overlay** — `HDR_FORWARDED` 設定後、 旧 obj の payload[0..8] に new addr を上書き保存する手法 (= 専用 `gc_fwd` field 不要)
- **force_promote** — major で young を強制 promote するための flag (= 4-面 backend で major fold-young 時に true)
- **scramble** — `copy_scramble` の per-cycle XOR mask。 stale slot を SEGV で検出するための audit 機能
- **AGE bits** — gc_flags 内の 2 bit (= 値域 0..3)。 N-survive で promote 判定に使う

## 2. 各アルゴリズム紹介

### 2.1 none

#### 2.1.1 概要

何もしない baseline。 alloc は `malloc(payload_size + sizeof(AroObjectHeader))`、
解放しない。 GC overhead を排除した上限 性能の参照値。

#### 2.1.2 パラメータ

なし。

#### 2.1.3 ヒープ構造

libc malloc。 自分では何も持たない。

#### 2.1.4 アルゴリズム詳細

- alloc: malloc + payload zero-fill (= GC-scan safe)。
- collect: `aro_gc_collect` は no-op。
- WB: no-op (= `ARO_GC_HAS_WB` 未定義)。
- finalize_walk: 全 finalize entry を「alive」扱い (= 解放しない)。

### 2.2 bump

#### 2.2.1 概要

mmap した 1 つの bump region に対し pointer を進めるだけ。 `none` よりも alloc が
速い (= malloc 経由ではなく `*top++`)。 解放しない。

#### 2.2.2 パラメータ

- `BUMP_BYTES` (= `ARO_GC_REGION_VIRT_BYTES = 64 GiB` 仮想)

#### 2.2.3 ヒープ構造

1 つの `mmap(NULL, BUMP_BYTES, MAP_NORESERVE)` region。 物理 page は touch 時に
commit。

#### 2.2.4 アルゴリズム詳細

- alloc: `top += ALIGN8(size)`。 region 越えると abort。
- collect: no-op。

### 2.3 mark

#### 2.3.1 概要

非世代の mark&sweep。 9 size class (= 32, 64, ..., 4096 B) の slab + page 構造。
generational なしの単純な mark-sweep baseline。

#### 2.3.2 パラメータ

- `NUM_SIZE_CLASSES = 9`、 サイズ classes `{32, 64, 128, 256, 512, 1024, 2048, 3072, 4096}`
- `PAGE_SIZE = 16 KiB`
- 閾値 (= alloc bytes / heap_bytes 比) で `gc_collect` 自動発火

#### 2.3.3 ヒープ構造

- `page_head[NUM_SIZE_CLASSES]` — class 別 page chain
- `freelist[NUM_SIZE_CLASSES]` — class 別 free slot chain (= `FreeSlot.next` overlay)
- 各 `Page` は `mark` bitmap、 large obj は `LargeObj` linked list (= header + map_bytes)
- `mark` / `freed` の状態は `gc_flags` の bit

#### 2.3.4 アルゴリズム詳細

- alloc: size class 決定 → freelist pop → 無ければ `new_page` で mmap。 large (> 4 KiB)
  は `large_alloc` で個別 mmap
- collect: ① roots → mark (gray queue で transitive)、 ② sweep_all_pages (= unmarked freed)、
  large list も同様
- finalize: live = MARKED、 dead = !MARKED

### 2.4 mark_gen

#### 2.4.1 概要

`mark` + 2-gen + N-survive。 minor は young (= `young_objs` 配列で管理) を対象、
major は heap 全体。

#### 2.4.2 パラメータ

- 2.3 と同じ。 加えて `PROMOTE_AGE = 3` (= 4 GC 生存で promote)
- young 閾値: `young_threshold = 16 MiB`
- major 閾値: `old_major_threshold = MAX(MAJOR_THRESHOLD_MIN = 16 MiB, live × 2)`

#### 2.4.3 ヒープ構造

- 2.3 と同じ slab/page
- 加えて `young_objs[]` — alloc 時に push、 minor の sweep_young で walk
- `remset_buf[]` — `aro_gc_remember` で push (= heap WB の slow path)
- `promoted_buf[]` — 各 minor 中に promote した obj。 minor 終了時に WB scan
- gc_flags の bit layout: MARKED, OLD, DIRTY, FREE, AGE×2

#### 2.4.4 アルゴリズム詳細

- alloc: 2.3 と同じ + `young_push` で young 追跡
- minor:
  1. AROH_VISIT_ROOTS → mark_edge (= young のみ mark、 old は早期 return)
  2. **remset compaction**: 各 entry h について scan_outgoing。 scan 中に
     mark_value が `scan_saw_young` flag を set。 flag 立ってれば h を keep
     (= dirty 維持)、 false なら CLR_DIRTY して drop
  3. process_gray で transitive mark
  4. finalize_walk
  5. sweep_young_minor: marked + age<PROMOTE_AGE は age++ (stay young)、
     marked + age>=PROMOTE_AGE は promote (set OLD、 age 0)。 promoted は `promoted_buf` に積む
  6. **promote-time WB**: 各 promoted obj について edge scan、 まだ young な child
     があれば SET_DIRTY + remset_push (= N-survive で必要な GC 内 WB)
- major: roots を起点に全 obj mark → sweep_young_major (= 全 marked young を
  unconditional promote) + sweep_old_pages (= unmarked old を free)
- WB: bit-in-head (gc_types.h の `ARO_GC_WB_OLD_MASK = 0x02`)。 inline fast path

### 2.5 mark_gen_inc

#### 2.5.1 概要

`mark_gen` + incremental major marking。 major mark を `inc_step` で chunk 化、
allocator から定期的に呼ばれる。 SATB barrier で 「marking 開始時の reachable set」
を保つ。

#### 2.5.2 パラメータ

- 2.4 と同じ
- `INC_WORK_PER_ALLOC = SIZE_MAX` — 現状 1 回で gray を drain (= STW 相当)。 true
  incremental は VALUE-stack WB 必要、 todo.md 参照

#### 2.5.3 ヒープ構造

- 2.4 と同じ
- 追加: `inc_marking` flag、 `mark_value_satb` (= SATB の旧値 mark)

#### 2.5.4 アルゴリズム詳細

- minor: 2.4 と同じ
- major: `inc_start_major` (= roots mark + flag set) → 各 alloc で `inc_step` → drain 完了で `inc_finish_sweep`
- WB: SATB (= overwrite される旧値を mark) + 通常の remset push
- WB は full out-of-line (= gc_types.h は `ARO_GC_WB_OLD_MASK` 未定義)

### 2.6 copy

#### 2.6.1 概要

Cheney semi-space copying GC。 2 つの tenured semi-space を切り替え。 generational
なし。

#### 2.6.2 パラメータ

- 2 つの semi-space。 各 `64 GiB` 仮想 (= `ARO_GC_REGION_VIRT_BYTES`)、
  `MAP_NORESERVE` で lazy paged

#### 2.6.3 ヒープ構造

- `tenured_base` / `tenured_top` / `tenured_end` — active region
- `tenured_alt_base` — 非 active (to-space at major)
- 8 B header + 8 B fwd overlay (= forward 時に旧 payload[0..8] に new addr 上書き)

#### 2.6.4 アルゴリズム詳細

- alloc: `tenured_top` を bump。 region 越えで `aro_gc_collect`
- collect (= major): ① to_top = tenured_alt_base → ② roots を forward (= memcpy +
  HDR_SET_FORWARDED + fwd_overlay_set) → ③ Cheney scan-loop (= forwarded obj の
  edges を forward) → ④ swap

### 2.7 copy_gen

#### 2.7.1 概要

世代別 Cheney + N-survive。 4-面 layout: 2 young halves + 2 tenured halves。
詳細は `runtime/precise_gc/gc_copy_gen.c` の冒頭 comment。

#### 2.7.2 パラメータ

- `YOUNG_BYTES = 16 MiB` × 2 (= active + alt)
- `TENURED_BYTES = 64 GiB` × 2
- `PROMOTE_AGE = 3`
- `MAJOR_THRESHOLD_FACTOR = 2`

#### 2.7.3 ヒープ構造

- `young_active_base` / `young_top` — alloc 領域
- `young_alt_base` — minor の to-space
- `tenured_base` / `tenured_top` — active tenured (= 4-面 の 1 面)
- `tenured_alt_base` — major の to-space
- `remset_buf[]` — flat array (= 仕様 MAX 128 K entries 超で overflow flag)
- AGE bits in `gc_flags` bits 3-4

#### 2.7.4 アルゴリズム詳細

- alloc: young_top bump。 fill で `nursery_collect_cold` (= major XOR minor、 詳細は
  source comment)
- minor:
  1. Phase 1: roots → forward via `forward_edge_minor` (= 同時に scan_saw_young
     を flag)
  2. Phase 2: 既存 remset を in-place 圧縮。 各 entry scan、 scan_saw_young で keep/drop
  3. Phase 3: 2 つの Cheney scan cursor (= young-to + 新規 promoted tenured) で
     並行 scan。 promoted で young ref が残れば SET_DIRTY + remset_push (= GC 内 WB)
  4. young swap (= alt が次の active)
- major: roots → forward (= young + from-tenured → to-tenured) → Cheney scan → swap
- WB: bit-in-head (`OLD = 0x01, DIRTY = 0x02`)

### 2.8 copy_gen_inc

`copy_gen` と同実装。 真の incremental Cheney は未実装 (= placeholder)。

### 2.9 mark_compact

#### 2.9.1 概要

Lisp-2 sliding compactor: ① mark → ② forward-addr 計算 → ③ 全 pointer 更新 →
④ memmove で一括 slide。

#### 2.9.2 パラメータ

- `REGION_BYTES = 64 GiB` 仮想 (= 単一 region)

#### 2.9.3 ヒープ構造

- 1 つの region (= `tenured_base` / `tenured_top`)
- 16 B header (= `ARO_GC_HAS_FWD` 定義、 専用 `gc_fwd` field 持ち)
- mark / OLD / DIRTY は gc_flags の bit

#### 2.9.4 アルゴリズム詳細

- alloc: bump pointer
- collect: ① roots mark + process_gray、 ② forward-addr (= 各 alive obj に new
  addr を `gc_fwd` 経由保存)、 ③ 全 alive obj の edges を fwd_addr へ update、
  ④ slide (= run-based memmove)、 ⑤ tenured_top = fwd
- 「run-based memmove」 = 連続 marked 列を 1 回の memmove で動かす最適化

### 2.10 mark_compact_gen

#### 2.10.1 概要

nursery 4-面 + tenured Lisp-2 slide。 minor は N-survive Cheney、 major は young
を force_promote → mark + slide。

#### 2.10.2 パラメータ

- `YOUNG_BYTES = 16 MiB` × 2
- `TENURED_BYTES = 64 GiB` 単一
- `PROMOTE_AGE = 3`

#### 2.10.3 ヒープ構造

- 2.7 と同じ young 構造 + 2.9 と同じ tenured 構造
- 16 B header (= `gc_fwd` あり)

#### 2.10.4 アルゴリズム詳細

- minor: 2.7 と同じ流れ (= 4-面 Cheney + remset 圧縮 + promote-time WB)
- major: ① `major_fold_young` (= `force_promote = true` で全 young を tenured に
  Cheney copy + remset 全 scan) → ② mark → ③ Lisp-2 slide
- WB: bit-in-head (`OLD = 0x02, DIRTY = 0x04`)

### 2.11 mark_bump_gen

#### 2.11.1 概要

bump nursery + bump tenured + linear sweep。 移動 (= nursery promote) のみ、
tenured は free しない (= bump pointer 戻らず、 線形 sweep で mark bit 確認)。

#### 2.11.2 パラメータ

- `YOUNG_BYTES = 16 MiB` × 2
- `TENURED_BYTES = 64 GiB` 単一
- `PROMOTE_AGE = 3`

#### 2.11.3 ヒープ構造

- 4-面 young + 1 個 tenured bump region
- 8 B header + fwd overlay
- AGE bits in gc_flags

#### 2.11.4 アルゴリズム詳細

- minor: 2.7 と同じ
- major: ① major_fold_young (= force_promote、 marked young → tenured) → ② mark +
  linear sweep (= tenured を line scan、 unmarked old を free。 tenured_top は戻らない)
- WB: bit-in-head

### 2.12 immix

#### 2.12.1 概要

Immix line/block mark-region。 32 KiB block × 128 B line。 line 単位の mark で
hole (= unmarked line runs) を見つけて bump alloc。 非 moving (= evacuation なし、 v1)。

#### 2.12.2 パラメータ

- `BLOCK_BYTES = 32 KiB`
- `LINE_BYTES = 128`
- `LINES_PER_BLOCK = 256`
- `ARENA_BYTES = 64 GiB` 仮想
- `MEDIUM_MAX = BLOCK_BYTES / 2`

#### 2.12.3 ヒープ構造

- `arena_base` — bump 領域 (= 64 GiB virt)
- `blocks[]` — block ごとの `BlockMeta { state, line_marks[LINES_PER_BLOCK] }`
- block state: `BLK_FREE` / `BLK_RECYCLABLE` (= hole あり) / `BLK_USED` (= 全 line marked)
- `cur_ptr` / `cur_end` — 現在 alloc 中の hole 範囲
- `cur_epoch` — mark epoch (= 1..255 で巡回、 0 reserved)
- `LargeObj` linked list で `> MEDIUM_MAX` は個別 mmap

#### 2.12.4 アルゴリズム詳細

- alloc: `cur_ptr` を bump。 hole 末端で `find_hole` (= 次の hole 探索)。 hole なしで
  `aro_gc_collect`
- collect: ① roots → mark (= epoch == cur_epoch で済印)、 line_marks 設定、 ②
  sweep (= block_state 再計算)、 ③ cur_epoch++
- 非 moving、 fragmentation 対策は line 単位 hole alloc に依存

### 2.13 immix_gen

#### 2.13.1 概要

4-面 nursery + Immix tenured。 minor で N-survive Cheney、 promote 先は Immix の
hole_alloc。 major は force_promote_all + Immix line-mark sweep。

#### 2.13.2 パラメータ

- 2.7 の young + 2.12 の tenured

#### 2.13.3 ヒープ構造

- 2 つの young half (= `young_active_base` + `young_alt_base`)
- 2.12 の Immix tenured (= arena + blocks meta)
- gc_flags layout: EPOCH × 8 bits, OLD bit 8, DIRTY bit 9, FORWARDED bit 10,
  AGE bits 11-12

#### 2.13.4 アルゴリズム詳細

- minor:
  1. roots → forward via `forward_payload_nursery`。 age check で young-to (= alt
     half) か Immix tenured (= `hole_alloc_header`) へ
  2. remset 圧縮 (= scan_saw_young で keep/drop)
  3. Cheney scan: gray queue で young-to + promoted を scan。 in_arena 判定で
     promoted obj だけ scan_saw_young → SET_DIRTY + remset_push
  4. young swap
- major: ① force_promote=true で minor を呼んで全 young を tenured に fold → ②
  全 line_marks zero → ③ roots mark + line_marks 更新 → ④ Immix sweep
- WB: bit-in-head (`OLD = 0x0100, DIRTY = 0x0200`)

### 2.14 mark_bitmap_gen

#### 2.14.1 概要

`mark_gen` と同じ意味 (= sticky mark + 2-gen + remset)。 構造変化:
- 8 B header (= mark/old/dirty bit を per-page bitmap に移動、 header 縮小)
- page は 16 KiB **aligned** mmap (= `page_of(obj) = obj & ~16383`)
- N-survive は実装中 / debug 中。 現状 first-survival で動作

#### 2.14.2 パラメータ

- `NUM_SIZE_CLASSES = 9`、 `PAGE_SIZE = 16 KiB`
- `MAX_SLOTS_PER_PAGE = 512`
- `BITMAP_BYTES = 64`、 3 bitmap (= mark/old/dirty) × 64 B = 192 B / page

#### 2.14.3 ヒープ構造

- `page_head[]` — 16 KiB-aligned mmap 直で取得
- Page: `{ next, class_idx, n_slots, mark_bm[64], old_bm[64], dirty_bm[64] }` (= 208 B header) + slots
- 8 B `AroObjectHeader` (= `flags`, `gc_flags`, `gc_size`)
- LargeObj: side-struct で `mark/old/dirty` を flag 持ち

#### 2.14.4 アルゴリズム詳細

- alloc: 2.3 と同じ slab/page。 加えて `page_of(obj)` 経由で bitmap index 計算
- minor: roots mark → remset scan (= 全 entry を bm_clr dirty + scan) → process_gray
  → finalize_walk → sweep_page (= marked young → set OLD bm)
- major: roots mark → finalize_walk → sweep (= unmarked old を free)
- WB: out-of-line (`ARO_GC_WB_OLD_MASK` 未定義、 bitmap 経由 `get_old`/`get_dirty`)

備考: N-survive 化は `mark_gen` と同じ logic を移植すると小規模 stress test は
通るが、 baruby `T_gc_big` など特定パターンで dangling pointer (= 別 BaArray が
freed BaArrayItems を参照) が発生する。 原因未特定 (= 同じ scan_saw_young flag
の write を加える/外すで挙動が分岐するが、 in-place compaction の有無やオーダー
変更ではなく、 別要因)。 当面 first-survival を維持。

### 2.15 mark_card_gen

`mark_bitmap_gen` と同じ + per-page `card_dirty` flag。 page 単位の dirty
tracking で remset overflow 時の scan を絞り込む。 N-survive 化は 2.14 と同じ
事情で deferred。

### 2.16 mark_freelist

`mark` と類似だが、 freelist 戦略が異なる minimal variant。 generational なし。

### 2.17 copy_scramble

`copy` + per-cycle XOR mask `R` で VALUE storage を撹乱する audit backend。

#### 2.17.1 概要

heap pointer の slot は `raw ^ R` で保存。 各 GC 後に R を rotate。 stale slot
(= sample が `ARO_LOAD` decode を忘れた slot) は次の deref で SEGV 確実。

#### 2.17.2 パラメータ

- 2.6 の copy backend + `ARO_GC_HAS_SCRAMBLE = 1`

#### 2.17.3 ヒープ構造

2.6 と同じ + `scramble_R` / `scramble_R_old` (= 直前と現在の mask)

#### 2.17.4 アルゴリズム詳細

- alloc / collect: 2.6 と同じ
- ただし `ARO_LOAD` と `ARO_GC_VISIT_EDGE` で必ず XOR decode/encode
- 各 collect 後に `scramble_R_old = scramble_R; scramble_R = random()` で rotate
- 推奨: `BARUBY_GC_STRESS=1` と組合せて全 alloc で R rotate (= audit 強化)

## 3. 共通 framework API

### 3.1 sample 側必須

- `AROH_VISIT_ROOTS(c, ctx, edge_visit)` — root scan macro。 sample 固有の root
  (= eval stack, globals 等) を `edge_visit` で walk
- `AROH_SCAN_EDGES(payload, payload_size, ctx, edge_visit)` — obj の outgoing
  edges を walk。 sample が type tag で dispatch
- `AROH_FINALIZE(payload)` — finalize hook (= mpz_clear 等)、 sample 不要なら no-op
- `AROH_IS_GC_OBJECT(v)` — VALUE が GC managed heap pointer かの predicate

### 3.2 framework 提供

- `aro_gc_alloc(c, size)` — scan-safe alloc (= zero-init)
- `aro_gc_alloc_byte(c, size)` — raw byte alloc (= zero-init なし、 sample が即 fill)
- `aro_gc_realloc_payload(c, p, new)` — sample 側で実装 (= sp slot park パターン)
- `aro_gc_wb(c, holder, slot, val)` — write barrier
- `aro_gc_wb_bulk(c, holder, dst, src, n)` — bulk WB
- `aro_gc_collect(c)` — 強制 collect (= 通常 major)
- `aro_gc_account_external(c, delta)` — GMP buffer 等 external 量を framework に
  通知 (= 閾値超で GC 発火)
- `aro_gc_finalize_register/check/walk/fini`

### 3.3 backend hook

backend は `gc_*.c` で以下を実装:
- `aro_gc_init(c)`, `aro_gc_fini(c)`
- `aro_gc_alloc_raw(c, size)` / `aro_gc_alloc_byte_raw(c, size)` (= encode は framework が wrap)
- `aro_gc_collect(c)` (= 強制 major)
- `aro_gc_size_of(payload)`
- `aro_gc_finalize_check(c, payload)`
- WB: `ARO_GC_HAS_WB` 定義時に `aro_gc_wb` (out-of-line) or `aro_gc_remember` (inline fast path + cold extern)

詳細 layering は `runtime/precise_gc/gc.h` と `gc_types.h` の冒頭 comment を参照。
