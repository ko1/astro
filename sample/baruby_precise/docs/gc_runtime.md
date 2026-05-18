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

GCHeader のサイズは backend で違う:

| Backend | GCHeader (B) | 内容 |
|---|---:|---|
| `mark` | 16 | kind, size, marked, _pad |
| `mark_gen` / `mark_gen_inc` | 24 | + `young_next`, `old`, `dirty` |
| `mark_bitmap` | **8** | kind, size のみ — bits は per-page bitmap へ |
| `copy*` / `mark_compact*` | 24 | + `fwd` (forward ptr), `old`, `dirty` |
| `bump` | 8 | kind, size のみ (no GC) |
| `immix*` | 16 | + `mark_epoch`, `flags` (gen) |

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

**採用 backend**: `mark`、 `mark_gen`、 `mark_gen_inc`、 `mark_bitmap`。

非 moving なので alloc/free を繰り返しても**アドレスが安定する** = root の
forwarding が不要。 古典的な malloc 系の親戚で、 CRuby の heap_page と
よく似た形。

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

## 3. 14 backend の早見表

`make GC=<name>` で切替え。 default = `copy`。

| # | Name | パターン | Gen? | Moving? | Major アルゴリズム | Header | 強み | 弱み |
|---|---|---|---|---|---|---:|---|---|
| 1 | `none` | malloc + leak | — | — | 無 | 16 | baseline | leak |
| 2 | `mark` | Slab + page | — | — | mark&sweep | 16 | 安定、 alloc 速 | 全 heap scan |
| 3 | `mark_gen` | Slab + young list | yes | — | sticky M&S | 24 | minor O(young) | 24 B header |
| 4 | `mark_gen_inc` | Slab + young list + SATB | yes | — | 増分 mark&sweep | 24 | 短 pause | 同上 |
| 5 | `copy` | Semispace | — | yes | Cheney 全コピー | 24 | alloc 最速、 no frag | 2× VA、 全 copy |
| 6 | `copy_gen` | bump nursery + semispace tenured | yes | yes | Cheney over tenured | 24 | nursery 完結率高 | 同上 |
| 7 | `copy_gen_inc` | copy_gen + SATB | yes | yes | 増分 Cheney | 24 | 短 pause | 同上 |
| 8 | `mark_compact` | 単一 region + Lisp-2 slide | — | yes | mark + compact | 24 | 1× VA、 in-place | sweep 重 |
| 9 | `mark_compact_gen` | bump nursery + bump tenured + slide | yes | yes | mark + slide | 24 | tenured 再利用効率 | major 複雑 |
| 10 | `bump` | bump only | — | — | 無 | 8 | alloc floor | leak |
| 11 | `mark_bump_gen` | bump nursery + bump tenured (no compact) | yes | yes (minor) | mark + region sweep | 24 | minor 速 | 累積 |
| 12 | `immix` | block / line region | — | — | mark + line-bitmap sweep | 16 | hole-based alloc | fragmentation |
| 13 | `immix_gen` | bump nursery + Immix tenured | yes | yes (minor) | Immix mark + sweep | 16 | both | 同上 |
| 14 | `mark_bitmap` | Slab + per-page bitmap | yes | — | sticky M&S (bitmap) | **8** | 24B → 8B header、 密度 2× | minor O(heap) |

## 4. 各 backend のアルゴリズム

### 4.1 `none` — GC 無し (baseline)

`malloc` を直接叩いて leak。 GC code path 自体のオーバヘッド (sp[]
rooting、 alloc API 間接化等) を測るための floor。

### 4.2 `mark` — non-moving mark&sweep + slab

slot をサイズクラス別の page で管理 (パターン B)。

- **Alloc**: class の freelist から 1 pop。 空なら new_page で 16 KiB
  mmap して slot を populate。
- **GC trigger**: `bytes_since_gc > gc_threshold` (adaptive、
  `max(4 MiB, 2 × live_post_sweep)`)。
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

- **Alloc**: active 側で bump (`active_top += total`)。
- **Collect**: active を from-space 化、 alt を to-space として bump 開始、
  roots から `forward_value` で Cheney scan-loop で copy。 swap して終了。
- **Stress mode**: 古い from-space は PROT_NONE + MADV_DONTNEED で恒久
  retire (stale 参照 deref で即 SIGSEGV)。

alloc は最も単純で速いが、 collect 時に **全 live を copy** するコストが
binary_trees のような long-live tree workload で目立つ。

### 4.6 `copy_gen` — Cheney nursery + Cheney tenured

nursery (16 MiB bump) + tenured (64 GiB virtual × 2 semispace)。

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

- **Alloc**: region_top bump。
- **Collect**: mark from roots → forward-address pass (各 marked obj の
  新位置を計算) → update outgoing pointers → update roots → slide live
  objects (memmove)。

非 moving と moving のハイブリッド: 普段は固定アドレス、 collect 時のみ
slide で fragmentation を解消。

### 4.9 `mark_compact_gen` — nursery + tenured (slide compact)

nursery (16 MiB bump) + tenured (64 GiB virtual、 1 region で slide
compact)。

- **Minor**: nursery 生存者を Cheney で tenured top に append。
- **Major**: tenured を Lisp-2 slide compact。 nursery を leading minor で
  折り畳んでから実行。

**多くの bench で全 backend 中の最速** (cons_list / list_alloc / 等)。
density と locality のバランスがいい。

### 4.10 `bump` — bump alloc only (no GC)

alloc floor baseline。 GC 完全に削除して rooting / WB / dispatch の
最小 cost を見る。 OOM 時 abort。

### 4.11 `mark_bump_gen` — bump nursery + bump tenured (no compact)

`mark_compact_gen` から compact を抜いた変種。

- **Tenured**: bump alloc、 sweep は region 走査 (`p += sizeof(GCHeader) +
  ALIGN8(h->size)`) で linked list 不要。
- **Major**: mark + region sweep (compact なし)。 dead slot は領域内で
  leak、 累積で 64 GiB 使い切ったら OOM。

compact 無しの累積 leak で性能上限が見える。 string_concat 等 short-lived
が支配する bench では accumulated leak が問題にならず速い。

### 4.12 `immix` — block / line mark-region (no evac, v1)

パターン D。 64 GiB arena を 32 KiB block × 256 line に分割、 per-block
line bitmap で mark。

- **Alloc**: 現在の hole (= unmarked line の run) で bump、 hole 尽き
  なら次の hole 探索。 hole が無ければ次 block を touch して伸長。
- **Mark**: 普通の mark に加え `mark_lines_for(h)` で span line を bitmap
  set。 mark epoch counter で前回 cycle の bit を自動 invalidate。
- **Sweep**: per-block の line_marks 集計、 全 free → BLK_FREE、 mix →
  BLK_RECYCLABLE。 in-place、 オブジェクト移動なし。

v1 は **evacuation 無し**。 fragmentation が累積するが、 mark-line 粒度の
sweep は cache-friendly。

### 4.13 `immix_gen` — bump nursery + Immix tenured

`immix` を gen 化。

- **Nursery**: 16 MiB bump。
- **Minor**: nursery 生存者を Immix の hole に Cheney-copy promote。
  forwarding は `oldh->kind = KIND_FREE` + payload 先頭 8 byte に新 ptr。
- **Major**: leading minor → Immix mark + line-bitmap sweep。

short-live 系で `immix` を上回るが、 binary_trees 等 long-live tree
workload は Cheney copy が逆効果。

### 4.14 `mark_bitmap` — sticky M&S + per-page bitmap

`mark_gen` の **「semantics 同じ、 実装が違う」 変種**。

- **GCHeader 8 B** (kind, size のみ) — 元 24 B から大幅減
- **mark / old / dirty bit** は per-page bitmap (64 B × 3 = 192 B/page)
- **page を 16 KiB aligned** で mmap → `ptr & ~0x3fff` で O(1) で
  page base 取得
- **young_next 廃止** → minor sweep は全 page walk (O(heap))

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
| `mark_bitmap` | — | slab + page bitmap (8B hdr) | no | yes (sticky) |
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

## 6. Fairness — 比較するうえで揃えてある設定

各 backend を「同じ workload に同じ条件」 で当てるため、 以下を揃えている:

| 設定 | 値 | 補足 |
|---|---|---|
| 仮想ヒープ上限 | 64 GiB | 全 region 系 backend で virtual reservation + lazy commit (iter 27)。 `ARO_GC_REGION_VIRT_BYTES` (`gc.h`)。 |
| Nursery size | 16 MiB | 全 gen 系。 minor 頻度の tuning 共通化。 |
| Major threshold MIN | 64 MiB | 非moving sticky 系 (`mark_gen` / `mark_gen_inc` / `mark_bump_gen` / `mark_bitmap` / `immix_gen`)。 |
| Major threshold factor | 2 × live | 同上 (max(MIN, 2×live) で適応)。 |
| Minor threshold | nursery overflow / 16 MiB 相当 | 全 gen 系。 |

移動系 (`copy_gen` / `copy_gen_inc` / `mark_compact_gen`) は **明示 major
threshold を持たず**、 tenured 容量に達するまで minor のみ。 これは設計上
moving GC が minor copy で nursery を完全に回収できるため major を必要と
しない、 という性質に基づく — fair から外れるのではなく、 設計の違い。

`gc_combined.ba.rb` のような長寿命+短命混合 bench だと、 同じ collection
頻度同士で比較できるよう、 iter 29 で `immix_gen` と `mark_bitmap` の
MAJOR threshold を他の非moving sticky に揃えた (4 MiB → 64 MiB、 詳細
[done.md](done.md) iter 29 参照)。

## 7. どれを使うべきか

ベンチ結果 (perf.md §2 の勝者分布) と合わせると:

| Workload pattern | おすすめ |
|---|---|
| 短命大量 alloc + 長寿命少 (e.g. `string_concat`, `cons_list`) | `copy_gen`、 `mark_compact_gen`、 `mark_bump_gen`、 `immix_gen` |
| 長寿命優位 (`binary_trees`) | `bump`、 `copy`、 `mark_compact`、 `immix` |
| Hash table 系 (`hash_chain`) | `mark_bitmap` (24 B → 8 B header の density 効果)、 `bump`、 `immix_gen` |
| 構造 + churn mix (`gc_combined`, `list_alloc`) | `mark_compact_gen` 安定して上位 |
| Mutator-bound (`fannkuch`, `nqueens`) | どれでも近い (GC 比率小) |
| Latency 重視 | `*_inc` 系 (incremental mark で pause 短) |

汎用 default は `copy` (Cheney)、 GC を完全に抜いた baseline 比較は `none` /
`bump`、 worker memory が制約な systems では `mark_bitmap` (8B header) が
有利、 等。
