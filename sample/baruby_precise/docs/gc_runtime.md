# baruby_precise GC ランタイム入門

このドキュメントは **GC を知らない人でも読めること** を目標に、
baruby_precise が用意している 14 種類の garbage collector がそれぞれ
**どうヒープを管理し、 どんなアルゴリズムで動いているか** を説明する。

技術的により深い実装メモは [runtime.md §5](runtime.md) を参照。 ベンチ
結果と勝者分布は [perf.md §2](perf.md) を参照。

## 0. 前提: そもそも GC って何

プログラムが「new とか alloc して使い終えたメモリ」 を、 自動で識別して
回収する仕組みが **garbage collector (GC)** 。 主に 2 つの仕事:

1. **alloc** — 新しいオブジェクト用の領域を確保する。 速くて、 cache
   locality が良いほどよい。
2. **collect** — もう使われていないオブジェクト (= どこからも参照されて
   いないもの) を識別して領域を再利用可能にする。 これが GC algorithm の
   本体。

baruby_precise の 14 backend は、 **「alloc 戦略」 × 「collect 戦略」**
の組合せ違いを比較できる testbed。

### 用語ミニ辞典

| 用語 | 意味 |
|---|---|
| **root** | mark を始める起点 (stack / register / global など、 GC からは「外から見えてる」 もの)。 baruby_precise では `c->env..c->sp` の連続 VALUE 配列。 |
| **mark** | root から到達可能なオブジェクトに「生きてる」 印を付ける phase。 |
| **sweep** | mark されていないオブジェクトを freelist に戻す / 領域を解放する phase。 |
| **moving** | sweep の代わりに「生きてるオブジェクトを別の場所に コピー」 することで領域を再利用する。 移動後の元アドレスはダングリング (forwarding pointer で対処)。 |
| **non-moving** | オブジェクトは alloc 時の場所から動かさない。 |
| **generational** | 「ほとんどのオブジェクトはすぐ死ぬ」 仮説 (weak generational hypothesis) を活かす設計。 新しい (young) と古い (old / tenured) を分けて、 minor は young のみ collect。 |
| **sticky mark-bits** | 世代分離を「物理的な領域」 ではなく「mark bit を minor 後にクリアしない」 ことで実現する gen の流派。 mark=1 が即「old」 を意味する。 |
| **write barrier (WB)** | gen / inc GC で必須。 old から young への参照書込を mutator 側で記録する hook。 |
| **remset (remembered set)** | WB が書き出した old→young 参照の保存先。 minor 時に remset エントリだけを scan すれば全 old を scan しなくて済む。 |
| **incremental marking** | mark phase を mutator と細切れに交互実行することで pause を分散させる。 SATB barrier が必要。 |
| **SATB (snapshot-at-the-beginning) barrier** | incremental / concurrent GC で「mark 開始時点に到達可能だったオブジェクトは全部 live と扱う」 を実現する WB の一種。 具体的には mutator が **既存の参照を上書きしようとする時** に、 **上書きされる旧 値** を retain (gray queue に push) する。 こうすることで mark 中の mutator 操作で参照グラフが変わっても、 開始時のスナップショットに居た obj は確実に mark 漏れしない。 対義は incremental update barrier (= 「新参照を retain」 する流派)。 |
| **incremental update barrier** | SATB の対義。 mutator が「新たに参照を作る時」 の **新値** を retain する。 SATB と比べると "floating garbage" は少ないが、 mark の correctness を保証するために stack scan 等の追加コストが必要。 |
| **gray queue** | mark phase 用の中間バッファ。 「marked だけどまだ outgoing 参照を辿っていない」 オブジェクトを溜める stack/queue。 to-do 用。 root から mark を辿る BFS / DFS 走査の implementation の一つ。 これが空になったら mark phase 完了。 |
| **Cheney semispace algorithm** | moving GC の古典。 ヒープを 2 つの等サイズ region (from-space / to-space) に分け、 collect 時に live を from から to に copy。 copy 後 from を破棄。 alloc は bump、 fragmentation 無し。 copy 時に「to に既に copy 済の obj」 を scan ポインタで FIFO 走査するから "scan-loop"。 root → to ヘ copy → outgoing 参照を再帰でなく queue で辿る、 という形。 |
| **forwarding pointer (fwd)** | moving GC で、 元の場所に「移動先アドレス」 を上書き保存しておくフィールド。 同じ obj への 2 回目以降の参照を辿るとき、 元アドレスを deref すると fwd が見えて新アドレスに転送される。 |
| **Lisp-2 (slide compaction)** | mark + compact 系のアルゴリズム。 mark 後、 live obj を heap の頭から詰めて並べ替える (= "slide")。 fwd address pass → outgoing pointer 更新 pass → root 更新 pass → 実際の memmove pass、 という 4 段階。 in-place で 1 region 内で完結、 fragmentation 解消。 元論文 Knuth が Lisp-2 で実装したのが名前の由来。 |
| **hole / line / block (Immix)** | Immix 用語。 **block** = 一定サイズ (32 KiB) の領域、 **line** = block 内の更に細かい単位 (128 B)。 mark phase で「この line に live obj が居る」 を bit で記録、 mark されてない連続 line の run = **hole**。 alloc は hole 内で bump、 hole が尽きたら次の hole を探す。 |
| **evacuation (Immix v2)** | fragmentation が進んだ block から live obj を別 block に copy 退避すること。 これにより元 block を完全に空けて再利用可能にする。 v1 (本実装) には未搭載 (将来候補)。 |
| **epoch counter (mark epoch)** | 「mark bit のクリアを heap walk なしに済ませる」 ための trick。 GCHeader に uint8 mark_epoch を持たせ、 mark phase は `cur_epoch` (global) を slot に書き込む。 GC 完了後 cur_epoch を tick (+1) すると、 旧サイクルの値は cur_epoch と一致しなくなる = 自動で「未 marked」 扱いに戻る。 全 mark bit を巡回クリアする O(heap) コストが消える。 |

## 1. baruby_precise が共通で持つもの

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

### 1.2 VALUE 表現 (LSB-tagged)

```
[ ...62 bit... | 00 ]   ← ヒープへのポインタ (8-aligned なので下 2 bit 必ず 0)
[ ...62 bit... | 01 ]   ← 小整数
[ ...62 bit... | 11 ]   ← 特殊値 (true / false / nil 等)
```

GC は LSB を見て pointer かどうかを判定 (`IS_PTR(v)`)。 即値は無視する。
これが **precise scanning** (= conservative ではない) の根幹。

### 1.3 GCHeader (各 backend で layout が違う)

ヒープオブジェクトは payload の **すぐ前** に GCHeader を持つ:

```
[ GCHeader ][ payload ... ]
            ^
            VALUE はここを指す (h+1)
```

#### field の意味

GCHeader が持ち得る field 一覧。 backend ごとに「どれを持つか」 が違う:

**iter 31 で全 backend が flags byte packing に統一**: `kind`/`marked`/`old`/`dirty` の各 bool / enum field は **単一 `flags` byte (uint8_t)** にビット詰めされる。 これで header サイズが大幅に縮む (e.g., 24 B → 16 B、 immix は 16 B → 8 B)。

| Field | 型 | 意味 |
|---|---|---|
| `flags` | uint8_t | **packed 状態 byte**。 bit 0-2: `kind`、 bit 3-5: `marked` / `old` / `dirty` のうち backend で必要な分。 `HDR_KIND(h)` / `HDR_MARKED(h)` / `HDR_OLD(h)` / `HDR_DIRTY(h)` のマクロでアクセス。 |
| `kind` (bit 0-2) | 3 bits | オブジェクトの種類タグ。 `KIND_OBJ_ARRAY` / `KIND_OBJ_STRING` / `KIND_PAYLOAD_VAL` (= VALUE[]、 BaArray.items の中身) / `KIND_PAYLOAD_BYTE` (= char[]、 BaString.bytes の中身) / `KIND_FREE` (freelist 上の slot)。 5 種類 → 3 bit で足りる。 mark phase が outgoing 参照を辿るとき、 payload を何として解釈するか判定するのに使う。 |
| `size` | uint32_t | payload バイト数 (アライメント前の logical size)。 sweep でスロット境界を進めるとき + payload の中の VALUE 数 を求めるとき (KIND_PAYLOAD_VAL なら `size / 8` 個) に使う。 |
| `marked` (bit 3) | 1 bit | mark phase で「到達可能」 と印を付けたか。 sweep が free 対象か判定する。 mark 完了後にクリア。 mark_compact_gen / mark_bump_gen は bit 3、 mark_gen の場合も bit 3。 |
| `old` (bit 3 or 4) | 1 bit | gen 系 backend で「tenured (= 旧世代) に居る」 か。 minor は old skip、 promote 時に set。 marked と同居する backend は bit 4 (mark_gen / mark_compact_gen / mark_bump_gen)、 copy_gen 系は marked field 不要なので bit 3。 |
| `dirty` (bit 4 or 5) | 1 bit | gen 系で「この (old) obj が remset に既に入ってる」 か。 同じ old obj を二重 push しないための重複防止 flag。 WB が「old & ! dirty なら remset push + dirty set」。 minor 時にクリア。 |
| `young_next` (廃止) | — | iter 33 で削除。 `mark_gen` / `mark_gen_inc` は以前 per-header の linked list ポインタを持っていたが、 external `young_objs[]` 配列に移行。 header 16→8 B、 BaArray が slab class 32 に収まり cache locality 向上。 |
| `fwd` | void * | moving GC (copy* / mark_compact*) で **移動先 payload アドレス**。 同じ obj への 2 回目以降の参照が deref したとき、 元アドレスに置かれた fwd を辿ると新アドレスへ。 |
| `mark_epoch` | uint8_t | immix family の「sticky-mark 風」 mark 表現。 GC ごとに global `cur_epoch` を +1 し、 mark phase が `h->mark_epoch = cur_epoch`。 epoch tick で旧 mark を heap walk なしに無効化。 immix では別 byte (`flags` の隣)。 |
| `_pad[N]` | uint8_t | 構造体の **末尾 padding**。 GCHeader 全体のサイズを 8 の倍数に揃えて、 payload (= h+1) が 8-aligned になるようにする。 中身は使わない (read/write 不要)。 |

#### backend ごとの header

**iter 31 で全 backend が `flags` byte packing で compact 化** — `kind` (uint32 → 3 bit)、 `marked` / `old` / `dirty` (bool → 各 1 bit) を全部単一 byte に同居。

| Backend | GCHeader (B) | 内容 |
|---|---:|---|
| `none` | — | header なし (pure malloc、 size 情報も持たない) |
| `bump` | 8 | kind (uint32), size — 元から 8 B、 GC 不要なので flag bit 不要 |
| `mark` | **8** | flags(kind+marked), _pad[3], size |
| `mark_gen` / `mark_gen_inc` | **8** | flags(kind+marked+old+dirty), _pad[3], size — young set は external `young_objs[]` 配列 (iter 33) |
| `mark_bitmap_gen` | 8 | kind (uint32), size — mark/old/dirty bits は per-page bitmap へ (元から 8 B) |
| `copy` | **16** | flags(kind), _pad[3], size, `fwd` |
| `copy_gen` / `copy_gen_inc` | **16** | flags(kind+old+dirty), _pad[3], size, `fwd` (marked 不要 — fwd で代用) |
| `mark_compact` | **16** | flags(kind+marked), _pad[3], size, `fwd` |
| `mark_compact_gen` | **16** | flags(kind+marked+old+dirty), _pad[3], size, `fwd` |
| `mark_bump_gen` | **16** | `fwd` + flags(kind+marked+old+dirty), _pad[3], size |
| `immix` | **8** | flags(kind), `mark_epoch`, _pad[2], size |
| `immix_gen` | **8** | flags(kind+old+dirty), `mark_epoch`, _pad[2], size |

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

世代別 / incremental backend では、 **ヒープから別ヒープへのポインタ書込**
は必ず `aro_gc_wb` を経由する:

```c
aro_gc_wb(holder, slot, v);     // *slot = v + WB 処理
```

非 gen backend では `aro_gc_wb` は `*slot = v` に inline 化 (zero cost)。

## 2. ヒープ管理パターン (alloc 戦略)

14 backend は **4 種類のヒープ管理パターン** のいずれか (またはその組合せ)
を採用。

### パターン A: Bump allocator

```
region: [oooooooo<-top                   end]
                ↑                          ↑
                次の alloc はここ          境界
```

連続領域に「次の空き位置」 を 1 ポインタ (`top`) で覚えておき、 alloc は
`top` を進めるだけ。 速い。 collect で領域全体を解放するか、 moving GC の
受け側として使う。

**採用 backend**: `bump`、 各 gen 系の **nursery** 部分 (copy_gen 等)、
moving GC の to-space (copy / mark_compact)。

### パターン B: Slab / Page allocator

```
class 32:  [page]──[page]──[page]
            slot 0:  [hdr][24 B payload]
            slot 1:  [hdr][24 B payload]
            ...
            slot N:  (freelist next pointer or live obj)
class 64:  [page]──[page]
class 128: [page]
...
```

固定サイズの **slot** が並ぶ **page** (典型的に 16 KiB) を、 サイズ毎の
**class** に分けて管理。 alloc は class の **freelist** から 1 slot pop。
freelist が空なら新 page を mmap して populate。

**採用 backend**: `mark`、 `mark_gen`、 `mark_gen_inc`、 `mark_bitmap_gen`。

古典的な malloc 系の親戚で、 CRuby の heap_page とよく似た形。

NB: alloc 戦略は **moving / non-moving と直交**。 slab/page allocator 上で
collect 時に object を移動する設計も可能 (例えば size class 単位で compact
する、 別 page にコピーして元 page を解放、 等)。 baruby_precise の現状実装
は採用 backend 全部が non-moving だが、 これは「slab だから non-moving」
という必然ではなく **この testbed の実装選択**。 「mark で compact しない」
+「mark_compact_gen は別 region 設計を採用してる」 という都合で、 結果的に
slab + non-moving の組合せしか居ない、 という状態。

### パターン C: Semispace (Cheney)

```
active space:        from space:
[oooooooooo<-top]    [retired]
       ↑
       alloc が進む

collect 時:
   active を from-space に格下げ
   alt 領域を新 active として bump 再開
   生きてるオブジェクトを from → active へ Cheney scan-loop で copy
```

**2 つの等サイズ領域** を交互に使う。 alloc は active 側で bump。 collect
時は active を from-space にして、 live なオブジェクトを「もう一方」 へ
copy する Cheney アルゴリズム。 alloc が極めて速く、 fragmentation 無し。
代償: heap の 2 倍の virtual 空間 + 生きてるオブジェクトのコピーコスト。

**採用 backend**: `copy`、 `copy_gen` (tenured 部分)、 `copy_gen_inc`。

### パターン D: Block + Line region (Immix)

```
arena (1 つの大きな mmap):
  block 0 (32 KiB): [line 0][line 1]...[line 255]   ← lines は 128 B
  block 1 (32 KiB): [line 0][line 1]...[line 255]
  ...

per-block bitmap: [mark_lines | mark_objs | dirty]
                  「この line に生きてる obj が居る」 を bit で記録

alloc:
  block の中で「連続した unmarked line の run」 = "hole" を見つけ、
  hole 内で bump alloc。 hole が尽きたら次の block / 次の hole へ。
```

**block (32 KiB) を line (128 B) に細分**、 mark phase で line 単位に
"このサイクル live obj が居た" を記録、 未使用 line の run (= hole) で
bump alloc する。 moving と non-moving の中間。

**採用 backend**: `immix`、 `immix_gen` (tenured 部分)。

### パターン E: 何もしない (no GC)

`none` / `bump` は alloc だけして free しない (leak baseline)。 「rooting
overhead + WB API の zero-cost 化」 を測る floor として有用。

## 3. 15 backend の早見表 (iter 35-36 fair contract 後)

`make GC=<name>` で切替え。 default = `copy`。 Header size は iter 31-33
の flags-byte packing 後の値。 7 番 `copy_gen_inc` は実体が `copy_gen` と
同一なので comparison から除外 (#7 の slot は予約)。

| # | Name | パターン | Gen? | Moving? | Major | Header | Remset | 強み | 弱み |
|---|---|---|---|---|---|---:|---|---|---|
| 1 | `none` | malloc + leak | — | — | 無 | — | — | baseline floor | leak |
| 2 | `mark` | Slab page | — | — | mark&sweep | **8** | — | 安定、 alloc 速 | 全 heap scan |
| 3 | `mark_gen` | Slab + young_objs[] | yes | — | sticky M&S | **8** | obj-level | minor O(young) | obj remset 上限あり |
| 4 | `mark_gen_inc` | mark_gen + SATB | yes | — | 増分 M&S | **8** | obj-level | 短 pause | 同上 |
| 5 | `copy` | Semispace | — | yes | Cheney 全コピー | **16** | — | alloc 最速 | 2× VA |
| 6 | `copy_gen` | bump nursery + semispace tenured | yes | yes | Cheney over tenured | **16** | obj-level | nursery 完結率高 | 同上 |
| 7 | (`copy_gen_inc`) | (placeholder; copy_gen の clone) | — | — | — | — | — | — | not benchmarked |
| 8 | `mark_compact` | 単一 region + Lisp-2 slide | — | yes | mark + compact | **16** | — | 1× VA、 in-place | sweep 重 |
| 9 | `mark_compact_gen` | bump nursery + bump tenured + slide | yes | yes | mark + slide | **16** | obj-level | tenured 再利用効率 | major 複雑 |
| 10 | `bump` | bump only | — | — | 無 | 8 | — | alloc floor | leak |
| 11 | `mark_bump_gen` | bump nursery + bump tenured (no compact) | yes | yes (minor) | mark + region sweep | **16** | obj-level | minor 速 | 累積 |
| 12 | `immix` | block / line region | — | — | mark + line-bitmap sweep | **8** | — | hole-based alloc | fragmentation |
| 13 | `immix_gen` | bump nursery + Immix tenured | yes | yes (minor) | Immix mark + sweep | **8** | obj-level | both | 同上 |
| 14 | `mark_bitmap_gen` | Slab + per-page bitmap | yes | — | sticky M&S (bitmap) | **8** | obj-level (bm dirty) | 8B header、 密度 2× | minor O(heap)、 locate cost |
| 15 | `mark_card_gen` | mark_bitmap_gen + page-level remset | yes | — | sticky M&S | **8** | **page-level** | remset 上限 = page count (bounded) | inner-walk overhead |

### Remset 設計 (iter 36)

全 gen backend は object-level dirty bit + 動的配列 remset を採用。 iter 36
で以下を強化:
- **Overflow cap**: `MAX_REMSET_ENTRIES = 128K (= 1 MiB ptr 配列)`。 越えたら
  `remset_overflow` フラグを立てて push を skip (dirty bit は header に残す)。
- **Heap-walk fallback** (mark_gen / mark_gen_inc / copy_gen / mark_compact_gen
  / mark_bump_gen): overflow 時、 minor が全 page を O(heap) で走査して dirty
  olds を見つける。 bounded fallback。
- **Abort on overflow** (immix_gen / mark_bitmap_gen): heap walk 実装が複雑な
  ため未対応。 明示的に abort + diagnostic で誤動作回避。
- **`mark_card_gen` (#15)**: 根本対策 — remset entry が page-level なので
  容量爆発しない。 Inner walk は per-page で O(slots/page) 一定。 ユーザー
  提案 (iter 36 "card (page) ごとに remset に入れて 2段階 で dirty 列挙")
  に基づく設計。

実際の現 bench での peak |remset| は:
- binary_trees: ~22 (mark_gen)、 2 pages (mark_card_gen)
- hash_chain, string_concat: 0
- remset_pressure: 数千 entries 級 (mark_gen)、 2 pages (mark_card_gen)

## 4. 各 backend のアルゴリズム

### 4.1 `none` — GC 無し (baseline)

`malloc` を直接叩いて leak。 GC code path 自体のオーバヘッド (sp[]
rooting、 alloc API 間接化等) を測るための floor。

### 4.2 `mark` — non-moving mark&sweep + slab

slot をサイズクラス別の page で管理 (パターン B)。

- **Heap 拡張**: class ごとに linked list で page (16 KiB) を追加。
  freelist 空 → new_page() で 16 KiB mmap → page 内 slot を freelist に
  populate。 page 数に上限なし (heap が必要なだけ伸びる)。
- **Alloc**: class の freelist から 1 pop。
- **GC trigger**: `bytes_since_gc > gc_threshold` (adaptive、
  `max(16 MiB, 2 × live_post_sweep)`)。
- **Mark**: roots から `mark_value` → gray queue で BFS。
- **Sweep**: 全 page の全 slot を walk。 unmarked → freelist に push。
  marked → marked クリア。

短命と長命が混ざる workload で安定して速い。 但し alloc-heavy で大量の
short-lived は class spread が起き易い。

### 4.3 `mark_gen` — mark&sweep + 2 generation

`mark` を gen 化。 GCHeader に `young_next` (linked list) と `old` /
`dirty` bit。

- **Alloc**: `mark` と同じ slab。 alloc 時に young list 先頭に link。
- **Minor**: roots から mark、 old は skip (`in_minor && h->old`)。
  remset 処理 (dirty old から発する young 参照を mark)、 young list を
  walk して unmarked young は freelist 返却、 marked young は **in-place
  promote** (`old = true` set のみ — 移動なし)。
- **Major**: 全部 mark、 young list + 全 page を sweep。
- **WB**: `*slot=v` 後、 holder が old & dirty=false なら remset push。

「sticky mark-bits」 系の典型実装。 young 集合を **linked list で track**
するので minor sweep が O(young)、 ただし header に 8 B 余分が要る。

### 4.4 `mark_gen_inc` — mark_gen + 増分マーキング

`mark_gen` の major mark を細切れに実行できるよう SATB barrier + gray queue
を装備。 v1 は infrastructure のみ (STW として動作)、 短い single pause を
複数の中程度 pause に分割する fall-back ばらつきが見える。

### 4.5 `copy` — Cheney semispace (default)

パターン C。 2 つの 64 GiB virtual region を交互に使う。

- **Heap 拡張**: 各 region は **64 GiB virtual + MAP_NORESERVE** で予約済
  (起動時に mmap)。 物理 page は active_top の前進で OS が demand-paging
  で commit。 拡張は無く、 region 内で前進・swap。
- **Alloc**: active 側で bump (`active_top += total`)。
- **GC trigger**: `bytes_since_gc > gc_threshold` (adaptive、
  `max(16 MiB, 2 × live_post_cheney)`、 iter 29 で追加)。 region 容量
  到達でも fallback で発火。
- **Collect**: active を from-space 化、 alt を to-space として bump 開始、
  roots から `forward_value` で Cheney scan-loop で copy。 swap して終了。
- **Stress mode**: 古い from-space は PROT_NONE + MADV_DONTNEED で恒久
  retire (stale 参照 deref で即 SIGSEGV)。

alloc は最も単純で速いが、 collect 時に **全 live を copy** するコストが
binary_trees のような long-live tree workload で目立つ。

### 4.6 `copy_gen` — Cheney nursery + Cheney tenured

nursery (16 MiB bump) + tenured (64 GiB virtual × 2 semispace)。

- **Heap 拡張**: nursery 16 MiB 固定。 tenured は 2 × 64 GiB virtual 予約済、
  Cheney swap で交互利用、 物理は demand-paging。
- **Minor trigger**: nursery overflow (16 MiB)。
- **Major trigger**: `bytes_since_major > old_major_threshold` (adaptive、
  `max(16 MiB, 2 × live)`、 iter 29 で追加)。 tenured 容量到達でも fallback。
- **Minor**: nursery 生存者を Cheney で tenured へ copy promote、
  nursery を一括 reset。
- **Major**: tenured を Cheney で alt 側へ copy。 nursery は leading
  minor で先に折り畳む。
- **WB**: remset 経由。

string-heavy で大勝 (`string_concat`)。 ABI は `copy_gen_inc` と同一。

### 4.7 `copy_gen_inc` — copy_gen + SATB

`copy_gen` の major に SATB を装備した変種。 v1 は infrastructure。

### 4.8 `mark_compact` — single-region + Lisp-2 slide

64 GiB virtual の 1 region + bump alloc。

- **Heap 拡張**: 64 GiB virtual 予約済。 region_top の前進で物理 page を
  demand commit。 collect 時の slide で region_top が縮む。
- **Alloc**: region_top bump。
- **GC trigger**: `bytes_since_gc > gc_threshold` (adaptive、
  `max(16 MiB, 2 × live_post_compact)`、 iter 29 で追加)。
- **Collect**: mark from roots → forward-address pass (各 marked obj の
  新位置を計算) → update outgoing pointers → update roots → slide live
  objects (memmove)。

非 moving と moving のハイブリッド: 普段は固定アドレス、 collect 時のみ
slide で fragmentation を解消。

### 4.9 `mark_compact_gen` — nursery + tenured (slide compact)

nursery (16 MiB bump) + tenured (64 GiB virtual、 1 region で slide
compact)。

- **Heap 拡張**: nursery 固定 16 MiB + tenured 64 GiB virtual 予約済。
- **Minor trigger**: nursery overflow。
- **Major trigger**: `bytes_since_major > old_major_threshold` (adaptive、
  `max(16 MiB, 2 × live)`、 iter 29 で追加)。 tenured 容量到達でも fallback。
- **Minor**: nursery 生存者を Cheney で tenured top に append。
- **Major**: tenured を Lisp-2 slide compact。 nursery を leading minor で
  折り畳んでから実行。

**多くの bench で全 backend 中の最速** (cons_list / list_alloc / 等)。
density と locality のバランスがいい。

### 4.10 `bump` — bump alloc only (no GC)

alloc floor baseline。 GC 完全に削除して rooting / WB / dispatch の
最小 cost を見る。 region 64 GiB virtual 予約、 region_top bump、 OOM 時 abort。

### 4.11 `mark_bump_gen` — bump nursery + bump tenured (no compact)

`mark_compact_gen` から compact を抜いた変種。

- **Heap 拡張**: nursery 固定 16 MiB + tenured 64 GiB virtual 予約済。
- **Minor trigger**: nursery overflow。
- **Major trigger**: `old_alloc_since_major > old_major_threshold` (adaptive、
  `max(16 MiB, 2 × old_post_sweep)`)。
- **Tenured**: bump alloc、 sweep は region 走査 (`p += sizeof(GCHeader) +
  ALIGN8(h->size)`) で linked list 不要。
- **Major**: mark + region sweep (compact なし)。 dead slot は領域内で
  leak、 累積で 64 GiB 使い切ったら OOM。

compact 無しの累積 leak で性能上限が見える。 string_concat 等 short-lived
が支配する bench では accumulated leak が問題にならず速い。

### 4.12 `immix` — block / line mark-region (no evac, v1)

パターン D。 64 GiB arena を 32 KiB block × 256 line に分割、 per-block
line bitmap で mark。

- **Heap 拡張**: 64 GiB virtual で予約済、 **32 KiB block 単位** で
  `max_touched_block++` して逐次 touch (= sweep / find_hole が walk する
  範囲を実必要分のみに抑える)。
- **Alloc**: 現在の hole (= unmarked line の run) で bump、 hole 尽き
  なら次の hole 探索。 hole が無ければ次 block を touch して伸長。
- **GC trigger**: `bytes_since_gc > gc_threshold` (adaptive、
  `max(16 MiB, 2 × live)`)。
- **Mark**: 普通の mark に加え `mark_lines_for(h)` で span line を bitmap
  set。 mark epoch counter で前回 cycle の bit を自動 invalidate。
- **Sweep**: per-block の line_marks 集計、 全 free → BLK_FREE、 mix →
  BLK_RECYCLABLE。 in-place、 オブジェクト移動なし。

v1 は **evacuation 無し**。 fragmentation が累積するが、 mark-line 粒度の
sweep は cache-friendly。

### 4.13 `immix_gen` — bump nursery + Immix tenured

`immix` を gen 化。

- **Heap 拡張**: nursery 16 MiB 固定 + tenured 64 GiB virtual arena (immix
  と同じ block 単位)。
- **Minor trigger**: nursery overflow。
- **Major trigger**: `bytes_since_major > major_threshold` (adaptive、
  `max(16 MiB, 2 × live)`)。
- **Nursery**: 16 MiB bump。
- **Minor**: nursery 生存者を Immix の hole に Cheney-copy promote。
  forwarding は `oldh->kind = KIND_FREE` + payload 先頭 8 byte に新 ptr。
- **Major**: leading minor → Immix mark + line-bitmap sweep。

short-live 系で `immix` を上回るが、 binary_trees 等 long-live tree
workload は Cheney copy が逆効果。

### 4.14 `mark_bitmap_gen` — sticky M&S + per-page bitmap

`mark_gen` の **「semantics 同じ、 実装が違う」 変種**。

- **Heap 拡張**: slab と同じく **16 KiB page 単位** で追加。 ただし page を
  **16 KiB aligned** で mmap (over-mmap して trim) して `ptr & ~0x3fff` で
  O(1) で page base 取得を可能にしている。
- **GCHeader 8 B** (kind, size のみ) — 元 24 B から大幅減
- **mark / old / dirty bit** は per-page bitmap (64 B × 3 = 192 B/page)
- **young_next 廃止** → minor sweep は全 page walk (O(heap))
- **Minor trigger**: `bytes_since_gc > MINOR_THRESHOLD` (16 MiB 固定)
- **Major trigger**: `old_alloc_since_major > old_major_threshold` (adaptive
  `max(16 MiB, 2 × live)`)

副次効果: 8 B header で **BaArray (24 B payload) が class 32 にぴったり
収まる** (`mark_gen` は class 64 で 40% waste)。 hash_chain で -29% 改善。
binary_trees 等は minor sweep O(heap) で不利。

## 5. 設計空間の俯瞰

14 backend を「nursery 戦略 × tenured 戦略 × compact」 の 3 軸で並べる:

| Backend | Nursery 戦略 | Tenured 戦略 | Compact? | Gen? |
|---|---|---|---|---|
| `none` | — | malloc (leak) | — | no |
| `bump` | — | bump (leak) | — | no |
| `mark` | — | slab + page | no | no |
| `copy` | — | semispace (2 region) | Cheney | no |
| `mark_compact` | — | bump (1 region) | Lisp-2 slide | no |
| `immix` | — | block + line region | no (v1) | no |
| `mark_bitmap_gen` | — | slab + page bitmap (8B hdr) | no | yes (sticky) |
| `mark_gen` | slab + young list | slab | no | yes |
| `mark_gen_inc` | slab + young list | slab | no | yes + inc mark |
| `copy_gen` | bump | semispace (2 region) | Cheney | yes |
| `copy_gen_inc` | bump | semispace | Cheney | yes + inc mark |
| `mark_compact_gen` | bump | bump (1 region) | Lisp-2 slide | yes |
| `mark_bump_gen` | bump | bump (1 region) | no (累積) | yes |
| `immix_gen` | bump | block + line region | no (v1) | yes |

軸:
- **Nursery 戦略**: bump nursery (16 MiB の bump 単 region) vs slab young
  list (page 内で young と old が混在、 linked list で分離) vs 無し
  (non-gen / sticky)
- **Tenured 戦略**: bump (連続)、 semispace (2 region)、 slab (size class
  毎 page)、 block+line region (Immix)、 のいずれか
- **Compact 動作**: なし、 Cheney、 Lisp-2 slide、 のいずれか

## 6. ヒープ管理 — サイズ戦略と GC 発火条件

### 6.1 仮想ヒープ予約 (全 region 系 backend 共通)

連続領域を持つ backend (`copy*` / `mark_compact*` / `mark_bump_gen` /
`immix*` / `bump`) は **64 GiB を MAP_NORESERVE で mmap 予約** している:

```c
ARO_GC_REGION_VIRT_BYTES = 64 GiB     /* gc.h */
mmap(NULL, 64 GiB, PROT_READ|PROT_WRITE,
     MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);
```

- **virtual** には 64 GiB を確保するが、 **物理** page は最初の write で
  初めて commit (4 KiB 粒度の demand-paging)
- `MAP_NORESERVE` で overcommit_memory=2 環境でも失敗しない
- prgoram-limiting cap として機能しない (iter 27 でこれ意図的に撤廃)

つまり「上限 64 GiB の virtual 予約」 ≠ 「上限まで GC を発火させない」 。
両者は別軸:
- **virtual 予約**: アドレス空間を 64 GiB 確保 (物理 commit は別)
- **GC 発火条件**: 後述の `bytes_since_gc > threshold` で決まる

### 6.2 GC 発火条件 — adaptive threshold

iter 29 以降、 **全 GC 系 backend で統一**:

```
trigger:  bytes_since_gc > gc_threshold
発火後:    gc_threshold = max(GC_THRESHOLD_MIN, 2 × live_post_collect)
GC_THRESHOLD_MIN = 16 MiB
```

つまり:
- 初回 GC は **16 MiB alloc** で発火 (小さい heap で頻繁に GC)
- 以降、 GC ごとに live size を確認、 次の trigger は live × 2 (= heap が 2 倍
  に膨らんだら GC) で adaptive 化
- 例: live=1 MB → 次の trigger は 16 MiB MIN まで cap、 live=100 MB → 次は 200 MB

これにより live が大きい workload では GC 頻度が自動的に低下、 live が
小さい workload では頻繁に GC してメモリ圧迫を抑える。

**設定根拠**:
- 16 MiB MIN: nursery と同サイズで、 minor / non-gen collection が一致 cadence
- factor 2: 「heap が 2 倍に膨らむまで放置」 = doubling growth、 amortized
  O(N) maintenance

### 6.3 backend ごとのヒープ拡張単位

| backend | 拡張単位 | 拡張トリガ |
|---|---|---|
| `mark` / `mark_gen` / `mark_gen_inc` | **16 KiB page** (slab) | freelist 空、 size class に新 page 必要時 |
| `mark_bitmap_gen` | **16 KiB page** (16 KiB-aligned mmap) | 同上 |
| `mark_compact` | (仮想予約済) — 64 GiB 内で region_top 進行 | bump のみ、 物理は touch で lazy commit |
| `mark_compact_gen` | nursery 固定 16 MiB + 64 GiB virtual tenured | 同上 |
| `copy` | 2 × 64 GiB virtual region | 半空間切替時に alt 側が touch される |
| `copy_gen` / `copy_gen_inc` | nursery 16 MiB + 2 × 64 GiB tenured | 同上 |
| `bump` | 64 GiB virtual region | bump、 拡張なし (leak) |
| `mark_bump_gen` | nursery 16 MiB + 64 GiB virtual tenured | bump、 物理は lazy |
| `immix` / `immix_gen` | **32 KiB block** (Immix arena 内) | hole 枯渇時に `max_touched_block++` で次 block touch |

整理:
- **slab 系** (mark family / mark_bitmap_gen): heap は **16 KiB page** 単位
  で追加される。 ある size class が freelist 枯渇 → mmap で新 page →
  freelist populate。 page 数は無制限 (per-class linked list)。
- **region 系** (copy / mark_compact / bump / gen 系 tenured): heap は
  **virtual 64 GiB 予約済**、 拡張は OS の demand-paging に委任 (実質 4 KiB
  page 単位で物理 commit)。 「page を追加する」 という明示処理は無く、
  region_top の進行が自動的に新 page を touch する。
- **Immix arena** (immix / immix_gen): 32 KiB block 単位で論理拡張。
  `find_hole` が touched 範囲で hole 見つけられないとき `max_touched_block++`
  して次の virtual block を touch、 全 block 一括 hole として返す。

### 6.4 fairness 設定の対比表

各 backend を「同じ workload に同じ条件」 で当てるため、 以下を揃えている:

| 設定 | 値 | 統一範囲 |
|---|---|---|
| 仮想ヒープ上限 (region 系) | **64 GiB** | 全 region 系 backend |
| Nursery size (gen 系) | **16 MiB** | 全 gen 系 |
| GC threshold MIN | **16 MiB** | 全 GC 系 (none / bump 除く) |
| Threshold factor | **2 × live** | max(MIN, 2 × live) で adaptive、 全 GC 系 |
| Minor 発火 (gen 系) | nursery overflow (= 16 MiB) | 全 gen 系 |
| Major 発火 (gen 系) | `bytes_since_major > threshold` | 全 gen 系 (iter 29 で移動 gen に新規追加) |

**iter 29 で揃えた点**:
- `copy` / `mark_compact` に `bytes_since_gc > threshold` の adaptive
  trigger を新規追加 (それ以前は region 容量基準のみ = 64 GiB virtual で
  実質発火せず、 bump 同然 → 不公平に速い数値だった)
- `copy_gen` / `copy_gen_inc` / `mark_compact_gen` に MAJOR threshold を
  新規追加 (同じ理由で major が実質発火していなかった)
- 全 backend で MIN を **16 MiB に統一** (一部は 4 MiB / 64 MiB の不揃いだった)

これにより全 backend が「同じ alloc 量で同じ回数 GC を打つ」 状態になり、
algorithm の真の差が見える比較になる。

## 7. どれを使うべきか

ベンチ結果 (perf.md §2 の勝者分布) と合わせると:

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
