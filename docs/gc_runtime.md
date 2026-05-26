# ASTro precise GC runtime: backend ガイド

ASTro `runtime/precise_gc/` には 16 個の GC backend が同居しており、 build 時に
`-DBARUBY_GC=<n>` で切替する (build option ID 1..17 のうち ID 7 は予約 hole)。 すべて同じ framework API (= `aro_gc_alloc`,
`aro_gc_wb`, `aro_gc_collect`, `AROH_VISIT_ROOTS`, `AROH_SCAN_EDGES`,
`aro_gc_finalize_*`) を実装し、 sample 側は backend 切替に透明。

このドキュメントは各 backend の動作を 1 か所にまとめた reference。 性能比較は
`docs/perf.md` と `sample/*/docs/perf.md` を、 framework の設計思想は
`docs/gc_design.md` を参照。 本 doc では 各 backend の実装に踏み込み、
データ構造 / アルゴリズム / finalizer / heap 拡張ポリシーを揃って記述する。

## 1. 概要

### 1.1 サポートしている GC アルゴリズム一覧

実用 GC algorithm (§2):

| §    | 名称 | 特長 |
|------|------|------|
| 2.1  | `mark` | mark&sweep。 9 size class slab + page。 generational なし |
| 2.2  | `mark_gen` | mark&sweep + 2-gen + N-survive (= age 0..3, promote on 4th survival) |
| 2.3  | `mark_gen_inc` | `mark_gen` + incremental marking (SATB barrier)。 ただし INC_WORK_PER_ALLOC=SIZE_MAX で事実上 STW |
| 2.4  | `copy` | Cheney semi-space copying。 8 B fwd overlay (= no `gc_fwd` field) |
| 2.5  | `copy_gen` | Cheney + 2-gen + 4-面 layout (= 2 young halves + 2 tenured halves) + N-survive |
| 2.6  | `mark_compact` | Lisp-2 sliding compactor (mark + fwd-addr + update-ptr + slide) |
| 2.7  | `mark_compact_gen` | Cheney nursery (= active + alt) + tenured Lisp-2 slide。 N-survive |
| 2.8  | `immix` | Immix line/block mark-region。 32 KiB block × 128 B line。 非 moving |
| 2.9  | `immix_gen` | Cheney nursery (= active + alt) + Immix tenured。 N-survive |
| 2.10 | `mark_bitmap_gen` | `mark_gen` と意味同等、 metadata を per-page bitmap 化 (= 8 B header)。 N-survive |
| 2.11 | `mark_card_gen` | `mark_bitmap_gen` + per-page card-dirty flag (= page 単位 remset)。 N-survive |

特殊用途 backend (§3):

| §   | 名称 | 特長 |
|-----|------|------|
| 3.1 | `none` | 解放しない。 malloc + leak。 GC overhead = 0 の baseline |
| 3.2 | `bump` | bump allocator のみ。 解放しない (= `none` strictly faster baseline) |
| 3.3 | `mark_bump_gen` | Cheney nursery (= active + alt) + bump tenured + linear sweep。 tenured は monotonic 増加 (= sweep が free しない testbed)。 N-survive |
| 3.4 | `mark_freelist` | mark&sweep、 region + size-class freelist。 fragmentation 対策なし (= no coalescing / size-class isolation / region_top never retreats) の fragmentation testbed |
| 3.5 | `copy_scramble` | `copy` + per-cycle XOR mask R で VALUE storage を撹乱。 audit / debug backend |

8 個の `_gen` backend (mark_gen, mark_gen_inc, copy_gen,
mark_compact_gen, mark_bump_gen, immix_gen, mark_bitmap_gen, mark_card_gen) は
全て N-survive (PROMOTE_AGE = 3、 4 回目の survival で promote) に統一済 (=
commit a8914250 で完了)。 first-survival promote の variant は現存しない。

### 1.2 用語

- **young / nursery** — 新規 alloc 領域 (= 短命オブジェクト向け、 minor GC のみで回収)
- **tenured / old** — 長命オブジェクト領域 (= 通常 major GC のみで回収)
- **promote** — young → tenured への昇格。 N-survive (= N 回生き残ると promote) に統一
- **N-survive** — `age` bits を header に持ち、 marked young の age を minor 毎に inc。 age >= `PROMOTE_AGE` (= 3) で promote
- **minor GC / major GC** — minor は young のみ、 major は heap 全体を対象とする GC
- **WB (write barrier)** — heap-pointer 書込で remset に holder を追加する hook。 `aro_gc_wb(c, holder, slot, val)`
- **remset (remembered set)** — tenured→young pointer を持つ tenured obj の集合。 minor mark phase で remset entry を scan することで young 子孫を到達可能にする
- **SATB (snapshot-at-the-beginning)** — incremental marking で overwrite 直前の旧値を mark する WB 方式
- **dirty bit** — tenured obj が remset に居る印。 WB で set、 minor の remset scan 後に対象が young child を失えば clear
- **remset 整理** — minor 中に remset_buf を 1 pass 走査して、 「もう young child を持たない entry」 を drop し残った entry を buf の先頭に詰める処理。 具体的には ① 各 entry の edges を scan し young 子があれば forward + `scan_saw_young` を立てる、 ② flag が false なら entry を drop + DIRTY clear、 true なら keep。 結果として remset_buf は 「(元の dirty 集合) → (minor 後もまだ young (= to-space) への参照を持つ tenured obj の集合)」 に shrink する
- **mark bit** — mark phase で訪問済の印。 sweep 後に clear (= incremental では epoch 方式で再利用)
- **sticky mark** — major でも mark bit を残し、 minor では「mark bit が無い young が dead」という判定に使う方式 (= `mark_gen` 系)
- **Cheney** — semi-space copying GC アルゴリズム。 from-space を to-space に forward しながら scan
- **semispace** — Cheney copying GC が使う、 同サイズの heap 領域 2 つの片方 (= 一方の region)。 「2 つの semispace で 1 組」 が Cheney の基本構成、 GC ごとに役割を入れ替える (= flip)。 文献では "semi-space" とも書く
- **active / alt (alternate)** — Cheney 構成の 2 つの semispace を GC **外** で区別する呼び名。 active = 今 alloc に使っている semispace、 alt = もう片方 (= 次の GC で to-space になる)。 GC 末で `swap(active, alt)` で役割が入れ替わる
- **from-space / to-space** — Cheney 構成の 2 つの semispace を GC **中** で区別する呼び名。 from-space = GC 開始時点の active (= 中身を読出して copy 元になる)、 to-space = alt (= copy 先)。 GC 完了時に from-space は破棄され、 to-space が次の active になる
- **4-面 layout** — 世代別 copying GC で「young 用 Cheney (= active + alt の 2 領域) + tenured 用 Cheney (= active + alt の 2 領域)」 計 4 領域を持つ構成。 minor は young の active/alt で flip、 major は tenured の active/alt で flip + young は全 promote で空に
- **Lisp-2 slide** — non-copying sliding compactor。 mark → forward-addr 計算 → 全 pointer 更新 → 一括 memmove
- **Immix line / block** — Immix の領域単位。 block (= 32 KiB) ごとに line (= 128 B) の mark bit を持ち、 line 単位で hole alloc
- **FORWARDED bit** — moving GC で 「この obj は new addr に copy 済」 マーク。 詳細位置は backend ごと
- **fwd overlay** — `HDR_FORWARDED` 設定後、 旧 obj の payload[0..8] に new addr を上書き保存する手法 (= 専用 `gc_fwd` field 不要)
- **force_promote** — major で young を強制 promote するための flag (= 世代別 backend で major fold-young 時に true)
- **scramble** — `copy_scramble` の per-cycle XOR mask。 stale slot を SEGV で検出するための audit 機能
- **AGE bits** — gc_flags 内の 2 bit (= 値域 0..3)。 N-survive で promote 判定に使う
- **promote-time WB** — minor 中の promote で生じる tenured→young 辺を GC 自身が remset へ push する処理 (= ユーザ WB は user write しか拾えない)

### 1.3 sample と GC backend の対応

ASTro 全 sample (= 24 個) の GC 方式を分類:

#### 1.3.1 precise_gc を使う sample (= 本 doc の対象、 2 個)

`runtime/precise_gc/` を `make GC=<name>` で build-time 切替する。 16 backend 全てに対応。

| sample | 位置 | 用途 | default `GC` |
|--------|------|------|--------------|
| `baruby_precise` | `sample/baruby_precise/` | naruby fork + Array/String + precise rooting。 **GC algorithm 比較 testbed** (本 doc の primary 対象) | `copy` |
| `ascheme_precise` | `sample/ascheme_precise/` | Scheme サブセット (= `ascheme`) の precise rooting 版。 GC kernel 単体測定 + 細粒度 alloc bench に有用 | `copy` |

備考:
- bench matrix (`sample/<sample>/benchmark/`) は 16 × {plain, AOT cached} の組合せで回す
- `BARUBY_GC_STRESS=1` で全 alloc 毎に GC 発火 stress mode、 `BARUBY_GC_PURGE=1` で zero-fill audit
- 本 doc は §2 / §3 でこの 16 backend を解説

#### 1.3.2 libgc (= Boehm-Demers-Weiser conservative GC) を使う sample (10 個)

リンク時に `-lgc` で `libgc` を呼び出し、 `GC_MALLOC` で alloc。 mark phase は stack を保守的に scan するため precise rooting 不要。

`arawk` / `ascheme` / `asom` / `astocaml` / `astr` / `baruby` / `koruby` / `nuq` / `pascalast` / `pystro`

備考:
- `baruby` は `baruby_precise` の libgc 比較対象 (= bench v11 で `libgc` 列として並列)
- conservative ゆえに「pointer に見える int」 を保守的に alive 扱いする overhead がある
- 本 doc では扱わない (= 別 GC framework)

#### 1.3.3 独自 GC を持つ sample (2 個)

| sample | GC 実装 |
|--------|---------|
| `jstro` | hidden class IC + runtime GC (= `sample/jstro/js_runtime.c`) |
| `luastro` | Lua 流の mark-sweep (= `sample/luastro/lua_gc.c`) |

precise_gc framework と独立。 言語仕様に依存した GC pattern (= JS の hidden class、 Lua の userdata finalizer) のため共通化が難しく独自実装。

#### 1.3.4 CRuby GC を継承する sample (= CRuby C 拡張、 2 個)

CRuby に組み込む C 拡張形式で、 alloc / mark は CRuby (= MRI) の GC に委ねる。

| sample | 形態 |
|--------|------|
| `abruby` | Ruby サブセット を CRuby 拡張として組込み |
| `arjsv` | JSON Schema validator、 `json_schemer` 互換 API |

#### 1.3.5 GC 不要 sample (8 個)

理由は 2 系統:

- **そもそも heap object allocation を持たない**: `naruby` (= Ruby サブセット、 整数 / 浮動小数 / closure 等を frame 上で扱い、 heap obj を作らない設計)、 `aforth` (= stack machine、 値は stack に積むだけ)、 `calc` (= 6-node 電卓、 整数のみ)
- **short-lived eval で leak-and-forget**: `arcel` (= predicate eval CLI) / `asml` (= ML 評価) / `astrogre` (= regex + grep CLI) / `castro` (= C eval) / `wastro` (= Wasm 評価)。 process 終了で OS に解放される前提で free しない

## 2. 各アルゴリズム紹介

§2 には 11 個の実用 GC algorithm を記載 (= 旧 13 個から `mark_bump_gen` と `mark_freelist` を §3 特殊用途に移動済、 build option ID 7 は将来追加時のために予約 hole)。

各 backend は 5 subsection に分けて記述する:

- **2.x.1 概要** — 一行 summary + 主な特徴
- **2.x.2 パラメータ** — heap サイズ / 閾値 / age 等の compile-time 定数
- **2.x.3 データ構造** — heap layout + remset / gray queue / promoted_buf / finalize_list 等
- **2.x.4 アルゴリズム詳細** — phase 毎の input/output + 計算量
- **2.x.5 finalizer 実装** — register / walk / check の挙動

### 2.1 mark

#### 2.1.1 概要

非世代の mark&sweep。 9 size class (= 32, 64, ..., 4096 B) の slab + page 構造
+ large obj 個別 mmap。

#### 2.1.2 パラメータ

- `NUM_SIZE_CLASSES = 9`、 `size_class_bytes = {32, 64, ..., 4096}`
- `PAGE_SIZE = 16 KiB`、 `PAGE_HDR_BYTES = 16`
- `GC_THRESHOLD_MIN = 16 MiB`、 `GC_THRESHOLD_FACTOR = 2`
  (= 次回閾値は `MAX(MIN, live × 2)`)
- large 閾値: > 4 KiB は個別 mmap

#### 2.1.3 データ構造

**Heap layout** (= alloc 領域全体の構造):
- `page_head[9]`: class 別 page chain (= 単方向 linked list)
- `Page` header (16 B): `{Page *next, class_idx, _pad}`、 続いて slots
- `LargeObj` linked list: `{LargeObj *next, map_bytes}` + payload (= 個別 mmap)。 sweep 時は head から全走査 → unmarked は `munmap` で chain から外す、 marked は mark clear。 cost O(num_large)

**Object layout** (= 個別 obj の構造):
- 8 B `AroObjectHeader`
- `gc_flags` bit: MARKED=0x1, FREE=0x2

**Allocator state** (= freelist / bump pointer 等の alloc state):
- `freelist[9]`: class 別 free slot chain。 `FreeSlot.next` が
  payload offset 8 (= head 直後) に overlay

**GC 中の auxiliary 構造** (= gray queue 等):
- `gray_buf[]` (= flat array of `AroObjectHeader *`)。 初期 cap 0、
  256 → 2× で `realloc`。 overflow なし (libc が abort)

**共通**: finalize_list (= gc_common.c 管理)

#### 2.1.4 アルゴリズム詳細

phases:
1. **alloc**: size class 決定 (`size_class_for` = clz-based O(1)) → freelist pop → 無ければ `new_page` で mmap し slot を freelist に積む。 large は `large_alloc` で個別 mmap。 `bytes_since_gc` 加算 → 閾値超で `gc_collect_internal`
2. **mark**: `AROH_VISIT_ROOTS` → `mark_edge` → `mark_value` で MARKED + gray push → `process_gray` で `AROH_SCAN_EDGES` を transitive 適用
3. **finalize_walk**: 後述 2.1.5
4. **sweep**: 全 page を class 別に slot prefix 走査。 unmarked → `HDR_FREE_BIT` set + freelist push、 marked → mark clear。 large list は unmarked を `munmap`

計算量:
- alloc: O(1) amortized (= freelist pop)、 cold path で mmap O(page setup)
- collect: mark O(live edges)、 sweep O(heap slots)。 トータル O(heap)
- WB hot path: 0 (= no WB)

**GC 発火閾値の決め方** (= adaptive heuristic "live × 2"):
- 初期値 `gc_threshold = GC_THRESHOLD_MIN = 16 MiB`
- alloc 時に `bytes_since_gc + external_bytes + payload > gc_threshold` で GC 発火
- collect 末に `live_bytes` を計測し、 `gc_threshold = max(GC_THRESHOLD_MIN, live_bytes * GC_THRESHOLD_FACTOR)` で次回閾値を更新 (= FACTOR=2)
- 結果として heap は使用量に追従して伸縮 (= live が増えれば threshold も増、 減れば 16 MiB floor まで落ちる)。 固定値ではなく動的

heap growth/shrink: page は閾値超で `new_page` (= mmap)。 sweep で全 unmarked
になっても page は munmap しない (= 物理 release は `mark_freelist` と同様未実装)。
large obj は unmarked で munmap。 後続 backend で「2.1 と同じ閾値式」 と書く
場合は本式を指す。

#### 2.1.5 finalizer 実装

`aro_gc_finalize_check` は MARKED bit のみ判定 (= 非 moving、 payload 不変)。
`aro_gc_finalize_walk` を sweep 直前に呼ぶことで、 sweep が slot を freelist
に戻す前に sample 側 `AROH_FINALIZE` (= mpz_clear 等) が old payload に
access できる。

### 2.2 mark_gen

#### 2.2.1 概要

`mark` + 2-gen + N-survive。 minor は `young_objs[]` array で young を追跡 →
sweep_young は O(young)。 major は全 page walk + `young_objs` reset。

#### 2.2.2 パラメータ

- 2.1 と同じ slab/page + `PROMOTE_AGE = 3`
- `young_threshold = 16 MiB` (fixed)
- `MAJOR_THRESHOLD_MIN = 16 MiB`、 `old_major_threshold = MAX(MIN, old_bytes × 2)`
- `MAX_REMSET_ENTRIES = 1 << 17` (= 128 K entries = 1 MiB ptr array)

#### 2.2.3 データ構造

**Heap layout**:
- 2.1 の slab/page/large 構造 (= `page_head[9]` + `Page` 16 B header + `LargeObj` linked list)

**Object layout**:
- 8 B `AroObjectHeader`
- `gc_flags` bit layout: MARKED=0x1, OLD=0x2, DIRTY=0x4, FREE=0x8, AGE=bits 4-5

**Allocator state**:
- `freelist[9]`: class 別 free slot chain (= 2.1 と同形式)

**GC 中の auxiliary 構造**:
- **`young_objs[]`**: flat array of `AroObjectHeader *`、 alloc 時に push、
  sweep_young_minor で 1 pass scan + 不要 entry を drop して残りを array
  先頭に詰める処理 (= remset 整理 と同形式の array 操作。 memory compaction
  ではない)。 cap 0 → 1024 → 2× で realloc。 これにより `sweep_young_minor`
  は O(young live) で済む (= 全 page walk を回避)。 設計判断の議論は §6
  Q1 参照。
- **`gray_buf[]`**: mark 用の grey queue、 同じ grow 戦略
- **`remset_buf[]`**: tenured DIRTY 用 flat array。 256 → 2× で grow、
  上限 `MAX_REMSET_ENTRIES`。 上限超で `remset_overflow = true` を立て
  以後 push は no-op → minor で全 page heap-walk fallback (`remset_heap_walk`)
- **`promoted_buf[]`**: 当該 minor で promote した obj の append-only。
  256 → 2× で grow、 各 minor 末で 0 reset

**共通**: finalize_list

#### 2.2.4 アルゴリズム詳細

minor phase:
1. `AROH_VISIT_ROOTS` → `mark_edge` (in_minor=true、 OLD なら早期 return、
   target が young なら `scan_saw_young=true`)
2. **remset compaction**: 各 entry h について `scan_outgoing` 呼出 +
   `scan_saw_young` 観察。 立てば h を keep (= DIRTY 保持)、 false なら
   `HDR_CLR_DIRTY` して drop。 remset_overflow 時は `remset_heap_walk` で
   全 page から `OLD && DIRTY` を再列挙し同じ処理
3. `process_gray` で transitive mark
4. `aro_gc_finalize_walk`
5. `sweep_young_minor`: marked + age<PROMOTE_AGE は age++ + young keep、
   marked + age>=PROMOTE_AGE は `HDR_SET_OLD` + age=0 + `promoted_push`、
   unmarked は `free_slot` (= freelist 戻し)
6. **promote-time WB**: 各 promoted obj について `check_edge_for_young` を
   `AROH_SCAN_EDGES` で適用、 outgoing edge に young child があれば
   `HDR_SET_DIRTY` + `remset_push`

major phase:
1. mark roots + process_gray (in_minor=false、 OLD 含め全 obj mark 可)
2. finalize_walk
3. `sweep_young_major`: marked young を unconditional promote (= age リセット、
   MARKED は残す → sweep_old_pages が unmarked-old と区別)
4. `sweep_old_pages`: 全 page を slot 走査、 unmarked OLD を free、 marked
   OLD は MARKED + DIRTY clear

計算量:
- minor: O(young live + remset_entries × edges)
- major: O(heap slots + live edges)
- WB hot path: bit-in-head `(gc_flags & (OLD|DIRTY)) != OLD` 1 cmp + 1 jcc、
  cold path は `aro_gc_remember` (= SET_DIRTY + remset_push) 関数 call

**GC 発火閾値の決め方** (= 世代別 backend 共通パターン):
- **young**: `young_threshold` は **固定 16 MiB** (= 動的に伸ばさない、 nursery は worst-case 必ず 1 回 promote されるための上限として固定)
- **major**: `old_major_threshold` は 2.1 と同じ live × 2 heuristic (= 初期 `MAJOR_THRESHOLD_MIN = 16 MiB`、 major 終了時 `max(MIN, tenured_live * 2)` で更新)
- minor / major の選択: alloc 時 ① `old_alloc_since_major > old_major_threshold` または `external_bytes > old_major_threshold` → major、 ② young 側超過 → minor。 後続 gen backend で「同じ」 と書く場合は本式を指す

heap growth/shrink: page-on-demand alloc (= freelist 空で new_page)。 page
の release は未実装 (= 完全に空になっても保持)。 large obj は free 時 munmap。

#### 2.2.5 finalizer 実装

`aro_gc_finalize_check`:
- minor (in_minor=true): OLD ならば conservatively alive、 MARKED ならば alive、
  どちらでもなければ dead
- major: MARKED のみで判定

非 moving なので payload 不変 (= 戻り値はそのまま入力 payload)。

### 2.3 mark_gen_inc

#### 2.3.1 概要

`mark_gen` + incremental major marking。 major mark を allocator から定期的に
呼ぶ `inc_step` で chunk 化、 SATB barrier で「marking 開始時の reachable
set」を保つ。 **現状 INC_WORK_PER_ALLOC = SIZE_MAX のため 1 alloc で gray を
drain しきる**= 実質 STW。 マルチセグメント pause 計測の structure だけは
入っている。

#### 2.3.2 パラメータ

- 2.2 と同じ
- `INC_WORK_PER_ALLOC = SIZE_MAX` (= 事実上 STW)
- 真の incremental には VALUE-stack WB が必要 (= 未実装、 todo.md 参照)

#### 2.3.3 データ構造

**Heap layout**: 2.2 と同じ (= slab/page/large)

**Object layout**: 2.2 と同じ (= 8 B header + gc_flags MARKED/OLD/DIRTY/FREE/AGE)

**Allocator state**: 2.2 と同じ (= `freelist[9]`)

**GC 中の auxiliary 構造**:
- 2.2 と同じ (= `young_objs[]`, `gray_buf[]`, `remset_buf[]`, `promoted_buf[]`)
- 追加: `inc_marking` flag、 SATB 用 `mark_value_satb`

**共通**: finalize_list

#### 2.3.4 アルゴリズム詳細

minor: 2.2 と同等 (= N-survive remset compaction + sweep_young_minor + promote-time WB)。

major incremental flow:
1. `inc_start_major`: roots を mark + `inc_marking = true` set
2. 各 alloc で `inc_step(budget=SIZE_MAX)`: gray drain。 drain 完了で
   `inc_marking = false`
3. drain 完了直後の alloc で `inc_finish_sweep`: roots を再 scan (= inc
   window 中の新規 root は WB が拾えない)、 finalize_walk、 sweep_young_major、
   sweep_old_pages

WB:
- SATB: `inc_marking` 中は overwrite 直前の旧値を `mark_value_satb` で mark
  (= 強制 gray push、 in_minor filter なし)
- 通常の remset push (= OLD && !DIRTY なら SET + push)
- WB は full out-of-line (= `ARO_GC_WB_OLD_MASK` 未定義、 `aro_gc_wb` は extern)

計算量:
- mark hot path: SATB load + AROH_IS_GC_OBJECT + (mark) — 数命令
- 残りは 2.2 と同等

heap growth/shrink: 2.2 と同じ。

#### 2.3.5 finalizer 実装

2.2 と同じ。 `finalize_walk` は `minor_gc` / `inc_finish_sweep` /
`aro_gc_collect` から呼ばれ、 `inc_step` 中には呼ばない (= mark 不完全な
窓では実行しない)。

### 2.4 copy

#### 2.4.1 概要

Cheney semi-space copying GC。 2 つの tenured semi-space (= 64 GiB 仮想 ×
2 面) を切替。 generational なし。 大 obj (= ≥ 4 KiB) は別途 malloc-backed
非 moving list (= LargeObj) に置く。

#### 2.4.2 パラメータ

- `REGION_BYTES = 64 GiB` 仮想 × 2 面 (`MAP_NORESERVE` で lazy paged)
- stress 時は `STRESS_REGION_BYTES = 64 MiB` (= mmap/munmap を毎 GC 行うため)
- `LARGE_THRESHOLD = 4 KiB`
- `GC_THRESHOLD_MIN = 16 MiB`、 `GC_THRESHOLD_FACTOR = 2`

#### 2.4.3 データ構造

**Heap layout**:
- `space0` / `space1` — 2 面の pre-mmap、 `active_idx` で alternation
- `LargeObj` linked list (= malloc-backed): `{LargeObj *next, LargeObj *next_gray}` + payload

**Object layout**:
- 8 B header + 8 B fwd overlay (= forward 時に旧 payload[0..8] に new addr 上書き)
- `gc_flags` bit: `HDR_FORWARDED = 0x1`、 large 用 `HDR_MARKED = 0x2`

**Allocator state**:
- `active_base` / `active_top` / `active_end` — 現在 alloc 中の semispace の base / bump / 上限

**GC 中の auxiliary 構造**:
- Cheney scratch: `to_top` / `to_base` / `from_base_cur` (= 専用 gray queue なし、 to-space 自体が queue)
- `large_gray` chain (= 非 moving large の gray queue)

**共通**: finalize_list

stress と purge の区別:
- stress (= GC every alloc): space alternation で再利用、 GC 頻度は最大
- purge (= 旧 STRESS 相当): 毎 GC で `mmap_region` 新規 + 旧 region `munmap` (=
  stale slot は SEGV 確実)。 stress と組合せ可能

#### 2.4.4 アルゴリズム詳細

alloc: `gc_bump`。 閾値 / region 超 / stress で `gc_bump_cold` → `gc_collect_internal`。
large は閾値で `large_alloc`。

collect (= 常に major、 single generation):
1. **setup**: `to_base = next_to_base` (= alternation で他面、 purge では fresh mmap)
2. **roots forward**: `AROH_VISIT_ROOTS` で `forward_edge` を全 root に適用 →
   `forward_payload` が ① already-forwarded → fwd_overlay を返す、 ② large + !MARKED → SET_MARKED + `large_gray` push (= 非 moving)、 ③ from-space 内 → memcpy + SET_FORWARDED + overlay
3. **Cheney scan-loop**: `scan` cursor を `to_base` から `to_top` まで進めつつ各 obj の `AROH_SCAN_EDGES` を `forward_edge` で適用。 forward 時に `to_top` が前進 → cursor < to_top の間 loop が継続 → 「forward queue として `to_top` と `scan` cursor の差を使う」が Cheney の核心
4. **large gray drain**: `large_gray` から取り出し scan + to-space 側を再 drain (= 新規 forward が更に large_gray を増やす可能性を考慮)
5. `aro_gc_finalize_walk`
6. **large sweep**: `HDR_MARKED` clear、 unmarked は `free`
7. **swap**: active_idx 反転 (purge では from を `munmap`)

Cheney scan-loop の背景 (Cheney 1970): 明示的な gray queue を持たず、 to-space
自体を queue として使うのが核心。 `forward_payload` は ① 未 forward の obj を
to-space に memcpy + `HDR_FORWARDED` 設定 + fwd_overlay に new addr 書込み +
new addr を返却、 ② forward 済みなら overlay の new addr を返却、 を担う。
`forward_edge` は slot を `forward_payload` の戻り値で書換えるだけ。
`gc_collect_internal` の `scan < to_top` ループは scan cursor を進めながら各
obj の `AROH_SCAN_EDGES` を `forward_edge` で適用 — 書換中に `forward_payload`
が更に `to_top` を前進させる可能性があるため、 「scan cursor が to_top に
追いつく = 閉包に達した」 で自然停止する。 keys は ① obj は最初に touch
された時に 1 回だけ memcpy される (fwd_overlay で再 forward を抑止)、 ②
queue cost ゼロ (= to-space 自身が代行)、 の 2 点。 large obj は移動しないので
別 chain `large_gray` で gray queue を保つ。

計算量:
- alloc: O(1)
- collect: O(live × 1) (= copy + scan、 dead は触らない)
- WB: 0 (= 非 generational)

**GC 発火閾値**: 2.1 と同じ live × 2 heuristic (= 初期 16 MiB MIN、 active
semispace で `gc_bump` が threshold 超 or region 末で発火)。 ただし virtual
予約が 64 GiB なので heap 自体は閾値で grow させない (= virt 上限のみ)。

heap growth/shrink: 64 GiB virt 予約。 grow 不要、 shrink なし (= madvise
DONTNEED は未呼出)。 大 obj だけ free で物理解放。

#### 2.4.5 finalizer 実装

`aro_gc_finalize_check`:
- HDR_FORWARDED → fwd_overlay の new addr を返却 (= 小 obj は移動)
- HDR_MARKED (= large) → payload 自身を返却 (= 非 moving)
- どちらでも無ければ NULL

Moving GC のため、 register 時の payload と finalize_walk 時で addr が変わる
点が非 moving と異なる。 walk は sweep / swap の直前に呼ぶ (= from-space
data がまだ readable)。

### 2.5 copy_gen

#### 2.5.1 概要

世代別 Cheney + N-survive。 4-面 layout: young 用 Cheney (= active + alt の
2 つの semispace) + tenured 用 Cheney (= 同じく active + alt の 2 つ)。 minor
は young の active (= from-space) を alt (= to-space) に Cheney copy しつつ、
age >= PROMOTE_AGE の obj は tenured 側に promote (= tenured も Cheney scan
で並行進行)。 major は tenured の active → alt に Cheney copy しつつ young
は全 promote で空にする。

#### 2.5.2 パラメータ

- `YOUNG_BYTES = 16 MiB` × 2
- `TENURED_BYTES = 64 GiB` × 2 (`MAP_NORESERVE` lazy paged)
- `PROMOTE_AGE = 3`
- `MAJOR_THRESHOLD_MIN = 16 MiB`、 `MAJOR_THRESHOLD_FACTOR = 2`
- `MAX_REMSET_ENTRIES = 1 << 17`

#### 2.5.3 データ構造

**Heap layout**:
- young 用 Cheney (= active + alt の 2 つの semispace、 各 `YOUNG_BYTES`):
  - `young_active_base` — active semispace の base
  - `young_alt_base` — alt semispace の base (= 次 minor の to-space になる)
- tenured 用 Cheney (= active + alt の 2 つの semispace、 各 `TENURED_BYTES`):
  - `tenured_base` / `tenured_end` — active semispace の base / 上限
  - `tenured_alt_base` — alt semispace の base (= 次 major の to-space になる)

**Object layout**:
- 8 B header + fwd overlay
- `gc_flags` layout: OLD=0x1, DIRTY=0x2, FORWARDED=0x4, AGE=bits 3-4

**Allocator state**:
- `young_top` — active young semispace 内の bump pointer (= 次 alloc 位置)
- `tenured_top` — active tenured semispace 内の bump pointer

**GC 中の auxiliary 構造**:
- `remset_buf[]`: flat array of `AroObjectHeader *`、 256 → 2× / 上限
  `MAX_REMSET_ENTRIES`、 `remset_overflow` で heap-walk fallback
- Cheney scratch: `young_from_base/end`、 `young_to_base/top/end`、 `old_tenured_top`、
  `to_base/top`、 `from_base_cur/end_cur`
- `in_minor`、 `scan_saw_young` flags

**共通**: finalize_list

#### 2.5.4 アルゴリズム詳細

alloc: `nursery_bump` → young_active で bump、 size が `YOUNG_BYTES/2` 超なら
`pretenure_alloc` で直接 tenured。 young 閾値超 or external_bytes 過大で
`nursery_collect_cold` → major XOR minor (詳細下記) 経由。

minor phases:
1. **roots forward** (`forward_edge_minor`): young-from を forward。 `forward_obj`:
   - age < PROMOTE_AGE → young-to に copy + age++
   - age >= PROMOTE_AGE → tenured に copy + OLD set + age=0
   - FORWARDED 既設なら fwd_overlay 返却
2. **remset 整理** (= §1.2 用語参照): minor 開始時の
   remset_buf を 1 pass 走査し、 各 entry (= tenured obj) について:
     1. `scan_saw_young = false` に初期化
     2. `AROH_SCAN_EDGES` で entry の全 slot を `forward_edge_minor` 経由 forward
        (= 子が young なら to-space に copy + slot を new addr に更新)
     3. forward 中に「forward 後の new addr も依然 young 領域に居る」 子があれば
        `scan_saw_young = true`
     4. true なら entry を buf の write cursor に keep (= 依然 dirty)、 false なら
        drop + `HDR_CLR_DIRTY` (= もう young child を持たないので remset から除外)
   結果として remset_buf は古い dirty entry を含む長さから、 今 minor 終了時点で
   実際に young child を持つ entry のみに縮む。 remset overflow (= 過去に WB が
   buf を溢れさせた) の場合、 個別 entry の場所が分からないので `tenured_base..
   old_tenured_top` を slot prefix で heap-walk して同等の filter を実行
3. **Cheney scan-loop**: `young_scan` cursor と `tenured_scan` cursor の 2 つ。
   各 round で young_to_top と tenured_top が動かなくなるまで loop。
   `process_object_promoted` (= tenured 側) は scan 後に `scan_saw_young` を
   観察し、 立てば `HDR_SET_DIRTY` + `remset_push` (= **promote-time WB**)
4. `aro_gc_finalize_walk`
5. **young swap**: 旧 alt が次回 active、 旧 active は次回 GC で to-space になる alt

major phases (`major_gc`):
1. `to_base/top = tenured_alt_base`、 `from_base_cur/end_cur = old tenured`
2. roots → `forward_edge_major` で young AND from-tenured を to-tenured へ forward
3. Cheney scan over to-tenured で edges を transitive forward
4. finalize_walk
5. swap: 新 tenured = alt、 young は空 (= 全 promote 済)

minor / major の選択は `nursery_collect_cold` で「tenured worst-case
overflow」「累積 tenured alloc 閾値」「external_bytes 閾値」 の OR で
判定。 DIRTY bit は「remset_buf 在席」のマーカで invariant 維持に使う。
設計上の論点 (DIRTY の意義、 minor/major 選択基準、 major と N-survive の
関係) は §6 Q2 参照。

計算量:
- alloc: O(1)
- minor: O(young live + remset_entries × edges + promoted × edges)
- major: O(tenured live × 1 + young live × 1)
- WB hot path: bit-in-head (= 2.2 と同形式)、 cold path `aro_gc_remember`

**GC 発火閾値**: 2.2 と同じ世代別 pattern (= young 側 `YOUNG_BYTES = 16 MiB`
固定、 tenured 側 `old_major_threshold` は live × 2 heuristic)。

heap growth/shrink: 64 GiB virtual 予約のため成長/縮小なし。

#### 2.5.5 finalizer 実装

`aro_gc_finalize_check`:
- HDR_FORWARDED → fwd_overlay
- minor: in_young_from 内 (= 未 forward の dead young) なら NULL、 それ以外は alive
- major: alive 候補は forward された obj のみ → 未 forwarded ならば NULL

walk は swap 直前 (= from-space data readable) に呼ばれる。

### 2.6 mark_compact

#### 2.6.1 概要

Lisp-2 sliding compactor: 単一 region に bump alloc、 collect で
① mark → ② forward-addr 計算 → ③ 全 pointer 更新 → ④ run-based memmove で
一括 slide。 generational なし。

#### 2.6.2 パラメータ

- `REGION_BYTES = 64 GiB` 仮想 (`MAP_NORESERVE`)
- `LARGE_THRESHOLD = 4 KiB` (= 別途 malloc-backed list)
- `GC_THRESHOLD_MIN = 16 MiB`、 `GC_THRESHOLD_FACTOR = 2`

#### 2.6.3 データ構造

**Heap layout**:
- 1 つの region (= `region_base` / `region_end`)
- LargeObj malloc-backed list (= 非 moving)

**Object layout**:
- 16 B header (`ARO_GC_HAS_FWD` 定義、 専用 `gc_fwd` field 持ち)
- mark bit は `gc_flags` の bit 0 (`HDR_MARKED_BIT`)

**Allocator state**:
- `region_top` — region 内の bump pointer (= slide で前進・後退の両方あり)

**GC 中の auxiliary 構造**:
- `gray_buf[]` (= 256 → 2×)
- slide 用 scratch (= forward-addr 計算は `gc_fwd` field、 専用 buf なし)

**共通**: finalize_list

#### 2.6.4 アルゴリズム詳細

alloc: bump、 大は LargeObj。 閾値で `gc_collect_internal`。

collect (5 phase):
1. **mark**: roots → process_gray (= 標準 BFS mark)
2. **forward-addr 計算**: region 線形 walk、 各 marked obj に新 addr (= packed dst) を `gc_fwd` 経由保存。 large obj は marked なら `gc_fwd = self` (= 非 moving、 後段で identity 解決)
3. **pointer update**: 各 live obj の edges を `fwd_edge` で書換 (= `gc_fwd` 経由)。 roots も同様
4. `aro_gc_finalize_walk` (= gc_fwd 含めて整合状態で実行)
5. **slide**: 連続 marked 列 (= "run") を 1 回の `memmove` で動かす最適化。 各 run の `dst = run_first.gc_fwd`、 `src = run_start`、 size = 累積 ALIGN8。 移動後 MARKED + gc_fwd を clear
6. **large sweep**: marked → clear bits / unmarked → `free`

計算量:
- collect: O(heap × 4 phase) = O(heap)。 mark / fwd-addr / update-ptr / slide
  それぞれ 1 sweep
- WB: 0 (= 非 generational)

**GC 発火閾値**: 2.1 と同じ live × 2 heuristic。

heap growth/shrink: virt 予約、 grow/shrink なし。 region_top は slide
で前進・後退の両方あり (= compaction で縮む)。

#### 2.6.5 finalizer 実装

`aro_gc_finalize_check`: 「mark/fwd-addr 後、 slide 前」の窓で呼ばれる。
HDR_MARKED + gc_fwd != NULL なら live → `gc_fwd` 返却 (= post-slide addr)。
non-marked → NULL。 large obj は `gc_fwd = self` を返す (= 非 moving)。

### 2.7 mark_compact_gen

#### 2.7.1 概要

nursery は Cheney (= active + alt の 2 つの semispace)、 tenured は単一 region で Lisp-2 slide。
minor は N-survive Cheney、 major は young force_promote → tenured を mark +
slide compact。 nursery + tenured で計 3 領域 (= 4-面 ではない、 tenured 側に
alt 半なし)。

#### 2.7.2 パラメータ

- `YOUNG_BYTES = 16 MiB` × 2
- `TENURED_BYTES = 64 GiB` 単一
- `PROMOTE_AGE = 3`
- `MAJOR_THRESHOLD_MIN = 16 MiB`、 `MAJOR_THRESHOLD_FACTOR = 2`
- `MAX_REMSET_ENTRIES = 1 << 17`

#### 2.7.3 データ構造

**Heap layout**:
- 2.5 の young 構造 (= active + alt の 2 つの semispace × `YOUNG_BYTES`)
- 単一 tenured region (= 2.6 と同等、 `tenured_base` / `tenured_end`)

**Object layout**:
- 16 B header (= `gc_fwd` あり)
- `gc_flags` layout: MARKED=0x1, OLD=0x2, DIRTY=0x4, AGE=bits 3-4

**Allocator state**:
- `young_top` — active young semispace の bump pointer
- `tenured_top` — tenured region の bump pointer (= major slide で前進・後退)

**GC 中の auxiliary 構造**:
- `remset_buf` (= 2.5 と同形式)
- `gray_buf`
- Cheney scratch (= 2.5 と同種)
- `force_promote` flag (= major の fold-young 中に true)

**共通**: finalize_list

#### 2.7.4 アルゴリズム詳細

minor: 2.5 と同じ流れ (= nursery Cheney + remset 整理 + promote-time WB)。
ただし promote 先は単一 tenured (= `tenured_top` を bump)。 minor で
`tenured_top` を伸ばすだけで slide は走らない。

major phases:
1. `major_fold_young`: `force_promote=true` で minor 相当を呼出。 全 young を
   tenured に Cheney copy + 既存 tenured DIRTY を全 scan + 新規 promoted
   tenured を Cheney drain。 完了時 young は空
2. **mark**: roots → process_gray_major で全 tenured BFS mark
3. **forward-addr**: 各 marked tenured obj に packed dst を gc_fwd 保存
4. **update interior + roots**: `fwd_edge_compact` で書換
5. `aro_gc_finalize_walk`
6. **slide**: run-based memmove、 MARKED + gc_fwd + DIRTY clear
7. `tenured_top = fwd`、 閾値更新

計算量:
- minor: O(young live + remset + promoted)
- major: O(young live + tenured × 4 phase)
- WB hot path: bit-in-head `(MARKED|OLD|DIRTY)` のうち OLD=0x2, DIRTY=0x4 mask
  (gc_types.h で定義)

**GC 発火閾値**: 2.2 と同じ世代別 pattern (= young 固定 16 MiB、 tenured
側は live × 2 heuristic)。

heap growth/shrink: tenured は major で slide により縮む (= top が後退)。
基底 region は 64 GiB virt 予約、 madvise なし。

#### 2.7.5 finalizer 実装

`aro_gc_finalize_check`:
- minor: gc_fwd != NULL → fwd 返却 (= forwarded young)、 young_from 内なら NULL、 それ以外 alive
- major: MARKED → gc_fwd 返却 (= slide 後 addr)、 そうでなければ NULL

### 2.8 immix

#### 2.8.1 概要

Immix line/block mark-region (Blackburn-McKinley 2008)。 mark は line
単位、 alloc は line 単位の hole 内で bump。 v1 は **非 moving** (=
evacuation 未実装、 将来 v2 で defragmentation 対策の opportunistic
evacuation を入れる予定)。

**設計動機**: Cheney は live × 1 の copy cost が常に発生、 mark&sweep は
freelist で fragmentation が積む。 Immix は line 粒度の mark → hole alloc
で 「marked obj は動かさない / unmarked line だけ reuse」 を行い、 copy
cost ゼロを保ちつつ fragmentation を line 粒度に抑える。

**block / line / hole**: block (= 32 KiB) = arena 内 alloc 単位、 line (=
128 B) = block 内の mark 粒度。 hole = block 内の連続 unmarked line 列。
alloc は cur_ptr が hole 内なら bump、 hole 末端なら `find_hole` で次 hole
を探す。

**conservative line mark**: obj が line 境界をまたぐとき 「始 line + 末 line +
間の全 line」 を mark する。 一部しか占有しない trailing line も full mark
となり未使用 bytes は次回まで再利用されない (= Immix の既知 trade-off、
単純実装のため許容)。

**mark epoch**: sweep 中に bitmap zero clear せず、 `cur_epoch` counter を
1..255 で循環 (= 0 reserved = "never marked")。 mark 時 `h->gc_flags ==
cur_epoch` を「既 mark」 判定に使う。 cycle 末で `cur_epoch++` だけで過去
mark を全 invalidate。 ただし `gc_flags` 16 bit のうち low 8 bit を epoch
で占有するため OLD / DIRTY / AGE 等他の bit と混在不可 — 非 generational
の immix では他 bit を使わないので問題なし。

**size 閾値**: ≤ 128 B = small (= 1 line)、 ≤ 16 KiB = medium (= multi-line
hole 要)、 > 16 KiB = large (= 個別 mmap、 line 管理外)。 `MEDIUM_MAX =
BLOCK_BYTES / 2 = 16 KiB`。

**v1 = 非 moving**: line 内で marked obj は addr を保持。 marked line と
marked line の間に hole が残っても OK で alloc は次の hole を使う。
fragmentation が重い workload では memory 効率が下がるが v1 はこれ以上
対処しない (= v2 で opportunistic evacuation 予定)。

#### 2.8.2 パラメータ

- `LINE_BYTES = 128`
- `BLOCK_BYTES = 32 KiB`
- `LINES_PER_BLOCK = 256`
- `ARENA_BYTES = 64 GiB` 仮想
- `MEDIUM_MAX = 16 KiB` (= block の半分超は large)
- `GC_THRESHOLD_MIN = 16 MiB`、 `GC_THRESHOLD_FACTOR = 2`

#### 2.8.3 データ構造

**Heap layout**:
- `arena_base` (= 64 GiB virt MAP_NORESERVE)
- `blocks[]` (= 別 mmap、 `BlockMeta { state, line_marks[256] }` × N_BLOCKS、 lazy paged)
- block state: `BLK_FREE` / `BLK_RECYCLABLE` / `BLK_USED`
- `max_touched_block` (= 既 touch 上限。 sweep は 0..max_touched_block 範囲のみ)
- LargeObj linked list (= mmap-backed、 > MEDIUM_MAX 用)

**Object layout**:
- 8 B header
- `gc_flags` low 8 bit を epoch 値で使用 (= mark phase との比較に使う)
- `cur_epoch` (= 1..255、 wrap で 1) は global state だが obj 側の matching 値はここ

**Allocator state**:
- `cur_ptr` / `cur_end` — 現在 hole 内の bump pointer / 末端
- `block_cursor` / `line_cursor` — alloc の探索位置

**GC 中の auxiliary 構造**:
- `gray_buf[]`

**共通**: finalize_list

#### 2.8.4 アルゴリズム詳細

alloc: `aro_gc_alloc_raw`:
1. 閾値 check → 超で `gc_collect_internal`
2. `total > MEDIUM_MAX` → `large_alloc`
3. else → `hole_alloc`: cur_ptr が hole 内なら bump、 末端なら `find_hole`
   で次 hole 検索 → 無ければ `hole_alloc_cold` (= collect)

find_hole: `block_cursor..max_touched_block` 範囲を走査、 各 block で
line_marks の連続 0 run を探索。 必要な line 数を満たす hole 発見で
返却。 全 block で hole 無しなら max_touched_block++ で新 block 利用。

collect phases:
1. `for b in 0..max_touched_block: memset(line_marks[b], 0)` (= 全 line mark reset)
2. roots → `mark_value`: `gc_flags == cur_epoch` ならば skip、 set epoch + arena 内なら `mark_lines_for` (= obj 占有 line を 1 fill) + gray push
3. `process_gray` で transitive
4. `aro_gc_finalize_walk`
5. **sweep**: 各 block を line_marks 走査 → state 再計算 (= 0 marked → FREE、 256 → USED、 中間 → RECYCLABLE)、 live_bytes 集計
6. large list を unmarked epoch で munmap
7. `cur_epoch++` (255 wrap で 1)

計算量:
- alloc: O(1) amortized、 cold path で `find_hole` (= O(block scan))
- collect: O(live edges + max_touched_block × LINES_PER_BLOCK)
- WB: 0 (= 非 generational)

**GC 発火閾値**: 2.1 と同じ live × 2 heuristic。

heap growth/shrink: arena は 64 GiB virt 予約。 物理は block touch で増、
sweep で BLK_FREE になっても `madvise(DONTNEED)` は未呼出 (= 物理 release
されない)。 large obj だけ free 時 munmap。

#### 2.8.5 finalizer 実装

`aro_gc_finalize_check`: `gc_flags == cur_epoch` ならば payload (= alive、 非 moving)、
それ以外 NULL。 `cur_epoch++` を walk **の後**に行う必要あり (= walk 中は
完了 epoch を読む)。 実装通り。

### 2.9 immix_gen

#### 2.9.1 概要

世代別 Immix。 nursery は Cheney (= active + alt の 2 つの semispace × 16 MiB)、 tenured は Immix 単独
arena。 minor で N-survive promote 先は `hole_alloc_header` (= Immix の bump)。
major は force_promote_all + Immix line-mark sweep。 各 obj の young / tenured
判定は addr range で O(1) (= `in_arena` / `in_young_active` 等の inline
helper)。 Immix 自体の背景は §2.8 概要を参照。

#### 2.9.2 パラメータ

- nursery: `NURSERY_BYTES = 16 MiB` × 2
- tenured Immix: `LINE_BYTES = 128`, `BLOCK_BYTES = 32 KiB`,
  `ARENA_BYTES = 64 GiB` virt, `MEDIUM_MAX = 16 KiB`
- `PROMOTE_AGE = 3`
- `MAJOR_THRESHOLD_MIN = 16 MiB`、 `MAJOR_THRESHOLD_FACTOR = 2`
- `MAX_REMSET_ENTRIES = 1 << 17`、 `REMSET_PRESSURE_THRESH = MAX - 1`

#### 2.9.3 データ構造

**Heap layout**:
- nursery (= active + alt の 2 つの semispace × 16 MiB、 Cheney):
  - `young_active_base` — active semispace の base (= 今 alloc 中)
  - `young_alt_base` — alt semispace の base (= 次 minor の to-space)
- tenured: 2.8 と同じ Immix (= arena + blocks + max_touched_block)
- LargeObj mmap-backed list

**Object layout**:
- 8 B header
- `gc_flags` layout: EPOCH=bits 0-7, OLD=0x100, DIRTY=0x200, FORWARDED=0x400, AGE=bits 11-12

**Allocator state**:
- `young_top` — active nursery semispace の bump pointer
- `cur_ptr` / `cur_end` — tenured Immix の hole 内 bump pointer / 末端

**GC 中の auxiliary 構造**:
- Cheney scratch: `young_from_base/end`、 `young_to_base/top/end`
- `remset_buf[]` (= AroObjectHeader *、 同じ overflow 戦略だが pressure-based)、
  `remset_pressure` flag
- `gray_buf[]`、 `in_minor` / `force_promote` / `scan_saw_young` flags

**共通**: finalize_list

#### 2.9.4 アルゴリズム詳細

alloc: `nursery_bump` (= young_top bump、 閾値超で `nursery_collect_cold` 経由 minor or major)。 large は直接 `large_alloc`。

minor phases:
1. setup: young_from = active、 young_to = alt
2. roots → `fwd_edge_minor` → `forward_payload_nursery`:
   - age < PROMOTE_AGE → young_to bump-copy + age++
   - age >= PROMOTE_AGE (or force_promote) → `hole_alloc_header` で tenured Immix へ copy + H_OLD set
   - gray_push 新 obj (= 通常 Cheney と異なり明示 gray queue)
3. **remset 整理**: `H_DIRTY` 残存 entry を scan + scan_saw_young 判定 → keep/drop
4. **gray drain**: 各 gray obj を scan。 obj が `in_arena` (= promoted) かつ
   `scan_saw_young` で `H_DIRTY` 未設定なら set + `remset_push`
5. `aro_gc_finalize_walk`
6. young swap (= alt が次回 active)

major phases:
1. young 非空なら `force_promote=true` で `minor_gc` を呼んで fold (= 全 young promote)
2. `remset_cnt = 0`、 全 block line_marks を 0 fill
3. roots → `mark_edge_major` → `mark_value_major`: `HDR_EPOCH == cur_epoch` skip、 set + line_marks fill + gray push
4. process_gray_major で transitive
5. finalize_walk
6. sweep_major: 各 block の line_marks 集計で state、 large list 整理
7. `cur_epoch++`、 閾値更新

計算量:
- alloc: O(1)
- minor: O(young live + remset × edges + promoted × edges)
- major: O(young live (= force fold) + live edges + max_touched_block × LINES_PER_BLOCK)
- WB hot path: bit-in-head OLD=0x100, DIRTY=0x200

**GC 発火閾値**: 2.2 と同じ世代別 pattern (= young 固定 16 MiB、 tenured
側は live × 2 heuristic)。

heap growth/shrink: nursery + arena ともに virt 予約、 grow なし shrink なし。

#### 2.9.5 finalizer 実装

`aro_gc_finalize_check`:
- HDR_FORWARDED → fwd_overlay (= promoted nursery、 大 obj は移動しない)
- in_minor: young_from 内なら NULL、 それ以外 (= tenured) alive
- major: HDR_EPOCH == cur_epoch → alive、 そうでなければ NULL

### 2.10 mark_bitmap_gen

#### 2.10.1 概要

`mark_gen` と意味同等 (= sticky mark + 2-gen + remset + N-survive)、 metadata を
per-page bitmap 化:
- 8 B header (= mark/old/dirty bit を per-page bitmap に移動、 AGE/FREE のみ head)
- page は 16 KiB **aligned** mmap (= over-mmap + trim、 `page_of(obj) = obj & ~16383`)
- N-survive 化完了 (= 旧 doc の "debug 中" は古い)

#### 2.10.2 パラメータ

- `NUM_SIZE_CLASSES = 9`、 `PAGE_SIZE = 16 KiB`
- `MAX_SLOTS_PER_PAGE = 512`、 `BITMAP_BYTES = 64`
- 3 bitmap (= mark/old/dirty) × 64 B = 192 B / page
- `MAJOR_THRESHOLD_MIN = 16 MiB`、 `MINOR_THRESHOLD = 16 MiB`
- `PROMOTE_AGE = 3`、 `MAX_REMSET_ENTRIES = 1 << 17`

#### 2.10.3 データ構造

**Heap layout**:
- `page_head[9]` (= 16 KiB-aligned mmap 直で取得 / over-mmap then trim)
- `Page` (208 B): `{ next, class_idx, n_slots, _pad, mark_bm[64], old_bm[64], dirty_bm[64] }` + slots
  (= 3 bitmap (mark/old/dirty) は page header に内蔵、 per-slot metadata の物理位置)
- LargeObj: side-struct で `mark/old/dirty` を bool 持ち

**Object layout**:
- 8 B `AroObjectHeader`: head の `gc_flags` 内に FREE bit + AGE bits 1-2 のみ
- mark/old/dirty bit は page bitmap 側 (= obj 本体に bit を持たない)

**Allocator state**:
- `freelist[9]`
- `size_class_shift[9]` (= clz 最適化、 3072 だけ shift=0 で div fallback)

**GC 中の auxiliary 構造**:
- `remset_buf[]` (= AroObjectHeader *)、 `remset_overflow`
- `promoted_buf[]`
- `gray_buf[]`

**共通**: finalize_list

#### 2.10.4 アルゴリズム詳細

alloc: 2.1 と同様 slab + bitmap (= alloc 時 `bm_clr(mark_bm, idx)` 等は
不要、 freelist 初期値で 0)。

minor phases:
1. roots → `mark_edge` → `mark_value` (= `get_old(gc, h)` で skip、 young は
   `set_mark` + gray push + `scan_saw_young = true`)
2. `process_gray` で transitive (= 各 obj の `scan_outgoing`)
3. **remset 整理** (overflow 時: 全 page 走査 + LargeObj 走査、 通常時:
   remset_buf 線形走査): scan + scan_saw_young で keep/drop、 false なら
   `bm_clr(pg->dirty_bm, idx)` / `lo->dirty = false`
4. process_gray 再 drain (= remset 整理中に新規 gray が積まれる可能性)
5. `aro_gc_finalize_walk`
6. `sweep` (minor=true): 全 page 走査。 marked + age<PROMOTE_AGE → mark clear + age++、
   marked + age>=PROMOTE_AGE → set old_bm + clear mark/dirty + `promoted_push`、
   unmarked young → free
7. **promote-time WB**: 各 promoted を `check_edge_for_young` で scan、 立てば
   `bm_set(dirty_bm)` + `remset_push`

major phases:
1. `remset_cnt = 0`、 **全 page の dirty_bm を 0 fill + LargeObj.dirty = false**
   (= N-survive WB invariant: dirty ↔ remset 在席、 cnt 0 にしたら bit も全消し)
2. roots → mark → process_gray
3. finalize_walk
4. `sweep(minor=false)`: marked → mark clear + (若 young) → set old + age=0、
   unmarked → 全 bit clear + free

計算量:
- alloc: O(1) amortized (= slab freelist)
- minor: O(young live + remset_entries × edges + promoted × edges)
- major: O(全 page 走査 = O(heap_slots))
- WB: full out-of-line (= `aro_gc_wb` extern、 bitmap lookup を 1 関数で
  実行。 `ARO_GC_WB_OLD_MASK` 未定義のため inline fast path 無し)

**GC 発火閾値**: 2.2 と同じ世代別 pattern (= young 固定 16 MiB、 tenured
側は live × 2 heuristic)。

heap growth/shrink: page-on-demand。 sweep で全空 page も munmap せず保持
(= mark / mark_gen と同様)。

#### 2.10.5 finalizer 実装

`aro_gc_finalize_check`:
- minor: `get_old` → alive、 `get_mark` → alive、 どちらでもなければ NULL
- major: `get_mark` → alive、 NULL otherwise

非 moving、 payload addr 不変。

### 2.11 mark_card_gen

#### 2.11.1 概要

`mark_bitmap_gen` + per-page `card_dirty` flag + page-level remset。 slot 単位
の dirty_bm に加え page 全体が「remset に居る」 flag を持つ。

- `dirty_bm[slot_idx]` = どの slot が DIRTY か (= bitmap_gen と同じ)
- `card_dirty` = この page が remset_buf に居るか (= page-level 高速 check)
- `remset_buf[]` = **Page *** の array (= bitmap_gen は AroObjectHeader * だった)

remset は **page 単位** に粗化されている。 card_dirty と dirty_bm の役割
分担 / 「両持ち」 ではなく「page-level remset が main」 である点の設計
議論は §6 Q3 参照。

#### 2.11.2 パラメータ

- 2.10 と同じ + `card_dirty` per page (= 1 byte)

#### 2.11.3 データ構造

**Heap layout**:
- `page_head[9]` (= 2.10 と同じ 16 KiB-aligned)
- `Page` (208 B): bitmap_gen に `card_dirty` byte を追加した layout (= per-slot `dirty_bm` + per-page `card_dirty` の両方を内蔵)
- LargeObj: side-struct + `card_dirty` bool

**Object layout**: 2.10 と同じ (= 8 B header + FREE/AGE のみ head 内)

**Allocator state**: 2.10 と同じ (= `freelist[9]`)

**GC 中の auxiliary 構造**:
- `remset_buf[]` (= `Page **` 型、 size_t cnt / capa)
- 上限なし (= heap page 数で自然境界)
- `gray_buf[]`、 `promoted_buf[]`

**共通**: finalize_list

#### 2.11.4 アルゴリズム詳細

alloc: 2.10 と同じ slab。

minor phases:
1. roots → mark → process_gray
2. **page-level remset 整理**: 各 remset entry (= page) を slot 走査 →
   slot.dirty_bm[i] が 1 の slot を scan、 scan_saw_young なら slot bit keep、
   false なら clear。 page 内 全 slot が clean になれば `card_dirty = 0` + page
   を remset から drop。 LargeObj も同様
3. finalize_walk
4. sweep (minor=true、 promote 含む)
5. promote-time WB: 各 promoted の check_edge_for_young、 立てば
   `bm_set(dirty_bm)` + (page.card_dirty 立ってなければ `remset_push_page`)

major phases:
1. `remset_cnt = 0`、 全 page で `card_dirty = 0` + `dirty_bm` 0 fill
2. mark → process_gray → finalize_walk → sweep(minor=false)

計算量:
- WB hot path: `(get_old, !card_dirty) ⇒ set dirty_bm + card + push_page`、
  「同 page への 2回目以降の WB は card_dirty=1 で早期 return」
- minor remset 整理: O(page数 × n_slots) (= page 単位の粗化、 bitmap_gen より cache friendly)
- 他は 2.10 と同じ

heap growth/shrink: 2.10 と同じ。

#### 2.11.5 finalizer 実装

2.10 と同じ。

## 3. 特殊用途 backend

ここで挙げる 5 つは GC algorithm として実用される backend ではなく、 特定
の目的に特化した参照実装。 `none` と `bump` は GC overhead を排除した上限
性能の baseline (= §2 各 backend の overhead 計測の対照値)、 `mark_bump_gen`
は bump promote した tenured を回収しない場合の挙動を測定する testbed (=
major sweep が unmarked tenured を free せず monotonic 増加)、 `mark_freelist`
は freelist + 無 coalescing で fragmentation を観察する testbed (= region_top
never retreats / size-class isolation で最悪 fragmentation を顕在化)、
`copy_scramble` は stale VALUE slot を SEGV で炙り出す audit / debug
backend。 bench の参照値および runtime の verification にのみ意義があり、
production の GC 選択肢としては想定しない。

### 3.1 none

#### 3.1.1 概要

何もしない baseline。 alloc は `malloc(payload_size)` で取得し、 解放しない。
GC overhead を排除した上限性能の参照値。 stress / purge / scramble 全て無効。

#### 3.1.2 パラメータ

なし。

#### 3.1.3 データ構造

malloc + leak のため GC 用 metadata なし。

- heap: libc malloc が管理 (= 自身では何も持たない)
- remset / gray queue: 不要 (= 無し)
- **共通**: finalize_list は `gc_common.c` の共通実装 (`AroGcCommonState.finalize_list`,
  `finalize_count`, `finalize_cap`)。 初期 cap = 0、 16 → 2× で grow

#### 3.1.4 アルゴリズム詳細

- alloc: `calloc(1, payload_size)`、 head.gc_size 書込。 O(libc malloc)
- collect: `aro_gc_collect` は no-op。 計算量 0
- WB: no-op (= `ARO_GC_HAS_WB` 未定義、 plain `*slot = v`)

heap growth/shrink: libc 任せ。 framework は閾値判定すらしない。

#### 3.1.5 finalizer 実装

`aro_gc_finalize_register` は共通実装。 `aro_gc_finalize_check` は 常に
payload を返す (= 全 entry を alive 扱い)。 collect が no-op なので finalize_walk
は事実上呼ばれず、 process 終了まで finalizer は走らない。 GMP buffer 等を
sample 側で使う場合は none では leak する点に注意。

### 3.2 bump

#### 3.2.1 概要

1 つの mmap region に対し pointer を進めるだけ。 `none` よりも alloc が
速い (= `*top++`)。 解放しない。

#### 3.2.2 パラメータ

- `REGION_BYTES = ARO_GC_REGION_VIRT_BYTES = 64 GiB` 仮想

#### 3.2.3 データ構造

mmap 1 region + `region_top` のみ。

- 1 つの `mmap(NULL, 64 GiB, MAP_NORESERVE)` region: `region_base` /
  `region_top` / `region_end` を `ASTroGC` 内に持つ。 物理 page は touch 時に
  commit
- gray / remset / promoted_buf: 不要
- **共通**: finalize_list (= gc_common.c、 ただし collect が no-op のため事実上 walk されない)

#### 3.2.4 アルゴリズム詳細

- alloc: `region_top += ALIGN8(size)`、 head 書込 + payload zero-fill。 O(1)
- collect: no-op
- WB: no-op

heap growth/shrink: 64 GiB virtual 予約のため成長なし、 縮小なし。 region
を超えると abort。

#### 3.2.5 finalizer 実装

`none` と同じ。 alive-forever。

### 3.3 mark_bump_gen

#### 3.3.1 概要

nursery は Cheney (= active + alt の 2 つの semispace)、 tenured は単一
bump region (= 移動なし、 alt semispace なし)、 sweep は線形走査。 major sweep は **unmarked tenured
を free せず**、 mark を clear するのみ。 結果として tenured は monotonic に
増え、 OOM か 64 GiB virt 上限まで進む。 「bump で promote させて回収しない」
場合の挙動を観察する testbed であり、 実用 GC ではない (= だから §3 に
所属する)。 「copy_gen で promote した tenured を free しない」 という見方は
誤読しやすく、 正確には 「nursery は Cheney (= copy)、 tenured は bump で
append のみ、 sweep は mark bit を見るだけで freelist は持たない」。
「mark_gen + bump tenured」 と呼ぶよりは 「copy nursery + bump tenured (=
純粋 append)」 が正確。 C コメントとの乖離は §5.1 を参照。

#### 3.3.2 パラメータ

- `YOUNG_BYTES = 16 MiB` × 2
- `TENURED_BYTES = 64 GiB` 単一
- `PROMOTE_AGE = 3`
- `MAJOR_THRESHOLD_MIN = 16 MiB`

#### 3.3.3 データ構造

**Heap layout**:
- 2.5 の young (= active + alt の 2 つの semispace × `YOUNG_BYTES`)
- 単一 tenured bump region (= `tenured_base` / `tenured_end`、 alt なし)

**Object layout**:
- 8 B header + fwd overlay
- `gc_flags` layout: MARKED=0x1, OLD=0x2, DIRTY=0x4, FREE=0x8, FORWARDED=0x10, AGE=bits 5-6

**Allocator state**:
- `young_top` — active young semispace の bump pointer
- `tenured_top` — tenured bump region の pointer (= monotonic 増加、 sweep でも戻らない)

**GC 中の auxiliary 構造**:
- `remset_buf`
- `scan_buf` (= 別 queue、 major で promoted の transitive scan 用)
- `gray_buf`
- Cheney scratch (= 2.5 と同種)
- `force_promote` flag

**共通**: finalize_list

#### 3.3.4 アルゴリズム詳細

minor: 2.5 と同じ。 promote 先は tenured bump (= `tenured_top` 前進のみ)。

major phases:
1. `force_promote = true` で全 young を tenured に Cheney copy (= forward + MARKED)
2. mark + transitive scan (`major_process` × gray_buf + scan_buf)
3. finalize_walk
4. **"sweep"**: tenured を線形走査、 marked は MARKED + DIRTY clear、 **unmarked
   は何もしない**(= bump pointer は戻らない、 freelist もない)

計算量:
- minor: O(young live + remset)
- major: O(young live + tenured slots)
- WB hot path: bit-in-head OLD=0x2, DIRTY=0x4

**GC 発火閾値**: 2.2 と同じ世代別 pattern (= young 固定 16 MiB、 tenured
側は live × 2 heuristic)。 ただし sweep が free しない設計のため tenured
live は実質「累積 promote 量」 = 単調増加、 閾値も同じく単調増加して
最終的に 64 GiB virt に到達。

heap growth/shrink: tenured は **monotonic 増加** (= 縮まない)。 OOM までは
増え続ける設計。 64 GiB virt が実 limit。

#### 3.3.5 finalizer 実装

`aro_gc_finalize_check`:
- HDR_FORWARDED → fwd_overlay
- minor: young_from 内なら NULL、 それ以外 alive
- major: MARKED ならば alive、 そうでなければ NULL

### 3.4 mark_freelist

#### 3.4.1 概要

mark&sweep、 単一 region + size-class freelist。 region 内に bump で append、
sweep で unmarked を class 別 freelist に戻す (= region pointer は戻らない)。
fragmentation 観察用 testbed。

**fragmentation を加速する構造**: 実装は意図的に fragmentation 対策を一切
持たない:
- **no coalescing**: 隣接する free slot を merge して大きな free 領域に
  する処理が無い (= size-32 × 2 が並んでも size-64 として使えない)
- **size-class isolation**: `freelist[9]` は class 単位 LIFO chain。
  size-N freelist の slot は size-N alloc にしか使えず、 他 class へは
  流用不可
- **region_top never retreats**: sweep で `region_top` は戻らない。
  物理 footprint = 歴代 peak alloc 量 で、 live が減っても物理 page は
  解放されない
- 結果: alloc pattern が時系列で変化すると、 過去の class の freelist が
  滞留しつつ region_top が前進し続け、 最終的に 64 GiB virt 上限まで到達。
  bench v11 でも多くの workload で FAIL するのはこの構造的特性が原因

実用的な mark&sweep (= CRuby など) は class 単位 page / coalescing / 物理
release などで fragmentation を抑える。 本 backend はそれら全てを外して
「最悪ケース」 を観察するためのもの。 mark_bump_gen (= §3.3) と並ぶ
testbed 系列で、 mark_bump_gen が 「promote tenured を回収しない場合」 を、
mark_freelist が 「freelist に coalescing も class 流用も無い場合」 を
それぞれ観察対象とする。

**位置付け** (compact / non-compact との関係): compact (= mark_compact、
§2.6) と非 compact (= mark、 §2.1) の中間。 mark_compact のように slide は
せず、 mark のように page 単位の reuse はしない。 sweep は region 全体
線形走査 → unmarked を **同じ位置に** free marker 化して freelist に戻す
(= "in-place freelisting")。 generational なし (= 「fragmentation testbed」
として shape は最小限。 generational 化は別 backend `mark_gen` 等で網羅済)。

#### 3.4.2 パラメータ

- `NUM_SIZE_CLASSES = 9`、 `size_class_bytes = {32..4096}`
- `MAX_SLOT_BYTES = 4096`
- `REGION_BYTES = 64 GiB` 仮想
- `GC_THRESHOLD_MIN = 16 MiB`、 `GC_THRESHOLD_FACTOR = 2`

#### 3.4.3 データ構造

**Heap layout**:
- 単一 region (= `region_base` / `region_end`、 `MAP_NORESERVE` 64 GiB)
- LargeObj mmap-backed list (= > 4 KiB)

**Object layout**:
- 8 B header
- `gc_flags` bit: MARKED=0x1, FREE=0x2
- free slot は `gc_size` を class slot size に書換 (= sweep 線形 walker が次
  slot へ進むため)。 link は payload offset 8 の overlay

**Allocator state**:
- `freelist[9]` (= class 別 LIFO free chain)
- `region_top` — region 内の bump pointer (= freelist 空時の fallback、 sweep でも戻らない)

**GC 中の auxiliary 構造**:
- `gray_buf[]`

**共通**: finalize_list

#### 3.4.4 アルゴリズム詳細

alloc: `alloc_slot`:
1. slot_total > MAX_SLOT_BYTES → `alloc_large`
2. class freelist pop or `region_top` から bump (= 新 slot)
3. head 書込 + AROH_INIT_PAYLOAD (= 値部 zero fill、 free slot に残った
   stale pointer-like data を消す)

collect phases:
1. mark roots + process_gray
2. finalize_walk
3. **sweep_region** (= 線形 walk):
   - HDR_IS_FREE: 既存 free を新 freelist に re-thread (= cycle 跨ぎで chain reset)
   - MARKED: clear、 live_bytes 集計
   - 未 marked: `HDR_SET_FREE`、 gc_size = sb (= class size に正規化)、 freelist push

計算量:
- alloc: O(1) amortized
- collect: O(region slots + live edges)
- WB: 0

**GC 発火閾値**: 2.1 と同じ live × 2 heuristic。

heap growth/shrink: region は `region_top` まで増加、 縮まない (= bump pointer は
sweep でも戻らない)。 64 GiB virt が限界。 large obj は free 時 munmap。

#### 3.4.5 finalizer 実装

`aro_gc_finalize_check`: HDR_MARKED → alive、 NULL otherwise (= 非 moving)。

### 3.5 copy_scramble

#### 3.5.1 概要

`copy` + per-cycle XOR mask `R` で VALUE storage を撹乱する audit backend。
heap pointer slot は `raw ^ R` で保存、 各 GC 後に R を rotate。 stale slot
(= sample が `ARO_LOAD` decode を忘れた slot、 GC が scan 漏らした edge) は
次の deref で SEGV 確実。

#### 3.5.2 パラメータ

- 2.4 (copy) の全パラメータ + `ARO_GC_HAS_SCRAMBLE = 1`

#### 3.5.3 データ構造

**Heap layout**: 2.4 と同じ (= 2 semispace + LargeObj list)

**Object layout**: 2.4 と同じ (= 8 B header + fwd overlay、 `HDR_FORWARDED` / `HDR_MARKED`)

**Allocator state**: 2.4 と同じ (= `active_base` / `active_top` / `active_end`)

**GC 中の auxiliary 構造**:
- 2.4 の Cheney scratch + large_gray
- `scramble_R` / `scramble_R_old` を `AroGcCommonState` 上で持つ
  (低 3 bit = 0 で 8-align 保持、 fixnum tag 保護)

**共通**: finalize_list

#### 3.5.4 アルゴリズム詳細

- alloc / collect: 2.4 と同じ
- ただし `aro_gc_alloc` の戻り値 VALUE は `raw_payload ^ scramble_R` で encode
- `ARO_LOAD` (= sample 側 macro) と `ARO_GC_VISIT_EDGE` (= gc.h) で必ず XOR decode/encode
- 各 collect の入口で `scramble_R_old = scramble_R`、 出口で
  `scramble_R = scramble_pick_R()` (= /dev/urandom + low 3 bit clear)
- 推奨: `BARUBY_GC_STRESS=1` と組合せて全 alloc で R rotate (= audit 強化)

非 scramble backend では `scramble_R = scramble_R_old = 0` 永続のため
XOR が identity に fold (= 性能 cost ゼロ)。

heap growth/shrink: 2.4 と同じ。

#### 3.5.5 finalizer 実装

2.4 と同じ (HDR_FORWARDED → fwd_overlay、 HDR_MARKED → self、 else NULL)。
scramble は finalize の戻り値には影響しない (= 戻り値は raw addr、 sample 側で
再 encode は不要)。

## 4. 共通 framework API

### 4.1 sample 側必須

- `AROH_VISIT_ROOTS(c, ctx, edge_visit)` — root scan macro。 sample 固有の root
  (= eval stack, globals 等) を `edge_visit` で walk
- `AROH_SCAN_EDGES(payload, payload_size, ctx, edge_visit)` — obj の outgoing
  edges を walk。 sample が type tag で dispatch
- `AROH_FINALIZE(payload)` — finalize hook (= mpz_clear 等)、 sample 不要なら no-op
- `AROH_IS_GC_OBJECT(v)` — VALUE が GC managed heap pointer かの predicate

### 4.2 framework 提供

- `aro_gc_alloc(c, size)` — scan-safe alloc (= zero-init)
- `aro_gc_alloc_byte(c, size)` — raw byte alloc (= zero-init なし、 sample が即 fill)
- `aro_gc_realloc_payload(c, p, new)` — sample 側で実装 (= sp slot park パターン)
- `aro_gc_wb(c, holder, slot, val)` — write barrier
- `aro_gc_wb_bulk(c, holder, dst, src, n)` — bulk WB
- `aro_gc_collect(c)` — 強制 collect (= 通常 major)
- `aro_gc_account_external(c, delta)` — GMP buffer 等 external 量を framework に
  通知 (= 閾値超で GC 発火)
- `aro_gc_finalize_register/check/walk/fini`

### 4.3 backend hook

backend は `gc_*.c` で以下を実装:
- `aro_gc_init(c)`, `aro_gc_fini(c)`
- `aro_gc_alloc_raw(c, size)` / `aro_gc_alloc_byte_raw(c, size)` (= encode は framework が wrap)
- `aro_gc_collect(c)` (= 強制 major)
- `aro_gc_size_of(payload)`
- `aro_gc_finalize_check(c, payload)`
- WB: `ARO_GC_HAS_WB` 定義時に `aro_gc_wb` (out-of-line) or `aro_gc_remember`
  (inline fast path + cold extern)。 `ARO_GC_WB_OLD_MASK` 定義時は gc.h の
  inline fast-path が使われる (= mark_gen / copy_gen / mark_compact_gen /
  mark_bump_gen / immix_gen 等)

詳細 layering は `runtime/precise_gc/gc.h` と `gc_types.h` の冒頭 comment を参照。

## 5. 実装上の問題点メモ

doc を書く過程で見つけた実装と doc / コメントの乖離。 後で修正する material。

### 5.1 mark_bump_gen の C コメントが実装と乖離

`mark_bump_gen` は §3.3 (特殊用途) で「sweep が free しない testbed」 として
位置付け済 (= 設計意図通り)。 ただし `gc_mark_bump_gen.c:586` の C コメントは
「Linear sweep tenured: free unmarked, clear bits on survivors」 と書いており、
実装 (MARKED clear + live bytes 集計のみ) と乖離している。 testbed の意図を
誤読させない為に C コメントを修正するのが material (= "free unmarked" の
記述を削る or "no-op for unmarked (testbed)" に書換)。

### 5.2 immix の gc_flags 使用 bit 数

`gc_immix.c` の `mark_value` は `h->gc_flags == cur_epoch` で「全 16 bit が
epoch 値そのもの」を仮定して比較する (= `HDR_EPOCH` macro と異なる)。
非世代の immix では他の bit を使わないので動くが、 macro と一貫性がない。
`immix_gen` は `HDR_EPOCH(h) == cur_epoch` (= low 8 bit のみ比較) と書いて
おり、 こちらが正解形式。 immix を `HDR_EPOCH(h) == cur_epoch` に統一すべき。

### 5.3 mark_gen_inc の "incremental" は事実上 STW

`INC_WORK_PER_ALLOC = SIZE_MAX` で 1 step が全 gray を drain する。 構造は
incremental 用 (start/step/finish)、 pause 測定の segment 分割は機能するが、
mutator-side WB が VALUE-stack まで届いてないため真の incremental には未到達。
todo.md にあるはず。

## 6. FAQ / 設計上の論点

各 backend の設計判断や 「なぜそうなっているか」 を後から振り返るための備忘。
本論 (§2 / §3) では実装の動作のみを淡々と記述し、 設計判断の議論は本
section に集約する。

### Q1. mark_gen の `young_objs[]` 配列は必須か? (§2.2)

**質問**: minor の sweep を 「全 page walk」 で代替すれば配列管理は不要に
なる。 にも関わらず別配列で young を追跡している理由は?

**回答**: page を全 walk すれば young は識別可能だが、 `young_objs[]` を
持つことで `sweep_young_minor` が O(young live) に下がる (= 全 heap walk
の `sweep_young_major` と対比)。 cost は alloc 時の push 1 entry + minor
終了の in-place compact (= 線形)。 配列を持たないなら sweep_young 相当の
ため全 page を slot 走査 (= O(heap)) する必要があり、 minor の
eden-collection 想定 (= young live は小さい) から大きく外れる。 別配列の
overhead が「全 heap walk を minor 毎に回避できる」 効果に対して十分に
小さい、 というのが採用理由。

### Q2. copy_gen の SET_DIRTY と major の効果 (§2.5)

**質問**: SET_DIRTY の目的は? major collection は何をする? N-survive と
major の関係は?

**回答**:
1. **DIRTY bit の意義**: 「この tenured obj は remset_buf に居る」 マーク。
   用途は (a) WB の fast-path で `(OLD && !DIRTY)` を 1 命令で判定 → 既
   DIRTY ならば無視 (= 同 obj への複数 write で remset 重複 push しない)、
   (b) minor 終了時に remset 整理で 「scan_saw_young false なら DIRTY を
   clear」 して entry drop → 次 minor で再 push 可能になる。 DIRTY と
   remset_buf 在席は invariant (= 片方だけ立つ状態が major 直後の cleanup
   window 以外には起きない)。
2. **minor / major の選択** (`nursery_collect_cold` の判定):
   - `tenured_top + young_used > tenured_end` (= worst-case promote が
     tenured を溢れさす)
   - `old_alloc_since_major > old_major_threshold` (= 累積 tenured alloc が
     閾値超)
   - `external_bytes > old_major_threshold` (= GMP buffer 等が major 必要)
   これらいずれかなら major、 そうでなければ minor。 「minor → 直後 major」
   の chain を起こさない方針。
3. **major で N-survive 効果が消えるか?**: major では実装上 「young も
   from-tenured も to-tenured へ forward」 (= 結果として全 live が tenured)。
   age check は `forward_obj` 内の `in_minor` ブランチでしか行われず、
   major では age に関わらず tenured 化。 major 直後は確かに 「全 live が
   tenured」 状態。 だが major 後の新規 alloc は young に積まれるので、
   次回以降は age 0 から再カウント。 「永久に tenured」 になるわけではない。
   major の頻度を `MAJOR_THRESHOLD_FACTOR = 2` で抑えているため、 ほとんど
   の cycle は minor で N-survive 動作する。

### Q3. mark_card_gen の card と remset の関係 (§2.11)

**質問**: card_dirty と remset_buf 両方を持つのはなぜ? どちらが primary か?

**回答**: 「両方持つ」 というより 「page-level remset が main、 slot-level
dirty_bm は scan 効率化」 の構成。 用途は以下:
1. WB hot path: `if (get_old && !card_dirty) { set both + remset_push_page }`
   (= 一旦 page を remset に入れたら以後 WB は page-level check のみで早期
   return)。
2. minor の remset 整理: 各 page を slot 単位で walk、 `dirty_bm` slot を
   scan、 page 内に 1 つも young child が残らなければ page 自体を remset
   から drop。
3. fallback として全 page heap-walk は不要 (= remset_buf cap が page 数なので
   overflow しない、 `MAX_REMSET_ENTRIES` 制限なし)。

bitmap_gen の slot-level remset と排他で、 fallback ではなく主目的が異なる
(= 大量 WB workload で remset が爆発するのを page 粗化で抑える testbed)。

