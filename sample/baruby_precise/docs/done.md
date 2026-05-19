# baruby Done

[spec.md](spec.md) — 言語仕様、[runtime.md](runtime.md) — 実装、
[todo.md](todo.md) — 残タスク、[perf.md](perf.md) — ベンチ。

## 2026-05-19 (42) — CRuby 比較 column + zero-init optimization speculation

### CRuby 比較
baruby benches は意図的に Ruby サブセットで書かれているので `ruby` (CRuby)
でも実行可能。 全 18 bench を CRuby 3.4 で median-of-3 計測し、
baruby_precise plain/AOT の最速 backend と並べた表を perf.md §2 末尾に追加。

結果:
- plain 幾何平均 1.83× faster than CRuby
- AOT  幾何平均 7.77× faster than CRuby
- plain で唯一 CRuby に負けるのは life (0.95×) / nqueens (0.92×) —
  mutator-bound で baruby の dispatch overhead が GC win を相殺
- AOT mode では全 bench で CRuby に勝利 (最低 4×、 最高 list_sort 34.6×)
- 特に string_concat AOT 19.9× は iter 37 の literal const-fold 効果

todo.md「CRuby の参考時間と並べる」 を完了マーク。

### zero-init optimization (試行 → revert)
node_call_N の callee local zero-init で arg slots (sp[0..N-1]) を skip
することで N store/call 節約を試みた。 が、 安全性検証で **NG**:

BARUBY_EVAL_ARG が child eval 中、 sp_top = sp + locals_cnt (callee scratch
top) を渡す。 GC scan range は `c->env..sp_top` なので callee locals 領域
sp[0..locals_cnt-1] も scan 対象。 zero-init を skip すると stale heap
pointer が GC に踏まれる危険 (false positive mark / corruption)。

実測でも改善は noise 範囲 (~1%) で、 correctness リスクに見合わず revert。
教訓: ASTro の precise GC では sp_top にまつわる scan range の不変条件を
壊さない optimization のみ可。 似たアイデアは「per-arg sp_top adjustment」
で実装する必要がある (sp_top = sp + i during arg i 評価) — 別 iter で
検討。

## 2026-05-19 (41) — New backend #16: `gc_mark_freelist`

「region + 非 compact + freelist」 という design point の demonstration として
新 backend を追加。 既存の `gc_mark` (slab + freelist) と `gc_mark_compact`
(region + compact) の中間。

### 設計
- **Layout**: 単一 bump region (64 GiB virtual lazy-paged)、 GCHeader 8 B
  (`mark` と同じ)、 9 size classes (32-4096 B、 `gc_mark.c` と同一)
- **Allocator**:
  1. 要求 payload を ALIGN8 して slot_total を計算
  2. size_class を引いて class freelist を試行 (LIFO pop)
  3. freelist が空なら region_top bump
  4. 大物 (slot_total > 4096) は large object に mmap
- **Mark**: 標準 BFS from roots、 H_MARKED bit を set、 gray_buf で walk
- **Sweep**: region を base→top に sequential walk、 各 slot で:
  - `HDR_KIND == KIND_FREE`: 既 freelist 上、 再 push
  - `HDR_MARKED`: clear mark
  - 上記以外 (= unmarked alive): KIND_FREE に変えて class freelist に push
  - size を保つことで次回 region walk が slot 境界を正しく辿れる
- **Write barrier**: 非 gen なので no-op (gc.h の static inline fallback)

### 既存 backend との比較
- vs `gc_mark` (#2、 slab page + linked list): malloc 介在なし、 page
  metadata なし。 freelist 自体は同様だが page chain がない。
- vs `gc_mark_compact` (#8、 region + Lisp-2 slide): compaction なし。
  forward/update/slide pass 不要 → 単純だが fragmentation あり。
- 同 region + non-compact の `mark_bump_gen` の non-gen 版 + freelist 付き
  と言える。

### 検証
全 18 bench で oracle pass を確認 (binary_trees / cons_list / dll_walk /
list_alloc / string_concat / substr_churn / remset_pressure を smoke test):
- binary_trees: 0.82 s
- cons_list:    0.78 s
- dll_walk:     0.78 s
- list_alloc:   0.77 s
- string_concat: 0.24 s
- substr_churn: 1.11 s
- remset_pressure: 0.35 s

`gc_mark` と比べて mix — small alloc 系で速め、 long-lived heavy で遅め。

### 追加変更
- Makefile に `GC_NUM_mark_freelist := 16` を追加
- gc.h に `BARUBY_GC_MARK_FREELIST 16` を追加
- bench/matrix.rb の `ALL_BACKENDS` に追加
- docs/runtime.md §5.x に #16 のセクション + §5.11 設計空間表に行追加
- docs/gc_runtime.md §3 早見表に行追加

## 2026-05-19 (38) — Remset overflow: heap-walk fallback for immix_gen + mark_bitmap_gen

iter 36 で全 7 gen backend に `MAX_REMSET=128K` cap を入れたが、 そのうち
5 backend (mark_gen / mark_gen_inc / copy_gen / mark_compact_gen /
mark_bump_gen) のみ heap-walk fallback を持ち、 `immix_gen` /
`mark_bitmap_gen` は `fprintf(stderr, ...) + abort()` だった。 iter 38 で
両 backend に fallback を追加。

### `mark_bitmap_gen`
Dirty bit が GCHeader でなく per-page `dirty_bm[64]` にあるので、 fallback
は単純: 全 page を size class ごとに辿り、 各 slot で `bm_get(pg->old_bm, i)
&& bm_get(pg->dirty_bm, i)` を条件に `scan_outgoing` を呼ぶ。 large は
`lo->old && lo->dirty` 直接。

```c
if (remset_overflow) {
    for (int sc = 0; sc < NUM_SIZE_CLASSES; sc++) {
        const size_t sb = size_class_bytes[sc];
        for (Page *pg = page_head[sc]; pg; pg = pg->next) {
            char *slot = (char *)pg + SLOTS_REGION_OFFSET;
            for (size_t i = 0; i < pg->n_slots; i++, slot += sb) {
                if (bm_get(pg->old_bm, i) && bm_get(pg->dirty_bm, i)) {
                    bm_clr(pg->dirty_bm, i);
                    scan_outgoing((GCHeader *)slot);
                }
            }
        }
    }
    /* + lo->old && lo->dirty 走査 */
    remset_overflow = false;
}
```

### `immix_gen`
Immix の line-allocator は per-slot bookkeeping を持たないので、 単純な
page walk ができない (lines はマークされるが「どこから object header が
始まるか」 を line_marks だけからは復元不能)。 そこで **`tenured_objs[]`
enumeration list** を導入:

- `large_alloc` と `forward_payload_nursery` (promote 後) で push
- `sweep_major` で `mark_epoch == cur_epoch` の entry のみ retain (compact)
- overflow 時の minor は remset_buf ではなく tenured_objs を walk

```c
static void
tenured_objs_push(GCHeader *const h)
{
    if (tenured_cnt >= tenured_capa) {
        tenured_capa = tenured_capa ? tenured_capa * 2 : 1024;
        tenured_objs = realloc(tenured_objs, tenured_capa * sizeof(GCHeader *));
    }
    tenured_objs[tenured_cnt++] = h;
}

/* in minor_gc, overflow path: */
for (size_t i = 0; i < tenured_cnt; i++) {
    GCHeader *const h = tenured_objs[i];
    if ((h->flags & H_OLD) && (h->flags & H_DIRTY)) {
        process_object_minor(h);
        h->flags &= (uint8_t)~H_DIRTY;
    }
}
```

Promotion path の cost は 1 store + 1 cap check (amortized realloc)。 list
size は major 後の live tenured count に bounded (compact で stale entry が
落ちる)。

### 検証
fault inject (cap を `1u << 7 = 128` に下げて再 build) で 4 bench
(binary_trees / cons_list / list_alloc / remset_pressure) を実行、 全 oracle
checksum pass。 cap 復元後 normal path も regression なし (matrix 比較)。

### 影響
これで 8 gen backend (mark_gen / mark_gen_inc / copy_gen / mark_compact_gen /
mark_bump_gen / immix_gen / mark_bitmap_gen / mark_card_gen) 全てが bounded
correctness を達成。 残る 7 backend (none / mark / copy / mark_compact /
bump / immix) は非 gen (remset 不使用) なので overflow 概念なし。
[gc_runtime.md §3](gc_runtime.md) の remset 表を更新。

### Perf trade-off と v2 (pressure-triggered minor)
v1 (`tenured_objs_push` per promotion) は cache write pressure で **5-15%
regression** が出た (binary_trees 0.74→0.84、 fib_pair 0.73→0.81、
list_alloc 0.64→0.71)。 試した最適化:
- `inline` + `__builtin_expect` で hot path 短縮 → 効果なし
- 64K 初期 capa で realloc 回数削減 → 効果なし
- 16 M entries (128 MB virtual) を mmap で preallocate → 効果なし
- Chunked linked list (1M entry chunks) で memcpy 回避 → 効果なし

本質的に「1M+ promotion 毎に外部 array へ 8 B write」 の cache pollution
が原因 — micro 最適化では消えない。

**v2 解決策 (pressure-triggered minor)**: `tenured_objs[]` を撤廃し、
代わりに `remset_push` で `remset_cnt >= MAX-1` になったら
`remset_pressure` flag を立てる。 次の alloc safepoint
(`nursery_bump`) が flag を check して minor を強制発火、 remset を drain。
WB 単体が hard cap (`MAX_REMSET_ENTRIES`) を超えるのは「alloc-less
adversarial loop」 のみで、 そこは abort + diagnostic で残す
(現実の Ruby workload では起きない)。

利点:
- promotion path に新コードなし — iter 37 と同じ hot path
- WB は 1 つの compare-and-set 増えるだけ (branch predicted taken)
- minor 自体は remset_buf を走査するだけ (iter 36 design 通り)
- fault-inject (cap=128) で 4 bench (binary_trees / cons_list /
  remset_pressure / list_alloc) を実行し全 oracle pass

mark_bitmap_gen は per-page `dirty_bm[]` の page scan で fallback (overhead
0)。

commit: `fe70397` (v1)、 `d654841` (v1 regression measurement)、 本 iter で
v2 commit。

## 2026-05-19 (37) — Perf 2: string literal const-fold + string_concat_dyn bench

`baruby_parse.c::alloc_binop` で `node_str_lit + node_str_lit` の op を
parse-time fold。 両 byte 列を `malloc` で連結して 1 つの `node_str_lit`
に縮約する。 `"a" + "b" + "c"` のような完全リテラル連結が 5 alloc / iter
から 1 static reference / iter に縮む。

実装は単純 (~12 lines):

```c
else if (ceq(tc, name, "+")) {
    extern const struct NodeKind kind_node_str_lit;
    if (lhs->head.kind == &kind_node_str_lit &&
        rhs->head.kind == &kind_node_str_lit) {
        uint32_t la = lhs->u.node_str_lit.len;
        uint32_t lb = rhs->u.node_str_lit.len;
        uint32_t total = la + lb;
        char *buf = (char *)malloc((size_t)total + 1);
        if (la) memcpy(buf, lhs->u.node_str_lit.bytes, la);
        if (lb) memcpy(buf + la, rhs->u.node_str_lit.bytes, lb);
        buf[total] = '\0';
        return ALLOC_node_str_lit(buf, total);
    }
    return ALLOC_node_add(lhs, rhs);
}
```

効果 (immix_gen):
- plain string_concat: 0.48 → 0.20 (-58%)
- AOT  string_concat: 0.34 → 0.07 (-79%)

ただし元の string_concat.ba.rb は意図 (string alloc を 5 個 / iter 測る)
を失う — fold で 1 個 / iter になる。 そこで `string_concat_dyn.ba.rb` を
追加:
- `make_chunk(i)` 関数で `i % 3` で異なる literal を返す → fold できない
- `a + b + c` で動的 concat (3 alloc / iter)
- 5_000_000 iter で oracle=45000000
- 結果: plain immix_gen 1.06s、 AOT immix_gen 0.39s — 本来の dynamic
  concat コストを保持

baruby (libgc) にも port (commit `bcecebd`):
- 同じ parser 修正
- 結果: string_concat 0.29s (libgc) — baruby_precise の immix_gen より
  遅い (0.20)。 dynamic 版は string_concat_dyn 1.51s で precise immix_gen
  1.06 の 1.4× (precise の bump nursery + line allocator が効く)

iter 37 final matrix (plain, 17 bench × 14 backend + libgc, median of 3):
- immix_gen 11 wins (cons_list / fib_pair / gc_combined / life / list_alloc /
  list_sort / remset_pressure / sieve / string_concat / string_concat_dyn /
  substr_churn) — iter 36 final の 7 wins から大幅拡大
- bump 3 wins (ast_eval / binary_trees / hash_chain)
- immix 2 wins (fannkuch / nqueens)
- copy 1 win (interp_calc)

教訓:
- **parse-time fold は bench の semantics を変える**。 win を喜ぶ前に
  「この最適化で bench が何を測らなくなるか」 を確認する必要がある。
  本来の workload を保存する別 bench を追加するのが対処。
- 文字列リテラルだけの fold は安全 (副作用なし、 immutable)。 変数を
  含む `s + "lit"` は元の semantics を保てないので skip すべき —
  Ruby の `String#+` は new string を返すので結果は同じだが、 オブジェクト
  identity / `__id__` の semantics が変わる (lazy 化されると frozen string
  cache を踏む)。 baruby は `__id__` を持たないので実害ないが、 一般化
  するときは要注意。

commits: `9a16099` (baruby_precise)、 `bcecebd` (baruby)。

## 2026-05-19 (36-final) — Perf 1 retry success (array literal 1-shot)

iter 36 で 1 度試して plain で regression と判断した `node_ary_lit_N`、
AOT mode で profiling し直したら違う picture だった:
- plain: DISPATCH 系 50% — array literal の savings がそこに隠れる
- AOT: DISPATCH SD bake で 7% に消える、 代わりに GC + memmove が 30% を
  占める。 alloc 削減の効果が直に出る

Retry の実装は前回と同じ (`node_ary_lit_{1,2,3,4}` + parser dispatch)。
clean rebuild + median-of-3 で測定し直すと真の win が確認できた:

plain mode (copy backend、 主な改善):
- fib_pair: 0.87 → 0.77 (-11%)
- gc_combined: 0.86 → 0.76 (-12%)
- list_alloc: 0.82 → 0.72 (-12%)
- interp_calc: 0.95 → 0.86 (-9%)

AOT mode (immix_gen backend):
- gc_combined: 0.28 → 0.19 (-32%)
- list_alloc: 0.30 → 0.19 (-37%)
- fib_pair: 0.31 → 0.26 (-16%)

baruby (libgc) にも port:
- binary_trees: 0.91 → 0.81 (-11%)
- cons_list: 0.99 → 0.90 (-9%)
- list_alloc: 1.03 → 0.96 (-7%)

教訓:
- **plain での regression 判断は noise + stale build の可能性高い**。
  ASTro 系は dispatch overhead が大きいので、 mutator path の最適化は
  AOT で見ないと真の signal が見えない。
- 「reviewer の見立てが間違ってる」 と早合点する前に measurement methodology
  を疑え。 clean rebuild + 複数 iteration の median を取る。

commits: `5fc85d0` (baruby_precise)、 `25815ea` (baruby)。

## 2026-05-19 (36) — AOT 修復 + Remset cap + mark_card_gen (#15) + macro benches

### AOT mode 修復
iter 35 で未着手だった「`-c` 起動時に `astro_cs_build: make failed (exit
512)`」 を解決:
- Makefile に `BARUBY_PRECISE_DIR` / `ASTRO_RUNTIME_DIR` / `ASTRO_PRISM_INC_DIR`
  の絶対パス macro を追加。
- main.c::common_build_flags_and_link で extra_cflags に
  `-I<abspath> -DBARUBY_GC=<n>` を埋めて astro_cs_build に渡す。
- node.c::astro_cs_init の version 引数に `BARUBY_GC` を渡して backend
  切替で code_store cache を自動 invalidate。

これで全 14 backend が `-c` AOT bake で動作。 動作確認 + perf 測定後、
plain mode から AOT mode で nqueens は 0.95s → 0.07-0.10s (15×)、 life
は 1.30 → 0.14-0.17s (10×) など mutator-bound bench で大幅高速化。

### matrix.rb 改良
- `--libgc-bin` (default `../baruby/baruby`) で sample/baruby (libgc) を
  並列 column として実行。 以前は手動 cross 比較だった。
- AOT/PG モードで `CCACHE_DISABLE=1` を auto-set + `code_store/` を bench
  ごとに clear (異 bench の SD pollution で fib_pair が 0.5 → 1.0s 劣化
  する問題回避)。

### Remset overflow guard (全 gen backend)
User からの「remset が膨張する危険性?」 指摘への対応:
- mark_gen / mark_gen_inc / copy_gen / mark_compact_gen / mark_bump_gen:
  `MAX_REMSET_ENTRIES=128K` cap + heap-walk fallback (overflow 時に全 page
  を O(heap) 走査して dirty olds を見つける)。 bounded fallback で
  silent corruption ゼロ。
- immix_gen / mark_bitmap_gen: cap + 明示的 abort (heap walk 実装が複雑
  で未対応、 次 iter で対応)。
- 現 bench での peak |remset| は最大 22 entries (binary_trees on mark_gen)、
  remset_pressure bench でも数千 entries。 128K cap には到達せず。

### mark_card_gen (#15) — page-level remset の新 backend
User 提案「card (page) ごとに remset に入れて、 card の中の dirty objects
を全列挙 (2段階)」 を実装:
- mark_bitmap_gen の page-aligned slab + per-slot dirty_bm を継承。
- Remset entry が `GCHeader*` → `Page*`。 同 page への複数 dirty write は
  `card_dirty` flag で 1 回 push に dedup。
- 上限 = heap_size / 16 KiB pages (例 64 GiB virtual → 4M pages max)。
- Minor は remset page を順走査 → page 内全 slot 走査 → dirty_bm 立った
  slot を scan_outgoing。 2段階 enumeration。
- remset_pressure で peak remset = **2 pages** (mark_gen の object-level
  だと数千 entries 相当)。 spatial-locality 利用で大幅メモリ節約。
- Raw 速度は mark_bitmap_gen と ±2% 以内。 本当の win は容量上限。

### 新 macro bench
- `bench/remset_pressure.ba.rb`: 50K-cell chain + 200K sparse young store。
  remset/WB の adversarial pressure test。 全 backend で oracle match。
- `bench/ast_eval.ba.rb`: 16K-node tree build + 200× iter eval。
  long-lived + short-lived の混在 workload。

### docs 全般見直し
- perf.md §2: iter 36 fair matrix (15 backend × 16 bench + libgc) 全面 refresh。
- gc_runtime.md §3: 早見表を 15 backend + remset 設計欄に拡張。
- todo.md: AOT の済み印 + remset overflow の対応状況追記。
- README: libgc 比較主張は iter 35 で取り下げ済。

### 性能観察 (iter 36 fair):
- `immix_gen` が 6 bench で最速 — line allocator + gen の balance 良好。
- `copy_gen` / `mark_compact_gen` / `mark_bump_gen` の gen 系が
  hash_chain で **1.23-1.27** に対し `mark` 1.65、 WB を活用できる
  bench で顕著。
- `binary_trees` は per-page bitmap 系 (`mark_bitmap_gen` 1.46 /
  `mark_card_gen` 1.47) が worst。 `locate()` overhead が 4M Array
  全 mark で効く。
- `bump` (no-GC) は binary_trees で他に倍速 (0.51)。

## 2026-05-18 (35) — Fairness contract: 7 件の比較不整合を一括修正

iter 34 で user から fairness 観点の指摘を 7 件受け、 比較契約全体を見直し。
各指摘とも妥当だったため、 perf.md / done.md の数値はすべて iter 35 で
再計測したものを正本にする。

### Critique と対応

1. **`copy_gen_inc` は実体が `copy_gen` の clone** — diff は comment と
   backend name string のみ、 incremental 実装 (inc_step / SATB) なし。
   matrix runner / comparison table から **除外**。 ファイル冒頭に honesty
   note。 将来 real incremental を実装する起点として file は保持。

2. **immix_gen の major trigger が `bytes_since_major` (= 全 alloc)**:
   他の gen backend は promotion 時の old growth で発火する設計だが
   immix_gen だけ nursery alloc 含む全 alloc で発火していた。 local list_alloc
   で immix_gen=42 minor/21 major vs copy_gen=52/0 と発火頻度が違っていた。
   `bytes_since_major` を削除し `old_alloc_since_major` に統一、 promotion
   サイト (forward_obj + large_alloc) で increment するように。 binary_trees で
   major count 21 → 2。

3. **mark_gen / mark_gen_inc の young threshold が 4 MiB**: 他 gen は
   16 MiB が nominal。 local list_alloc で mark_gen=133 minor vs copy_gen=52、
   mark_bitmap_gen=33 と policy 不一致。 4 → 16 MiB に統一。 mark_gen の
   list_alloc minor count 133 → 33。

4. **charging model が backend ごとに違う**: mark_bitmap_gen は payload
   bytes を threshold に対して数えていたが、 copy_gen / mark_bump_gen は
   header + aligned payload (nursery occupancy) を見ていた。 nominal 16 MiB
   でも実効 nursery budget が違う。 全 gen backend で
   `sizeof(GCHeader) + ALIGN8(payload_size)` (= alloc-bytes) に統一。

5. **mark_gen_inc の inc_step が GC timer の外**: 主要 mark work は
   allocator-path 上の inc_step にあるが、 `aro_gc_time_begin/_end` に
   囲まれていなかった。 結果 `gc_seconds` と `max_pause_ms` が他 backend より
   小さく出る。 inc_step / inc_start_major / minor_gc / major_gc 全て phase
   timer (`mark_seconds` / `reclaim_seconds`) で囲んだ。 binary_trees で
   `mark_seconds` が 0 → 0.37 に正常化。

6. **bench/run.rb のパースが壊れていた**: 旧 main.c の出力は
   `gc_seconds=X gc_pct=Y` 連続だったが、 iter 33 で間に mark_seconds /
   reclaim_seconds が挟まった。 regex がマッチせず gc_s / gc% 列が常に 0。
   追加で「各 repeat の stats を上書き、 time だけ sort」 で best time の
   stats が無関係 run の値だった。 修正: 各 run の (time + stats) を struct で
   保持、 picked run の stats を表示。 median/best/trimmed を選べる
   `--choose` オプションも追加。

7. **baruby vs baruby_precise の build flags が不一致**: baruby_precise は
   `-flto=auto`、 baruby は無し。 user 指示「baruby 側を変更するのがいいと
   思うね」 に従い baruby/Makefile に `-flto=auto` を追加して align。

### Matrix runner (iter 35 新規)
`bench/matrix.rb`: backend ごと rebuild → `strings` で `baruby_gc=<name>`
stamp 検証 → `oracle.json` で result checksum → CSV / JSON / Markdown 出力。
Iter 32 で発覚した「Makefile が rebuild されず別 backend のバイナリを測る」
事故を再発防止。

### ASTRO_DEBUG default 変更
context.h の default が 1 で、 perf 計測も `-DASTRO_DEBUG=1` 込みだった。
binary_trees で測定差は <1% (assertion が constant-fold される) だが、
**原則として perf build に assert overhead を含めるべきでない**。 Makefile の
`ASTRO_DEBUG ?=` を `?= 0` に変更し、 dev は `make ASTRO_DEBUG=1` で
opt-in。

### 過去 iter 数値の扱い
- iter 31〜34 の表は (a) Makefile bug (iter 32 で修正)、 (b) charging
  inconsistency、 (c) inc_step uncounted などで semantically 不連続。
- 「iter X → iter Y で X% 改善」 系の主張は **iter 35 fair contract 前後で
  混ぜると無効**。 履歴用 done.md の数値は保存するが、 perf.md の正本は
  iter 35 fair 数値のみ。
- README の「全 11 bench で勝つ、 geomean -22%」 主張は取り下げ
  (build flag 不一致 + Makefile bug の二重欠陥)。

## 2026-05-18 (34) — mark_bitmap_gen の adaptive minor threshold

binary_trees で mark_bitmap_gen が 14 minor + 2 major (gc_seconds=0.48,
gc_pct=38%、 全体 wall 1.43s で worst-of-all) と過剰に minor を発火して
いた。 原因は固定 `MINOR_THRESHOLD = 16 MiB` で、 binary_trees のように
**生存率が極端に高い** workload では毎 minor が「young 全部を促進」 する
だけで no garbage を回収しない。

修正:
- `MINOR_THRESHOLD` を static `minor_threshold` (initial 16 MiB) に変更
- 各 minor 終了時に survival ratio を計算:
  - survival > 75% → threshold × 2 (cap 256 MiB)
  - survival < 25% → threshold / 2 (floor 16 MiB)

副次的に `size_class_shift[]` table を追加して `locate()` の div を shift
に置換 (class 32, 64, ..., 4096 で pow2 のもの)。 LTO で constant-prop されて
いれば測定不変、 そうでなくても fast path 化。

perf 改善 (3-run best、 iter 33 → iter 34):
- binary_trees: 1.43 → **1.13** (-21%)
- gc_count: 16 → 4 (生存率高い workload で minor が指数的に sparse 化)
- gc_seconds: 0.48 → 0.21 (-57%)

stress mode + 13 bench で結果 checksum 一致。

ただし iter 35 で user から **fairness 上の本質的な問題** を 7 件指摘され、
mark_bitmap_gen の threshold だけ「16 MiB の中身」 が他 backend と違う
（payload-byte counting vs occupancy-byte counting）など、 単独最適化を
進めても collector 比較として fair でないことが判明。 iter 35 で
comparison contract 全体の整理を行う。

## 2026-05-18 (33) — GC phase 計測 + mark_gen 系の hash_chain 大幅高速化

### Phase timing
各 collect 関数を `aro_gc_phase_begin/end()` で挟んで mark phase と reclaim
phase の時間を別計上。 `BARUBY_GC_STATS=1` で `mark_seconds=` /
`reclaim_seconds=` を出力。 詳細は perf.md §2.5。

phase semantics:
- mark&sweep: mark = trace, reclaim = sweep
- mark&compact: mark = trace, reclaim = forward + update + slide
- copy (Cheney): trace と relocate 交錯のため mark=0, 全部 reclaim 計上

### mark_gen / mark_gen_inc の hash_chain regression 解消
iter 32 の真の perf 数値で mark_gen が hash_chain で 2.02s (mark の 1.24s
の **1.6×**)、 mark_gen_inc も 1.99s で同じ症状と判明。 perf record で
GC 時間は 0.008s しかなく、 mutator 側の cache miss 率が 35.95% vs mark の
17.85% と倍増していた。

**原因**: mark_gen の GCHeader は `young_next` (8 B) + flags(1 B) + pad + size
= **16 B**。 BaArray (24 B payload) を入れると合計 40 B → slab class 64
(slot 64 B、 24 B waste)。 一方 mark は header 8 B で BaArray 32 B → slab
class 32 (slot 32 B、 waste 0)。 結果、 hash_chain の 525K 個 BaArray で
mark_gen は **16.8 MB → 33.6 MB の heap footprint** に膨らみ LLC を抜ける。

**修正**: `young_next` per-header field を削除し、 external な
`young_objs[]` 配列 (`static GCHeader **young_objs`) に push して管理。
header は 16 → **8 B**、 BaArray は class 32 にぴったり収まる。

```c
// before: per-header linked list
typedef struct GCHeader {
    struct GCHeader *young_next;   // 8 B
    uint8_t flags;  uint8_t _pad[3];  uint32_t size;
} GCHeader;  // 16 B

// after: external array
typedef struct GCHeader {
    uint8_t flags;  uint8_t _pad[3];  uint32_t size;
} GCHeader;  // 8 B
static GCHeader **young_objs;  // pushed on each alloc
```

perf 改善 (iter 32 → iter 33):
- `mark_gen` hash_chain: 2.02 → **1.36 s** (-33%)
- `mark_gen_inc` hash_chain: 1.99 → **1.35 s** (-32%)
- 他の bench は ±5% の noise 範囲

stress mode (BARUBY_GC_STRESS=1) で nqueens / binary_trees / cons_list 全 PASS。

cache locality 観点での副次効果:
- minor GC の sweep_young が linked-list 走査 (pointer-chasing) → 配列の
  sequential scan に変わり、 prefetch が効くようになった
- 24 B BaArray の hot allocate-and-discard ループで footprint 半減

## 2026-05-18 (32) — Makefile 再ビルドバグ修正 + iter 31 perf 数値の再計測

### Makefile bug
`make GC=foo` で GC 切替を行ったとき、 `.c` ファイルの mtime が古いまま
だと **再 link されない**。 `*.c` glob 依存はすべての .c の mtime しか
見ず、 `-DBARUBY_GC=N` の値変化や `$(GC_SRC)` 選択変化を mtime に反映
できないため、 既存バイナリの GC backend が前回のままになっていた。

判明経緯: iter 31 packing 後の perf 数値が「全 backend で 0.86-0.91s に
収束、 spread 6%」 という異常な tight さ。 `bump` (no-GC) ですら 0.91s と
iter 30 (0.57s) より遅い。 perf record で `forward_payload_nursery` が
hot path に出てきたが bump には GC 経路が無いはずなので矛盾。 `strings
baruby_precise | grep baruby_gc=` で確認したら全部 `immix_gen` だった。

修正:
- `.built_gc` という marker file を Makefile に追加。 内容は現在の `GC`
  変数値。 `make` 起動時に `$(shell test -f .built_gc && cat .built_gc)`
  で前回値を読み、 `$(GC)` と異なれば marker を touch (echo redirect)。
  `baruby_precise` ターゲットの dep に `.built_gc` を加えたので mtime
  更新でリンクが走る。

### 真の iter 31 perf 数値

再計測後、 packing 効果は ↑ docs/perf.md §2 の通り **alloc-heavy bench で
顕著**:
- `mark` hash_chain: 2.13 → **1.24** (-42%)
- `mark` binary_trees: 1.07 → **0.89** (-17%)
- `mark` string_concat: 0.86 → **0.70** (-19%)
- `mark_bump_gen` string_concat: 0.53 → **0.41** (-23%)
- `copy` fib_pair: 0.87 → **0.72** (-17%)
- `bump` binary_trees: 0.57 → **0.45** (-21%) — Makefile bug 修正前の
  iter 29/30 数値 (0.55-0.57) も別 backend の数字だった可能性

### Stress mode sweep
fix 後、 12 GC backend × stress test (200 iter × 50 cell cons list、 stress
mode で全 alloc が GC を起こす) で全 PASS。 packing が introduce した
correctness regression なし。

### 含意
- 過去 iter (29/30/31 第一報) の perf 数値表は GC backend が混在した状態の
  測定。 packing 前後比較は無効。 iter 31 真値が正しい現在値。
- 「Makefile が *.c に依存している」 という pattern は GC switching を
  CLI 変数でやる setup で trap になる。 同様の setup を他 sample に持ち込む
  ときは marker file 戦略を踏襲する。

## 2026-05-18 (31) — GCHeader を flags byte で全 backend に compact packing

`kind` (uint32) は KIND_OBJ_ARRAY / OBJ_STRING / PAYLOAD_VAL / PAYLOAD_BYTE
/ FREE の **5 種類しかない** → 3 bit で足りる。 `marked` / `old` / `dirty` の
各 bool も 1 bit ずつ。 まとめて single `uint8_t flags` に packing する
ことで全 backend の GCHeader を大幅に縮小:

| Backend | iter 30 (B) | iter 31 (B) | 削減 |
|---|---:|---:|---:|
| `mark` | 16 | **8** | -50% |
| `mark_gen` / `mark_gen_inc` | 24 | **16** | -33% |
| `copy` / `copy_gen` / `copy_gen_inc` | 24 | **16** | -33% |
| `mark_compact` / `mark_compact_gen` / `mark_bump_gen` | 24 | **16** | -33% |
| `immix` / `immix_gen` | 16 | **8** | -50% |
| `bump` / `mark_bitmap_gen` / `none` | 8 | 8 | 据え置き (元から flag bit 不要) |

実装パターン: 各 backend が独自に flags byte の bit layout を決め、
`HDR_KIND(h)` / `HDR_MARKED(h)` / `HDR_OLD(h)` / `HDR_DIRTY(h)` (および
`SET_` / `CLR_` 変種) のマクロでアクセス。 backend ごとに必要な bit が
違うので bit position は backend ごとに異なる (例: mark_gen は marked=bit3
old=bit4 dirty=bit5、 copy_gen は marked 不要なので old=bit3 dirty=bit4)。

perf 改善 (3-run best、 iter 30 比、 主な変化):
- `mark` hash_chain: 2.13 → **1.19 s** (-44%) — slab class density 効果が
  大きい。 mark の 16 B header 時は BaArray (24 B + 16 B = 40 B) が
  class 48 に逃げて waste、 8 B header になり class 32 (32 B) にぴったり
  収まって waste 0
- `mark_gen` binary_trees: 1.10 → **0.90 s** (-18%)
- `mark` / `mark_gen` / `mark_gen_inc` の hash_chain は **全部 iter 30 の
  半分以下** (2.13/2.42/1.72 → 1.19/1.23/1.16)
- 他の backend (`copy*` / `mark_compact*` / `immix*`) は packing 後でも
  差は数 % 〜±10% の範囲。 元から dense なので header 縮小の伸び代が小さい

特筆事項:
- **iter 31 後の backend 間 spread が極めて小さくなった**: binary_trees で
  iter 30: 0.57 - 1.52 (2.7×) → iter 31: 0.86 - 0.91 (1.06×)。 全 backend が
  GCHeader sizing の最適化により hot path で同じ程度まで圧縮された
- `immix_gen` で `h->flags = H_OLD` パターンが kind を上書きする bug を
  発見し、 `(kind | H_OLD)` 形に書き直して修正
- sed word-boundary の罠 (`h->old` が `hh->old` や `newh->old` を巻き込む)
  に 3-4 回引っかかった。 順序を「長い prefix から」 にする必要

## 2026-05-18 (30) — slab_alloc per-alloc redundant init 削除

mark family (`mark` / `mark_gen` / `mark_gen_inc` / `mark_bitmap_gen`) の
slab_alloc が、 `h->marked = false` / `h->old = false` / `h->dirty = false`
を per-alloc に書いていた。 但し sweep / free_slot が free 時に同じ bit を
0 にしておく invariant を立てれば、 slab_alloc は重複書きを省ける。

修正:
- `gc_mark.c`: `h->marked = false` を slab_alloc / large_alloc / new_page
  から削除 (sweep が unmarked のみ free + mmap zero で invariant 成立)
- `gc_mark_gen.c` / `gc_mark_gen_inc.c`: `free_slot` で marked/old/dirty を
  クリアするように変更 → slab_alloc / large_alloc / new_page の冗長な
  reset を削除
- `gc_mark_bitmap_gen.c`: per-page bitmap 路で **3 個の bm_clr (locate +
  bit op を含む)** を slab_alloc から削除。 free 時に bitmap bit が既に
  0 である invariant で OK。

perf 改善 (3-run best、 iter 29 比):
- mark_bitmap_gen が顕著: binary_trees 1.63 → 1.50 (-8%)、 string_concat
  0.98 → 0.84 (-14%)、 substr_churn 1.27 → 1.14 (-10%)、 等 alloc-heavy
  bench で **-5〜-14%** 改善。 per-alloc の locate() + 3 bitmap op が
  消えた効果。
- mark_gen / _inc は header byte write 数個減で **-2〜-8%** 改善。
- mark は 1 byte write 減で大した差なし。

mark_bump_gen は bump nursery (slab でない) なので対象外。 immix family は
mark_epoch=0 が必須 (sweep が GCHeader を触らないので stale 値の可能性)、
copy / mark_compact 系も bump alloc で previous content が任意、 共に
skip 不可。

## 2026-05-18 (29) — unified 16 MiB adaptive threshold + 全 backend fairness (完)

user 指摘「copy / mark_compact が 64 GiB virtual で region 容量基準でしか
GC しない = 実質 bump 同然で fair じゃない」 への対応 + 全 backend で
threshold policy 統一。

統一ポリシー (iter 29 fairness 最終形):
- 全 GC 系 backend で `bytes_since_gc > threshold` で発火
- threshold = max(16 MiB, 2 × live_post_collect) で adaptive
- MIN を全 backend で 16 MiB に統一 (以前は 4 MiB / 64 MiB の不揃い)

修正:
- `gc_copy.c` / `gc_mark_compact.c`: adaptive threshold を新規追加
  - copy binary_trees: 0 GC → 3 GC、 0.62s → 0.90s (公平化)
  - mark_compact binary_trees: 0 → 3 major、 0.65s → 0.96s
- `gc_copy_gen.c` / `gc_copy_gen_inc.c` / `gc_mark_compact_gen.c`:
  MAJOR threshold 新規追加 (それ以前は tenured 容量基準 = 実質発火せず)
- `gc_mark.c` / `gc_immix.c`: MIN 4 MiB → 16 MiB
- `gc_mark_gen.c` / `gc_mark_gen_inc.c` / `gc_mark_bump_gen.c` /
  `gc_mark_bitmap_gen.c` / `gc_immix_gen.c`: MIN 64 MiB → 16 MiB

加えて: `mark_bitmap` → `mark_bitmap_gen` リネーム (gen 系の naming 規則
揃え)。

`docs/gc_runtime.md` 大幅更新:
- §4 各 backend に「Heap 拡張」「GC trigger」「Minor/Major trigger」 明記
- §6 を「ヒープ管理 — サイズ戦略と GC 発火条件」 に書き換え:
  - §6.1 仮想ヒープ予約の意味 (64 GiB は ≠ 上限まで GC しない)
  - §6.2 adaptive threshold policy (MIN=16 MiB、 factor=2×live)
  - §6.3 backend ごとの拡張単位 (slab 16 KiB page / Immix 32 KiB block /
    region 系の lazy commit 4 KiB page)
  - §6.4 fairness 設定の対比表 + iter 29 変更履歴

全 14 backend × 14 bench で正解返却 (fail=0)。 perf.md §2 は新数値で
refresh 予定 (sweep 後)。

## 2026-05-18 (29) — fairness 修正 + gc_runtime.md 入門書 (続)

(29) 後半:

**fairness 修正**: gen 系の MAJOR_THRESHOLD_MIN を audit して、 sticky
非moving gen family 内で揃えた:
- mark_gen / mark_gen_inc / mark_bump_gen: 元から 64 MiB ✓
- immix_gen: 4 MiB → **64 MiB** (16× 違いがあった)
- mark_bitmap_gen: 4 MiB → **64 MiB** + MINOR_THRESHOLD 4 → 16 MiB

修正後 fair sweep で perf.md §2 を 15 列 (14 backend + libgc) × 14 bench に
全面 refresh。 mark_bitmap_gen / immix_gen は数値悪化方向だが、 同 family 内で
同 cadence 比較可能に。

**`docs/gc_runtime.md` 新規**: user 要望「runtime.md だけだと heap 管理が
分かりにくい、 GC 知らない人向けに独立した方が良い」 で作成:
- §0 GC とは何か + 用語ミニ辞典
- §1 baruby_precise 共通基盤 (sp[] root、 LSB-tagged VALUE、 GCHeader、 WB)
- §2 ヒープ管理パターン 4 種 (bump / slab / semispace / Immix) ASCII 図入り
- §3 14 backend 早見表 (パターン × Gen × Moving × Header size × 強み弱み)
- §4 各 backend のアルゴリズム解説 1 つずつ
- §5 設計空間の俯瞰 (nursery × tenured × compact 3 軸)
- §6 Fairness — 揃えてある設定 (heap size / nursery / major threshold)
- §7 workload パターン別おすすめ backend

**今 iter の commits**:
- 860e992 minor_gc noinline (3 backend の alloc fast path 改善)
- f95469f mark_bitmap_gen 14th backend
- 3441e8b runtime.md #14 entry
- 7e68417 fairness 修正 (threshold 揃え)
- 647ddfd perf.md §6/§7 sync
- 2d95001 gc_runtime.md 入門書 (新規)
- 4e41f5d perf.md §2 fair sweep refresh

## 2026-05-18 (29) — minor_gc noinline 化で 3 backend の alloc fast path 改善 + `mark_bitmap_gen` 追加

**前半: minor_gc noinline**

(28) で copy_gen vs mark_compact_gen の perf 差を分析した際、 LTO の inlining
判断の偶然差が原因とわかった (詳細 perf.md §5)。 copy_gen / copy_gen_inc /
mark_bump_gen の 3 つは major_gc が比較的小さく、 LTO が minor_gc を
nursery_bump に inline → nursery_bump 1100 B 級に膨張 → aro_gc_alloc に
fast path inline 不成立 → alloc 毎に PLT call が残る、 という構図。

minor_gc に `__attribute__((noinline))` を付けて cold path として別関数
維持することで nursery_bump スリム化 → aro_gc_alloc に fast path 完全
inline。 サイズ変化:
- copy_gen:      aro_gc_alloc 168 → 522 B、 nursery_bump 1118 → 406 B
- copy_gen_inc:  同上
- mark_bump_gen: aro_gc_alloc 168 → 591 B、 nursery_bump 消失 (完全 inline)

perf 改善 (主な勝ち bench):
- copy_gen list_alloc -6%、 gc_combined -3%、 substr_churn -2%
- copy_gen_inc fib_pair -7%、 gc_combined -9%
- mark_bump_gen substr_churn -11%、 string_concat -5%、 gc_combined -6%

audit で他 backend に同じ問題なしを確認。 全 13 backend × 14 bench で
regression 無し。

**後半: 14 つ目の backend `mark_bitmap_gen`**

user「semantics が同じなら sticky M&S を別実装する意味は薄い、 bitmap だけ
で良い」 という指摘を受けて追加。 sticky mark&sweep を per-page bitmap で
実装した variant:

- GCHeader 8 B (kind + size のみ) — 元 mark_gen の 24 B から大幅削減
- mark / old / dirty bit は per-page bitmap (64 B × 3 = 192 B/page)
- page は 16 KiB **aligned** (over-mmap して trim) → `(ptr & ~0x3fff)` で
  O(1) で page base 取得
- young_next linked list 撤廃 — minor sweep は全 page を walk して
  old_bm 0 の slot を judge (O(heap)、 mark_gen の O(young) に対し)

**密度の副次効果**: 8 B header で **BaArray (24 B payload) が class 32 にぴったり収まる**。 旧 mark_gen では BaArray は class 64 (24 B header + 24 B
payload = 48 B → 64 B class) で 40% waste していたのが消える。

性能 (14 bench 3-run best):
- **hash_chain 1.67 (vs mark 2.50、 -33%!)** — 密度向上の効果が大きい
- string_concat 0.87 (vs mark 0.74、 mark_gen 0.89 と同水準)
- nqueens / cons_list / fannkuch / list_alloc 等は mark_gen と互角
- **binary_trees 2.02 (vs mark 1.00、 +100%!)** — minor sweep O(heap) +
  bitmap op overhead で long-live tree workload で苦戦

設計教訓: bitmap 化は「header 縮小 + 密度向上」 で alloc-heavy workload に
prefer されるが、 「per-mark bitmap op」 が hot mark phase で per-object
overhead を生む。 future work: per-page "all old" flag で minor sweep を
skip、 mark fast path 用の cached page pointer 等。

**アリア sing バグでハマった点**: GCHeader と FreeSlot が同じ 8 byte を共有
する初版は strict aliasing で GCC が write を reorder → freelist の最初の
要素が `(kind=3, size=128)` で破壊された (gdb で確認: 0x0000008000000003)。
gc_mark.c と同じく **FreeSlot を payload 領域 (h+1) に配置** で解決。

## 2026-05-18 (28) — sieve macro bench 追加 + MADV_DONTNEED 撤回 + perf.md refresh

(27) 系で入れた MADV_DONTNEED が perf regression を生んでいた:
- immix string_concat: 0.70 → 1.47 (2× 遅)
- immix list_alloc: 1.03 → 1.41
- copy hash_chain: 1.45 → 1.85

原因: alloc-heavy workload で DONTNEED した page を即再利用 → 毎 cycle 全 page
page-fault → 物理メモリ節約より遥かに高くつく。 撤回。 64 GiB virtual
reservation 自体は維持 (program-limit cap 撤廃の効果は保持)、 物理は peak
working set 分使う = OS pressure で必要なら自動 swap。

**新 bench `sieve.ba.rb`**: Sieve of Eratosthenes (N = 10^7、 primes = 664579)。
1 つの long-lived 80 MB boolean 配列 + 1 つの medium result 配列の組合せ、
scattered write (`j += i` で page-spread cross-off) が cache locality を負荷。
既存 14 bench にない「単一 huge object」 系の workload を追加。 ベスト 1.36 s
(`none`)、 GC-less が勝つ = mutator-dominated。 baruby (libgc) にも port。

**perf.md §2 全面 refresh** (14 bench × 14 構成、 3-run best):
- 勝者分布で immix_gen が初の bench 最速を 3 件獲得 (gc_combined / hash_chain
  / list_alloc tied)
- string_concat 最速 tie: immix / immix_gen の 0.57

## 2026-05-18 (27c) — VALUE stack の固定 800 KB cap → 8 GiB virtual (lazy-paged)

(27) 系の continuation。 `create_context(10000, 2000)` → calloc(100k slots,
8B) = 800 KB の VALUE stack は「recursion depth × per-frame locals」 の
program limit だった (深いプログラムでは crash する)。

修正: stack を `mmap(8 GiB, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE)` に
変更。 virtual に 1 B (10^9) slots 確保、 物理 page は触ったとき commit。
GC scan は `c->env..c->sp` のままなので touched 範囲のみ。 untouched は
zero (VAL_FALSE) で安全。 `frames` 引数は historical で無視。

全 13 backend × 5 bench で regression 無し。

## 2026-05-18 (27b) — toplevel sp の hardcode "64" 撤廃

(27) と同じ「program-limiting な固定長を撤廃」 方針の続き。
`main.c::create_context` の `c->sp = c->env + 64` は「toplevel locals が
64 を超えると sp が locals 領域に食い込んで壊れる」 という program limit。
todo.md の P0 として残っていた。

修正:
- `baruby_parse.c::PM_PROGRAM_NODE` で `tc->frame->max_cnt` (parser 計算
  済の toplevel locals 数) を grobal `aro_toplevel_locals_cnt` に書き出す
- `main()` で PARSE 直後に `c->sp = c->env + aro_toplevel_locals_cnt`
- `create_context` の sp 初期化を `c->env` (0 offset) に変更
  (PARSE 前に GC を発火することはない)

検証: 100 toplevel locals の test program (`a01..a100 = ...; p a01 + ...`)
で正常実行。 全 13 backend × 13 bench で regression 無し。

## 2026-05-18 (27) — プログラム制限の固定長を撤廃 (region cap → 64 GiB virtual)

user 要望「まともな処理系にするために、固定長の部分をまともにしようか / ページ
サイズとかは固定でいいけど、プログラムに制限を入れる固定長はやめて」。

それまで各 backend は REGION_BYTES / ARENA_BYTES / TENURED_BYTES として
512 MiB - 4 GiB の固定 cap を持ち、 program の live data がそれを超えると
OOM abort していた (← gc_combined で immix_gen が踏んだのが直近)。

**対応**: 「huge virtual reservation + lazy commit」 (V8 / ZGC / G1 等の
標準 modern GC pattern) を採用:

- `gc.h` に共通定数 `ARO_GC_REGION_VIRT_BYTES` = **64 GiB** を導入。 全
  region 系 backend がこれを参照。
- 全 mmap 呼出に `MAP_NORESERVE` を付加 (overcommit_memory=2 環境でも
  失敗しないため)。
- per-cycle tuning knob である `NURSERY_BYTES` (16 MiB) はそのまま
  (これは program limit でなく minor 頻度の tuning)。
- per-chunk size (page 16 KiB / block 32 KiB / line 128 B) はそのまま
  (user の指示「ページサイズとかは固定でいい」 通り)。

**Immix family** (gc_immix.c / gc_immix_gen.c) の追加対応:
- `block_meta` 配列も 64 GiB / 32 KiB × 257 B ≈ 514 MB 仮想に巨大化する
  ので、 これも lazy-paged mmap に変更 (旧 `calloc`)。
- N_BLOCKS は 2M に膨れたが、 sweep が full N_BLOCKS を walk すると
  0.5 s/cycle 浪費するので **`max_touched_block` 変数で実際に使った
  block index の上限を track**、 sweep / mark-clear ループはこの範囲のみ
  scan する。 hash 表や linked list を持たない一直線 cursor 方式。
- `find_hole` が touched 範囲で hole 見つからない時は次の virtual block を
  touch して "1 block 一括 hole" として返す路を追加。 動的成長を実現。
- sweep で BLK_FREE 化した block は `madvise(MADV_DONTNEED)` で物理 page
  を OS に返却 (heap_bytes ≈ live_bytes を維持)。

**gc_copy.c**: non-stress mode でも from-space の用済み範囲 (top_pre_collect
まで) を `madvise(DONTNEED)` するように。 これがないと peak physical =
2 × live、 これで peak ≈ live。

**影響範囲**:
- gc.h (定数追加)
- gc_bump.c (4 GiB → 64 GiB)
- gc_copy.c (512 MiB → 64 GiB、 madvise 追加)
- gc_copy_gen.c (512 MiB → 64 GiB)
- gc_copy_gen_inc.c (同)
- gc_mark_compact.c (1 GiB → 64 GiB)
- gc_mark_compact_gen.c (512 MiB → 64 GiB)
- gc_mark_bump_gen.c (1 GiB → 64 GiB)
- gc_immix.c (512 MiB → 64 GiB virtual + lazy block_meta + max_touched + madvise)
- gc_immix_gen.c (同上 + nursery NORESERVE)

mark / mark_gen / mark_gen_inc は元から per-page slab で cap 無し、 無変更。

**テスト**: 全 13 backend × 13 bench で正解 (`make GC=X` × 13 を sweep)。

## 2026-05-18 (26) — 13 つ目の backend `immix_gen` (generational Immix)

user 要望「immix generational が欲しいかなあ」 で追加。 (25) の `immix` を
ベースに nursery + remset + minor を載せた generational 変種。

**構成**:
- nursery: 16 MiB bump region
- tenured: 512 MiB Immix arena (block 32 KiB / line 128 B)
- minor: nursery 生存者を `hole_alloc_header` で tenured hole に Cheney-copy promote
- major: leading minor → line_marks クリア → mark → sweep (immix と同じ)
- WB: H_OLD / H_DIRTY bit on GCHeader.flags、 remset push

**Forwarding 方式**: `oldh->kind = KIND_FREE` + 古い payload の先頭 8 byte に
新 ptr を書く (payload は dead-from-source なので破壊 OK)。 GCHeader 16 B
維持。

**ハマり所**: gc_combined で「tenured arena OOM during promotion」 が
発生。 原因: `items[65536]` (524 KB) が nursery に入って (旧
`total > NURSERY_BYTES/2` = 8 MiB の pretenure threshold) 、 promotion 時
に Immix の単一 block hole (32 KiB) に収まらず find_hole 失敗。 fix:
pretenure threshold を `MEDIUM_MAX` (16 KiB) に下げる。 これで「nursery に
入った時点で必ず単一-block hole に promote 可能」 を保証。

**性能** (13 bench 3-run best、 immix non-gen との対比):
- gc_combined 1.11 → **1.01** (-9%)
- cons_list 0.96 → 0.88 (-8%)
- hash_chain 1.49 → 1.38 (-7%)
- list_alloc 1.03 → 0.96 (-7%)
- fib_pair 1.10 → 1.02 (-7%)
- string_concat 0.70 → 0.67 (-4%)
- **binary_trees 0.68 → 1.15 regression** — long-lived tree workload で
  Cheney copy が逆効果 (古典的な世代別 GC が苦手な pattern)。 long-lived
  支配なら immix non-gen を使う運用。

**docs**: README.md / gc.h / runtime.md §5.10 #13 + §5.11 design table /
perf.md §2 (14 列に拡張)。

## 2026-05-18 (25) — 12 つ目の backend `immix` (mark-region, no evac v1)

「precise なら immix とかもいけるんじゃない？」 (user 要望) で着手。 v1 は
non-moving (no evacuation)、 hole-based bump alloc + line-mark sweep。

**設計**:
- 512 MiB arena を 32 KiB BLOCK × 16384 個、 各 block を 128 B LINE × 256 個
- per-block `line_marks[256]` (byte-wide) — mark phase で span するライン全てに set
- "hole" = 連続 unmarked line の run、 これ内で bump alloc
- `find_hole(n_lines)` で次の hole を block_cursor / line_cursor から resume
- large object (> 16 KiB) は別 mmap (gc_mark.c 流儀)
- **mark epoch counter** で sweep 後の bit クリア walk を省略 — `cur_epoch`
  を tick するだけで全 prior mark が自動 invalidate

**ハマった点**:
- 初期化で `cur_ptr/cur_end` をセットしつつ `line_cursor` 更新を忘れて
  block 0 を 2 回 alloc 候補にしてしまい live data overwrite → 「no size for
  non-array/string」 で crash。 init を `cur_ptr=NULL/cur_end=NULL` にして
  最初の alloc が必ず `find_hole` を通るように修正
- `find_hole` が毎回 i=0 から scan していて同じ hole を返すバグ → line_cursor
  を追加して resume

**性能** (全 13 bench 3-run best):
- binary_trees 0.68 s (`copy` 0.53 / `bump` 0.49 と比べると block metadata
  の overhead が見える、 `mark_compact` 0.60 と互角)
- string_concat 0.70 s (`mark_bump_gen` 0.51 より遅いが `mark` 0.68 と互角)
- substr_churn 0.91 s — `copy_gen` 0.88 と肉薄、 全 backend で 4 位タイ
- mid-pack overall。 v1 制限の no evacuation で long-running fragmentation が出る
  はずだが、 短時間 bench では問題なし。

**docs**: README.md / gc.h コメント / runtime.md §5.10 (#12 entry) +
§5.11 (設計空間 table) / perf.md §2 (13 列に拡張)。

## 2026-05-18 (24) — fannkuch macro bench、 `aro_gc_` rename、 perf.md §2 統合

3 件まとめ:

**1. `bench/fannkuch.ba.rb` 追加** — CLBG fannkuch-redux マクロベンチ。
N=9 で全 362880 順列を列挙、 各順列の「prefix flip」 最大数 = 30 を返す。
canonical な rotation-of-prefix enumeration を baruby に port (`break`
非サポートなので while/flag で書き換え)。 全 11 backend + libgc で正解
返却、 ベスト 0.66 s (`mark_bump_gen`)、 libgc 0.71 s。 ただし integer-heavy で
alloc/CPU 比率が低く 12 構成中の spread が 15% と GC 戦略の差は小さい
(macro だが mutator-bound)。 baruby (libgc) にも port して fair 比較を
追加。

**2. `baruby_gc_` → `aro_gc_` rename** — ASTro 標準 GC interface 化と
将来の `root/runtime/gc/` 移動を見据えた prefix 変更。 影響範囲:
- 全 11 `gc_*.c` (約 350 occurrences)
- `gc.h` / `node.def` / `main.c` / `node.c`
- 型: `BarubyGCKind` → `AroGcKind`、 `BarubyGCStats` → `AroGcStats`
- env var (`BARUBY_GC_STATS`, `BARUBY_GC_STRESS`) は端末 UX として保持

全 11 backend で nqueens 結果 2680 を確認、 fannkuch / string_concat /
binary_trees も同じ。

**3. perf.md §2 を libgc 統合 12 列に再構成** — 旧 §2 (11 backend) と
旧 §3 (libgc fair 比較) を一体化、 libgc を 12 番目の列として並べた。
利点:
- mark や copy_gen と libgc を直接横並びに比較できる (例: string_concat
  の libgc 0.88 s vs mark_bump_gen 0.51 s)
- 13 bench × 12 構成の全 panel を一つの table で把握可能
- 旧 §3 で別表だった「最速 backend vs libgc」 は §2 の `**` 印で表現
  → 13 bench 中 12 bench で baruby_precise が勝つ (nqueens のみ tie)

新表で fannkuch 列が加わって winner 分布が変化:
- `mark_bump_gen` 5 bench (fannkuch / fib_pair / life / list_sort /
  string_concat) で最速、 cheapest-alloc 路の強み
- `mark_compact_gen` 3 (cons_list / gc_combined / list_alloc)
- `bump` 1 (binary_trees)、 `copy_gen` 1 (substr_churn)
- `libgc` 1 tied (nqueens、 `none` と同値で mutator 支配を示唆)

## 2026-05-17 (23) — `gc_mark_compact_gen` の leading-minor overflow バグ修正

`BARUBY_GC_STRESS=1` で 11 backend を sweep して見つけた correctness バグ:

```
baruby_precise: gc_mark_compact_gen.c:261: forward_obj:
Assertion `to_top + total <= tenured_end' failed.
```

**原因**: `major_gc` の入口で無条件に leading `minor_gc` を呼ぶが、
nursery を tenured に折り畳むため `to_top = tenured_top` から bump する。
tenured が `tenured_end` 近くまで詰まった状態で major が走ると、 leading
minor が tenured を溢れさせて assertion 発火。 ASTRO_DEBUG=0 ビルドでは
assert が消えるので memory corruption になる real な correctness bug。

**修正**: `defer_fold` flag を導入。
- nursery が `tenured_end` を溢れる場合は leading minor を skip
- mark+compact を先に走らせて tenured の dead を回収
- compact 後の slide で空いた領域に nursery を fold する trailing minor
  を走らせる
- `fwd_payload_compact` は in_nursery pointer を no-op で素通し
  (defer_fold 中の tenured-to-nursery 参照は trailing minor で fwd)
- trailing minor は remset 不在 (compact で dirty bit クリア済) のため
  全 tenured を walk して nursery 参照を拾う
- 折り畳み前に nursery survivors の `marked` を明示的にクリア
  (major mark phase で set 済 → memcpy で tenured に伝播するのを防止)
- `forward_obj` 内の `ASTRO_ASSERT` を「clean abort + 内訳 print」 に
  差し替え (release build での silent corruption 防止)

stress test (cons_list / interp_calc / list_alloc / nqueens / string_concat)
で 5/5 PASS。 通常ベンチの perf 影響なし (defer_fold path に入らない)。

## 2026-05-17 (20) — `gc_mark_gen` / `gc_mark_gen_inc` も slab/page allocator に

(18) で `gc_mark` を slab 化したのに合わせて generational 兄弟 2 つも
port:
- 16 KiB page を 9 size class に分ける (mark.c と同じ pool 構造)
- generation tracking: per-slot に `old` bit、 young は single-linked list
  (young_next in header)。 old 側は page 走査で済むのでリスト不要
- minor: walk young_head、 marked → set old=true、 unmarked → freelist 返却
- major: mark 全 generation → sweep_young → sweep_old_pages

GCHeader は 24 bytes (旧 32 から -8、 mark_bump_gen と同サイズ)。

性能改善 (旧 vs 新、 3-run 中央値):

| Bench | mark\_gen 旧 → 新 | mark\_gen\_inc 旧 → 新 |
|---|---:|---:|
| binary_trees | 1.28 → 1.11 (-13%) | 1.44 → 1.16 (-19%) |
| string_concat | 1.47 → **0.78 (-47%)** | 1.51 → **0.83 (-47%)** |
| fib_pair | 1.43 → 1.06 (-26%) | 1.47 → 1.12 (-35%) |
| substr_churn | 1.46 → 1.11 (-24%) | 1.58 → 1.11 (-30%) |
| cons_list | 1.09 → 0.96 (-12%) | 1.23 → 1.04 (-24%) |
| list_alloc | 1.19 → 0.97 (-18%) | 1.26 → 1.10 (-18%) |
| gc_combined | 1.26 → 1.00 (-21%) | 1.33 → 1.19 (-21%) |
| interp_calc | 1.41 → 1.17 (-17%) | 1.55 → 1.23 (-21%) |
| hash_chain | 1.64 → 1.72 (noise) | 2.29 → 1.73 (-24%) |

実装中に 2 つのバグを発見・修正:

1. `mark_gen` の major で `sweep_young` が promoted 物の marked bit を
   clear し、 後続 `sweep_old_pages` が「marked=false の old」 を free に
   してしまう問題。 `clear_marked` パラメータを sweep_young に追加し、
   minor は true、 major は false で呼ぶ。

2. `mark_gen_inc` の incremental cycle で「inc_marking 中の新 alloc が
   stack WB 不在で漏れる」 古典的問題。 binary_trees が 4194301 vs 正解
   4194303 で off-by-2 になっていた。 `inc_finish_sweep` で root を
   再走査する mark phase を追加して修正。

全 11 backend で test 3 種 (plain + stress) + 7 bench (binary_trees /
string_concat / hash_chain / nqueens / life / fib_pair / cons_list)
が PASS。

## 2026-05-17 (18) — `gc_mark` を slab/page allocator に書換え (CRuby 風)

per-object malloc + 線形 prev/next リストを撤廃し、 GC が自前で page
heap を持つ slab allocator に書換え:
- 16 KiB page を size class ごとに mmap (32/64/128/256/512/1024/2048/3072/4096 B)
- size class より大きい alloc は個別 mmap (large object list)
- free slot は kind=KIND_FREE + payload に FreeSlot.next を overlay
- sweep は page 内 slot を sequential walk して unmark を freelist に push

malloc 比でわかりやすく速い:
- string_concat 1.68 → 0.70 s (-58%)
- fib_pair 1.46 → 0.89 s (-39%)
- cons_list 1.20 → 0.84 s (-30%)
- substr_churn 1.44 → 1.14 s (-21%)
- list_alloc 1.15 → 0.92 s (-20%)
- binary_trees 0.96 → 0.86 s (-10%)

heap を GC 側が提供する形式は CRuby と同型。 線形リストなし → GCHeader
12 → 16 bytes (`_Static_assert` で 16 固定)。

## 2026-05-17 (19) — baruby (libgc) との fair 比較を perf.md §3 で公開

姉妹サンプル `sample/baruby` (Boehm libgc 経由 conservative scanning)
との比較。 fairness のため non-GC な差分 (parser fix iter (12)、 bench
6 種) を baruby へ port (commit 34be8d2)。 `life.ba.rb` は baruby に
top-level long while loop の独立バグがあり 11 bench で比較。

結果: baruby_precise の最速 backend が **全 11 bench で libgc を上回る**
(geomean ~ -22%)。 最大差は string_concat -46% / binary_trees -40%。
最小差は nqueens -7% / list_sort -9% (mutator 支配ワークロード)。

旧表では precise (`copy` 単体) は libgc と互角〜+15% でバラついて
いたが、 (5)〜(18) の追加 backend と一連の最適化で「適切な backend を
選べば全 bench で libgc を超える」 という結果に。

## 2026-05-17 (17) — `max_pause_ms` 計測を追加 (latency upper-bound)

`BarubyGCStats` に `max_pause_seconds` を追加し、 `baruby_gc_time_end`
で 1 回の collect の最大 wall time を tracking。 GC_STATS 出力に
`max_pause_ms=...` を追加、 `bench/run.rb` の table にも `max_ms` 列を
追加。

binary_trees 実測:

| Backend | gc_seconds | max_pause_ms | 解釈 |
|---|---:|---:|---|
| mark_gen | 0.55 s | **288 ms** | 1 つの major sweep が支配 |
| mark_gen_inc | 0.24 s | **54 ms** | inc で mark / finish_sweep が分離 (5.4× short) |
| copy_gen | 0.43 s | 18 ms | minor のみ、 major なし |
| mark_compact_gen | 0.43 s | 18 ms | minor のみ |
| mark_bump_gen | 0.53 s | 55 ms | major: promote + sweep |

latency 重視ワークロードでの backend 選択基準ができた。 mark_gen_inc は
total throughput では mark_gen と差がない (current INC_WORK_PER_ALLOC =
SIZE_MAX のため真の incremental ではない) が、 mark / finish_sweep の 2 段
分割で max pause が大幅に短くなる効果が出ている。

全 11 backend で test 3 種 + bench 12 種が PASS。

## 2026-05-17 (16) — `mark_bump_gen` の線形リスト撤廃 + region 走査 sweep (-20% binary_trees)

(15) で tenured を bump 化したが線形リスト (prev/next) は維持していた。
今回それを撤廃し、 sweep を「tenured region を header-size-prefix で
sequential walk」 に変更。

効果:
- binary_trees: 1.15 → 0.92 s (-20%)。 累積で v1 (1.41 s) の -35%
- GCHeader: 40 → 24 bytes (prev/next 削除で 16 bytes 縮小)
- sweep が pointer chasing から sequential scan になり cache miss 激減

設計空間における最終位置付け:
- `mark_gen`: malloc nursery + malloc 線形リスト tenured (free に返却)
- `mark_bump_gen` v3: bump nursery + bump tenured + region 走査 sweep
                      (compaction なし、 領域累積)
- `mark_compact_gen`: bump nursery + bump tenured + slide compact
                      (compaction で領域再利用)
- `copy_gen`: bump nursery + bump tenured + Cheney compact (semispace)

binary_trees で mark_bump_gen 0.92 vs mark_compact_gen 0.79 の差は
compaction の cache locality 改善 + region 再利用効果 (~15%)。

全 11 backend で test 3 種 (plain + stress) + bench 12 種が PASS。
perf.md §2 table 更新。

## 2026-05-17 (15) — `mark_bump_gen` の tenured を bump 化 (-18% binary_trees)

(13) で導入した `mark_bump_gen` の tenured を「per-object malloc + 線形
リスト」 から「1 GiB mmap region への bump alloc + 線形リスト」 に変更。
linked list はまだ通すので mark+sweep 意味論は維持、 ただし `free_unlink`
は個別 free() せずリストから切るだけ (memory leaks until program exit、
ただし bench は短時間なので OK)。

効果:

| Bench | 旧 (v1: malloc tenured) | 新 (v2: bump tenured) | 差 |
|---|---:|---:|---:|
| binary_trees | 1.41 | **1.15** | -18% |
| interp_calc | 1.12 | **1.03** | -8% |
| 他多数 | (noise level) | (noise level) | ±5% |

binary_trees は major 中に 2M slot を malloc していたのが bump (~1 ns) に
なって ~150 ms 削減。

設計空間における位置付け:
- `mark_gen`: malloc nursery + malloc 線形リスト tenured
- `mark_bump_gen` v1: bump nursery + malloc 線形リスト tenured
- `mark_bump_gen` v2 (今): bump nursery + bump tenured (no compact)
- `mark_compact_gen`: bump nursery + bump tenured + slide compact
- `copy_gen`: bump nursery + bump tenured + Cheney compact

v2 と mark_compact_gen / copy_gen の違いは「major で compact するか」 だけ。
compact しない v2 は major 中の slot 移動コストがゼロだが、 領域は
累積消費 (1 GiB で OOM)。 短時間 bench では問題なし。

全 11 backend で test 3 種 (plain + stress) + bench 12 種が PASS。

## 2026-05-17 (14) — `realloc_payload` を sp_top[0] rooting で統一

9 つの GC backend (none / bump を除く) の `baruby_gc_realloc_payload` を
sp_top[0] に old を root して GC に追跡させる方式に統一 (commit e5b237f)。
旧来は backend 毎に方式がバラバラ:
- 非 moving (mark / mark_gen / mark_gen_inc): malloc-buf 中間
- moving 単一 region (copy): stress mprotect 対策で malloc-buf
- moving + gen 系: alloc-first + oldh->fwd 参照 (latent race あり)
- moving + compact (mark_compact): malloc-buf

旧 oldh->fwd 方式に潜む race: oldh が nursery_base 直近で minor が
fire すると次の alloc が oldh のバイトを上書きし fwd field が読めなく
なる。 hash_chain 等で稀に発火するが通常は深い nursery 位置なので
未顕在化していた。

sp_top[0] rooting で:
- GC が sp_top[0] を root として scan し forward する (universal pattern)
- 非 moving: sp_top[0] 不変、 sweep が old を free しない保証
- moving: sp_top[0] に post-move アドレスが入る
- stress mode mprotect 後でも sp_top[0] は to-space を指すので OK

副次効果 (perf 表 §2 refresh):
- `mark` string_concat 2.41 → 1.68 s (-30%)、 substr_churn 1.53 → 1.44 s
- `bump` hash_chain 1.50 → 1.11 s (-26%)
- 他は noise レベル

`gc_copy.c` だけは戻した (commit 82e84ec): 単一 region semispace は from /
to が別 region なので race の対象外、 sp_top[0] パターンは正しさには
寄与せず alloc-heavy bench で 5% 程度の regression が出ていた。

全 11 backend で test 3 種 (plain + stress) + bench 12 種が PASS。

## 2026-05-16 (13) — 11 つ目の backend: `mark_bump_gen`

bump-allocated nursery + linked-list mark&sweep tenured の hybrid。
既存設計空間における穴を埋める:

| Backend | Nursery | Tenured |
|---|---|---|
| `mark_gen` | malloc per-object linked list | malloc per-object linked list (mark&sweep) |
| `mark_compact_gen` | bump region (16 MiB) | bump region (512 MiB, mark+slide compact) |
| `mark_bump_gen` (新) | bump region (16 MiB) | malloc per-object linked list (mark&sweep) |

実装:
- 既存 generational インフラ (remset + WB) を継承
- Minor: bump nursery を scan、 marked obj を tenured (malloc + 線形リスト
  link) に promote、 nursery_top を reset。 Cheney FIFO queue で
  freshly-promoted obj から outgoing refs を follow。
- Major: 1 パスで「mark 既存 tenured + promote nursery 生存物」 を同時に
  行う。 root から scan、 nursery ref は in-place で promote 後の addr に
  書換え、 tenured ref は mark + gray queue。 純粋 mark&sweep の loop と
  生存物 promote の loop を統合することで O(live) で済む (素朴な
  「mark → 個別 promote → fixup ループ」 だと O(live × depth) になる)。
- 旧 generational 同様 adaptive major threshold を採用 (`max(MIN, 2×live)`)

性能特性:

| Bench | mark\_gen | mark\_bump\_gen | 効果 |
|---|---:|---:|---|
| string_concat | 1.67 | **0.60** | -64% (短命 alloc が nursery 完結) |
| fib_pair | 1.65 | **0.97** | -41% |
| list_alloc | 1.36 | **0.96** | -29% |
| substr_churn | 1.74 | **0.93** | -47% |
| binary_trees | **1.38** | 1.49 | +8% (long-lived は逆効果) |

short-lived ワークロードでは bump nursery が劇的に効く (mutator alloc が
malloc → ポインタ加算で 10× 速く、 死ぬ obj は scan 不要)。 long-lived
(binary_trees) では major が 2M slot を malloc + memcpy するので
mark_gen より逆に遅い。 `mark_compact_gen` と比較すると tenured 戦略の
差 (compact vs linked-list mark&sweep) が major コストに反映 (1.49 s vs
0.84 s)。

11 backend × test 3 種 (plain + stress) + bench 12 種が全 PASS。
[perf.md](perf.md) §2 に新 column 追加。

## 2026-05-16 (12) — parser バグ修正: binop 内 >3-arg call のオペランド競合

(11) で発見した parser バグを根治。 真因は: `n + foo(a, b, c, d, e)` のように
binop の RHS が >3 引数 call の場合、 call は (specialized が ≤3 のみ
対応のため) 一般パスで lset chain + `node_call` を発射する。 lset は
`fp[arg_idx..]` に args を書く。 arg_idx は parser が決めるが、
binop が使う sp[0..1] = fp[locals_cnt..locals_cnt+1] と同じ範囲に被ると
inner binop の rhs eval が arg slot を上書きしてしまう。 また args 内に
`x + 1` のような binop があると、 inner binop の sp[1] = outer.sp + 2 も
arg slot に被る (parent's sp + 1 から評価するため)。

修正は `baruby_parse.c::alloc_binop` 呼出前に `arg_index` を 4 slot bump
してから lhs/rhs を transduce、 後で rewind する。 これで:
- sp[0..1] (= outer binop の作業領域) は予約済み
- inner binop の sp[1] = outer.sp + 2 も予約範囲内
- 2-deep binop nesting in args まで対応 (実用的には十分)

検証: 元の repro (`bench/life.ba.rb` の inline `n + get(g,w,h,x±1,y±1)`
× 8) が動き、 全 10 backend で final population = 112 を一致確認。
`life.ba.rb` から workaround の temp-var bind を撤去し inline 形に戻して
よりシンプル化 (1.54 s → 1.30 s も bonus でついた)。

## 2026-05-16 (11) — `bench/life.ba.rb` 追加 + parser バグ発見

Conway's Game of Life の 80×80 grid × 200 tick macro bench を追加
(plain ~1.5 s)。 各 tick で grid を fresh alloc し前 tick を捨てる nursery
形ワークロード。 baruby は GC pressure が低い (実測 0-7 GC、 gc_pct < 0.5%)
ので「GC 自体は速いが mutator が支配的」 ケースの代表サンプル。 10
backend 全てで final population = 112 を一致確認。

副次成果: 実装中に baruby の parser バグを発見。
`n = n + get(g, w, h, x, y)` のように binop の RHS に 4+ 引数呼出を
書くと、 call が arg を `fp[arg_idx..]` に書き込む際に binop の sp[0]
(= LHS) を上書きする ([todo.md](todo.md) P0 参照)。 回避は call 結果を
一旦 local に bind すること。 `life.ba.rb` ではこのパターンを採用。

## 2026-05-16 (10) — `mark` family の major threshold を適応的に

`mark` の `gc_threshold` (= GC を発火する累積 alloc bytes) と
`mark_gen` / `mark_gen_inc` の `old_major_threshold` を固定値 (4 MiB / 64 MiB)
から適応的 (`max(MIN, 2 × live_bytes_post_sweep)`) に変更。 各 sweep が
O(heap) なので、 live 200 MiB のワークロードで 4 MiB ごとに発火していた
旧版は ~50 回 GC していたが、 新版は 4 回程度で済む。

効果:

| Backend | Bench | 旧 → 新 | 速度 |
|---|---|---|---|
| `mark` | binary_trees | 7.54 s → **0.97 s** | **7.8×** |
| `mark_gen` | binary_trees | 1.59 s → 1.38 s | 13% |
| `mark_gen_inc` | binary_trees | 1.61 s → 1.44 s | 10% |

short-lived workload (string_concat, list_alloc 等) では heap が MIN
(4 / 64 MiB) を超えないので動作不変。 `mark_compact` 系は単一 region
bump alloc なので threshold 概念がなく未変更。

## 2026-05-16 (9) — `bench/nqueens.ba.rb` 追加 + 全 backend bench refresh

N=11 の N-queens を backtracking で解く macro bench を追加。 2680
solutions を ~1 s で確認。 deep recursion + per-frame Array alloc
(column set を functional copy で pass-down) という LIFO 短命 alloc 主体の
形状で、 nursery 完結 backend の benefit が出やすい。

全 10 backend × 11 bench の 3-run 中央値を再測定し
[perf.md](perf.md) §2 を更新。 `copy_gen_inc` が 11 bench 中 8 で勝ち、
2026-05-16 (8) の realloc 修正で malloc/free を消したのが string_concat
(0.52 s) や hash_chain (1.21 s) で効いている。 `mark` は binary_trees で
7.54 s (89% GC) と相変わらず重く、 per-object malloc + sweep walk の
コストが浮き彫り。

## 2026-05-16 (8) — `baruby_gc_realloc_payload` の stale-ptr バグを根治

前 iter で診断した「3 つの moving-gen backend で hash_chain が落ちる」
バグの真因を発見し修正。 真の原因は EVAL_ARG の uninit slot ではなく、
`baruby_gc_realloc_payload` の構造的バグだった:

```c
// 旧 (バグあり)
memcpy(buf, old, copy_bytes);           // (1) old の bit pattern を buf に
void *newp = baruby_gc_alloc(...);      // (2) 中で GC fire → old の指す先が動く
memcpy(newp, buf, copy_bytes);          // (3) buf 内の ptr 値は pre-GC アドレスのまま
```

(1) で buf に copy された VALUE ptr 達は、 (2) の GC で移動先 (tenured)
に forward され、 (3) で newp に書かれるのは pre-GC = stale アドレス。
chain.items が newp になった後、 次回の minor で scan されると stale
nursery ptr を forward しようとして `process_object: unknown kind`
で abort。

修正方針: alloc を先に呼んでから、 forward 情報 (oldh->fwd) を経由して
post-GC の old location から memcpy:

```c
// 新
void *newp = baruby_gc_alloc(...);                     // (1) GC があれば fire
const void *cur_old = oldh->fwd ? oldh->fwd : old;     // (2) forward 先を解決
if (copy_bytes) memcpy(newp, cur_old, copy_bytes);     // (3) post-GC の ptr が入る
```

`gc_copy_gen.c` / `gc_copy_gen_inc.c` / `gc_mark_compact_gen.c` の 3 ファイル
に適用。 `gc_copy.c` は stress mode で from-space に mprotect PROT_NONE が
かかる仕様のため oldh->fwd が読めず、 旧 buf 方式のまま残す
(現状 hash_chain は copy で 1 GC のみなのでバグは表面化していない)。

副次対応として `node.def` の EVAL_ARG 新 sp_top 指定も「初期化済みスロット
のみ scan」 になるよう `sp + 2` を `sp / sp + 1` に段階化
(`node_call_aget`, `node_call_aset`, `node_call_push`, `node_ary_push`,
全 binop)。 これだけでは根治しなかったが、 framework としての健全性は
上がっており、 別ワークロードで隠れていた同型バグへの防御として残す。

検証: 全 10 backend で test 3 種 (plain + stress) と hash_chain が PASS。

## 2026-05-16 (7) — `bench/hash_chain.ba.rb` 追加 + uninitialized sp 穴の診断

Macro bench で「Array on Array」 形式のチェーンドバケット hash table を
実装。 2048 buckets / 150k keys / 3 rounds で plain ~1.5 s。 long-lived
buckets + medium-lived chains + short-lived `[k, v]` pairs の 3 層 lifetime
を持つので、 nursery + remset の組合せが効くワークロード。

10 backend のうち 7 で正常 (none / mark / mark_gen / mark_gen_inc / copy /
mark_compact / bump)。 残 3 (copy_gen / copy_gen_inc / mark_compact_gen)
は `process_object: unknown kind` で abort する既知バグを露呈:

> nested array literal (`[k, v]`) を chain.push に渡すと、 `node_call_push`
> および `node_ary_push` の引数評価で `BARUBY_EVAL_ARG(c, recv, sp + 2)` が
> 渡されるが、 そのとき `sp[1]` (val スロット) は未初期化のまま GC scan
> 範囲に入る。 過去フレームの leftover nursery ptr が残っていると
> forward_obj が stale ヘッダを follow して to-tenured へ corrupt copy →
> Cheney scan で unknown kind 検出 → abort。 minor GC 入口の高水位
> zeroing は sp_top retreat 経路でしか働かず、 sp_top が高い状態で
> uninit slot を拾うケースは未保護。

詳細と修正方針は [todo.md](todo.md) の P0 エントリ
「uninitialized sp scratch slot in GC scan range」 参照。 単発の `sp + 2`
を `sp + 1` / `sp` に下げる試みは効かなかった (バグの発火経路が他にも
あり)。 系統的審査が要る。

## 2026-05-16 (6) — 全 backend に GC 時間計測 (`gc_seconds` / `gc_pct`)

`BarubyGCStats.total_seconds` を追加し、 各 backend の collect entry を
`baruby_gc_time_begin()` / `baruby_gc_time_end()` で挟むことで
ミューテータ時間と GC 時間を分離。 `BARUBY_GC_STATS=1` で:

```
__GC_STATS__ backend=mark_gen alloc_bytes=... gc_count=133 minor=133 major=0 \
             gc_seconds=0.1648 gc_pct=12.3
```

実装ポイント:
- `gc.h` に `extern int baruby_gc_time_depth; extern struct timespec baruby_gc_time_t0;`
  を置き、 minor が major を呼ぶ (mark_compact_gen 等) re-entrant ケースで
  最外側だけ計測する depth-guard を入れた。
- `CLOCK_MONOTONIC` を使うことでサスペンド・時刻変更の影響を排除。
- 8 backends (`mark`, `mark_gen`, `mark_gen_inc`, `copy`, `copy_gen`,
  `copy_gen_inc`, `mark_compact`, `mark_compact_gen`) の collect / minor /
  major / inc_finish_sweep 全 entry に追加。 `none` と `bump` は GC を
  しないので何もしない (`gc_seconds=0.0000`)。

これで以後の perf チューニングで GC vs mutator の振り分けが clear に
わかる: 例えば mark_gen_inc の binary_trees で 1.53s 中 0.26s (16.9%) が
GC、 mark_compact_gen の同 bench は 0.83s 中 0.41s (49.3%) が GC で、
gen+compact は GC が重い代わりに mutator-side が速い (連続配置による
cache friendliness) ことが定量化できる。

## 2026-05-16 (5) — 10 つ目の backend: `bump` (allocation floor baseline)

GC を全く行わず単一 4 GiB region への bump alloc のみ。 OOM 時 abort。
`none` (libc malloc + leak) より strictly に速い: malloc 内の bin 管理が
ないぶん、 alloc は cmp + add のみ。

役割: 「rooting + WB + dispatch + sp[] threading」の最小コストを示す
baseline。 binary_trees で 0.53s = `copy` の 0.56s より速い (GC オーバー
ヘッドが完全に消えるので)。

全 8 bench で `none` を上回る:

| Bench         | none  | bump  |
|---------------|------:|------:|
| binary_trees  | 0.62  | 0.53  |
| list_alloc    | 1.47  | 1.13  |
| string_concat | 1.69  | 0.92  |
| fib_pair      | 1.68  | 1.26  |
| substr_churn  | 1.77  | 1.18  |
| gc_combined   | 1.49  | 1.21  |
| interp_calc   | 1.34  | 1.18  |
| list_sort     | 1.29  | 1.23  |

## 2026-05-16 (4) — 9 つ目の backend: `mark_compact_gen` (gen + Lisp-2 hybrid)

`copy_gen` の major (semispace Cheney) を `mark_compact` (Lisp-2 sliding) に
差し替えた generational hybrid。

- Nursery: 16 MiB bump (`copy_gen` と同じ)
- Tenured: 512 MiB single region (copy_gen は 2×256 MiB だった)
- Minor: Cheney-style nursery → tenured (= copy_gen と同じ)
- Major: tenured 内で mark + Lisp-2 sliding compact (3-pass)
- WB / remset: copy_gen と同じ

メリット: tenured 仮想空間が 1×512 MiB (vs copy_gen は 2×256 MiB)。
デメリット: major が semispace より複雑 (3-pass) だが compact 自体は速い
(連続 marked を memmove で batch)。

性能 (plain, 1 run、 vs copy_gen / copy_gen_inc):

| Bench         | copy_gen | copy_gen_inc | **mark_compact_gen** |
|---------------|---------:|-------------:|---------------------:|
| binary_trees  |     0.82 |         0.82 |            **0.78** |
| list_alloc    |     0.97 |         0.96 |            **0.89** |
| string_concat |     0.59 |         0.53 |            **0.51** |
| fib_pair      |     0.95 |         0.92 |            **0.81** |
| substr_churn  |     0.92 |         1.04 |                0.93 |
| gc_combined   |     0.93 |         1.08 |                0.93 |
| interp_calc   |     1.00 |         0.98 |                1.00 |
| list_sort     |     1.13 |         1.16 |            **1.08** |

binary_trees / list_alloc / string_concat / fib_pair / list_sort の **5/8 で
mark_compact_gen が gen 系の中で最速**。 copy_gen の Cheney は 2 region 間
の memcpy が連続するので tenured へ大量 promote する worklload に強いが、
mark_compact_gen は **in-place compaction で 1 region で済む**ぶん帯域節約。

## 2026-05-16 (3) — mark_compact の slide 段階を batching

3-pass の最終 (slide) で、 連続 marked オブジェクトは src - dst delta が
共通なので 1 回の `memmove` に纏められる。 dead が間に挟まると delta が
変わるので runs を分割。 数百万回の memmove 呼び出しを runs 単位に削減。

影響は限定的: binary_trees / list_alloc などで誤差程度。 mark_compact の
ホットスポットは GC 自体ではなく dispatch (perf record で DISPATCH_node_if
13%, _ary_push 9% など) で、 GC 内最適化のリターンが小さいと判明。

## 2026-05-16 (2) — 8 つ目の backend: `mark_compact` (Lisp-2 sliding compactor)

`gc_mark` の per-object malloc/free を回避しつつ非 moving (compaction 時の
み移動) を実現する 8 つ目の backend。 単一 mmap'd region (1 GiB virtual,
lazy-paged) からの bump alloc + 古典的「Lisp 2」 圧縮:

1. **Mark**: BFS from roots via gray queue (= mark_gen と同じ)
2. **Forward-address pass**: region を線形走査、 marked オブジェクトの
   ->fwd に packed dest 計算
3. **Update-pointers pass**: 再び線形走査、 marked の outgoing pointer
   (a->items, s->bytes, items[i]) を target の ->fwd に書き換え。 root も
4. **Slide pass**: 各 marked を ->fwd へ memmove。 dst ≤ src なので
   memmove で安全、 連続 src だが間に dead があると memmove は分裂

### 詰まったポイント

- **stress mode で test_eq.ba.rb が SEGV**: `update_pointers` が
  `s->bytes` 0x7....0220 (region top の少し外) を deref → 高 sp slot に
  stale heap pointer が残っていて root scan で誤って live と判定された。
  copy_gen 同様に **high-water-mark zeroing** を追加 (前回の最深 sp 以下、
  かつ現在の sp_top より上の slot を 0 で埋める) で解決
- 全 test (plain + stress) + 全 bench で動作確認済み

### 性能 (plain mode, 1 run)

binary_trees で **mark の 7.18s → 0.59s** に (12×)。 list_sort や fib_pair
は世代別系 (copy_gen) には負けるが、 mark との比較では概ね optimal。

## 2026-05-16 — gen 系 backend の explicit remset + macro bench 追加

### 性能改善: explicit remembered set

mark_gen / mark_gen_inc / copy_gen / copy_gen_inc の 4 backend で、
旧版が minor GC で行っていた「dirty bit を求めて old/tenured 全走査」
(= O(|old|)) を、 WB で push される明示 remset (= O(|dirty|)) に置換。

- WB: holder->dirty が false なら remset に push し dirty = true
- minor: remset を走査して dirty=true のものだけ scan_outgoing
- major: remset を破棄して全 trace、 sweep で生存者の dirty を clear

perf record で interp_calc on mark_gen を見ると minor_gc が 44% を
占めていた。 remset 化で:

| Bench         | mark_gen 旧 | mark_gen 新 | copy_gen 旧 | copy_gen 新 |
|---------------|------------:|------------:|------------:|------------:|
| binary_trees  |        2.28 |    **1.56** |        1.11 |    **0.79** |
| interp_calc   |        2.87 |    **1.51** |        1.22 |    **1.07** |
| gc_combined   |        1.39 |        1.33 |        0.93 |        0.91 |
| list_sort     |        1.36 |        1.33 |        1.16 |        1.05 |

### マクロベンチ追加

- **`interp_calc.ba.rb`**: depth-12 AST を make_expr で構築 → eval_expr で
  再帰評価。 1000 反復。 build phase が alloc burst、 eval phase は
  純計算。 short-lived alloc + recursive read の典型
- **`list_sort.ba.rb`**: 2000 要素の整数 array に merge sort を 350 回
  実行。 merge 1 回が中規模 alloc burst を生み、 merge 完了で全部死ぬ
  パターン

## 2026-05-15 — GC backend を 7 種から build-time 選択可能に

`Makefile GC=<backend>` で 7 種類の GC アルゴリズムから build-time に
選べるようにした。 全 backend で test.ba.rb / test_ary / test_eq の
plain + stress mode、 bench 6 種が PASS。

### Backend 一覧

| GC値 | 名前 | 説明 |
|---|---|---|
| 1 | none | malloc + leak (rooting オーバーヘッドの baseline) |
| 2 | mark | non-moving mark&sweep (linked list of objects) |
| 3 | mark_gen | mark&sweep + 2-gen (nursery / tenured list) |
| 4 | mark_gen_inc | mark_gen + SATB 風 incremental marking infra |
| 5 | copy | semispace Cheney (現状の default) |
| 6 | copy_gen | nursery (bump) + tenured (semispace) |
| 7 | copy_gen_inc | copy_gen + 増分 major marking infra |

`make GC=mark_gen` のように選択。 未指定なら `GC=copy` (default)。
`-DBARUBY_GC=<N>` が Makefile から渡される。

### Infrastructure 整理

- `gc.h` を共通 interface 化 (BarubyGCKind / BarubyGCStats / WB hooks)
- backend ごとに `gc_<name>.c` (~200〜400 行)
- WB() macro: 非世代別 backend では no-op (`*slot = v`)、 gen 系は
  remset (dirty bit) を更新
- node.c / node.def の heap pointer 書込を全部 `baruby_gc_wb` /
  `baruby_gc_wb_bulk` 経由に統一 (6 箇所)
- stats output に `backend=<name>` と minor/major カウントを追加

### 実装と詰まったポイント

- **mark_gen の `promote()` バグ**: major GC で sweep_young が marked を
  clear してから sweep_old がスキャンすると、 新規 promote が unmarked と
  判定されて free される。 `promote(h, clear_marked)` を導入、 major では
  `clear_marked=false` で運用、 minor では `true` で運用
- **copy_gen の tenured 容量**: binary_trees の live tree は ~352 MB
  (header + payload 別 alloc で BaArray ノードは 88 byte/個)。 tenured
  semispace を 512 MiB に拡張
- **copy_gen の `from_end_cur`**: from-tenured の range check が region
  全体ではなく valid object 範囲 (= old_active_top まで) でないと、
  stale pointer が forward 経路に入って memcpy SEGV
- **copy_gen の pretenuring**: `nursery_size/2` を超える alloc は直接
  tenured に。 18 MB の string repeat (substr_churn) が小 nursery に
  入らない問題を回避
- **inc 系 backend の SATB 限界**: VALUE stack write には barrier が
  無いため、 純粋な SATB だけでは stack 経由で reachable になった
  オブジェクトを取りこぼす。 atomic root re-scan を追加したが、
  testbed としては安全側で「INC_WORK_PER_ALLOC = SIZE_MAX」 = 実質
  STW major としている。 infra (gray queue / SATB barrier) は残しているので
  stack-WB を入れれば真の incremental に切替可能

### 性能 (plain mode, 1 run, vs libgc baruby)

| Bench         | libgc | none  | mark  | mark_gen | mark_gen_inc | copy  | copy_gen | copy_gen_inc |
|---------------|------:|------:|------:|---------:|-------------:|------:|---------:|-------------:|
| binary_trees  | 0.91  | 0.60  | 7.17  | 2.28     | 2.30         | 0.53  | 1.11     | 1.16         |
| list_alloc    | 1.09  | 1.32  | 1.13  | 1.28     | 1.41         | 1.16  | 0.92     | 0.95         |
| string_concat | 0.97  | 1.70  | 1.72  | 1.64     | 1.75         | 0.94  | 0.50     | 0.55         |
| fib_pair      | 1.13  | 1.63  | 1.45  | 1.59     | 1.66         | 1.22  | 0.91     | 0.93         |
| substr_churn  | 1.36  | 1.74  | 1.23  | 1.64     | 1.78         | 1.31  | 0.87     | 0.92         |
| gc_combined   | 1.08  | 1.46  | 1.23  | 1.39     | 1.49         | 1.20  | 0.90     | 0.97         |

**観察**:
- **copy_gen が string-heavy で圧勝** (string_concat 0.50 s = libgc の 0.52×).
  短命 string の churn が nursery 経由でほぼ memcpy 不要に処理される
- **binary_trees は plain copy が最速** (0.53s). gen は long-lived tree
  の promote コストで遅くなる
- **mark は binary_trees が極端に遅い** (7.17s). 数百万オブジェクトの
  per-object malloc + sweep walk
- **none baseline は意外と遅い**: malloc の overhead で copy より遅い場面が
  多い。 bump alloc の威力

## 2026-05-14 — alloc 周りのオーバーヘッド削減

perf record で hot path を特定し、 string-alloc 系のオーバーヘッドを
潰した。 詳細 [perf.md §4](perf.md)。

### 変更内容

- `baruby_gc_alloc` を分割: 通常版 (zero-init payload) と
  `baruby_gc_alloc_byte` (memset スキップ)。 KIND_PAYLOAD_BYTE は
  caller が即座に bytes を埋めるので memset 不要
- `baruby_str_new` の malloc バッファ撤去。 caller が source の寿命を
  保証する前提に変更 (rodata / C スタック / GC-rooted)
- `baruby_str_slice(VALUE *src_ref, offset, len, sp_top)` を新設、
  heap interior 起点の slice (node_call_aget / _aget2 の STR 経路)
  はこちらに移動
- `baruby_gc_realloc_payload` も内部で kind 別に dispatch
  (PAYLOAD_BYTE は alloc_byte 経由)
- `Makefile`: `-flto=auto` を追加。 fib_pair 等で小さい alloc が
  inline されて -4% 効く

### 性能 (5 run 中央値、 plain mode、 vs `sample/baruby` libgc)

| Bench | conservative | precise (before) | precise (after) |
|---|---:|---:|---:|
| binary_trees | 0.907 s | 0.544 s | 0.576 s |
| list_alloc | 1.085 s | 1.152 s | 1.175 s |
| **string_concat** | 0.968 s | 1.160 s | **0.961 s** (-17%) |
| fib_pair | 1.127 s | 1.271 s | 1.285 s |
| **substr_churn** | 1.361 s | 1.594 s | **1.354 s** (-15%) |
| gc_combined | 1.079 s | 1.231 s | 1.244 s |

geomean ≈ 0.98× (precise が conservative より 2% 速い)。
string-heavy ベンチが parity 到達。 stress mode の全テスト PASS 維持。

## 2026-05-13 — semi-space moving GC + stress mode + ASTRO_ASSERT

mark&sweep の MVP を **Cheney 風 copying GC** に置き換え、 stress mode で
moving GC 特有のバグを総当たり退治した。 詳細 [runtime.md §5](runtime.md)。

### gc.c の刷新

- `BarubyGCNode` の linked-list + per-object malloc を捨て、
  **`mmap` 512 MiB の region 2 本を交互に使う semi-space** に変更
- alloc は `active_top` を bump するだけ。 collection は Cheney scan-loop で
  to-space を線形に処理
- `GCHeader { kind, size, fwd }` を payload 直前に置き、 forwarding pointer は
  この `fwd` に書く

### Stress mode (`BARUBY_GC_STRESS=1`)

- **毎 alloc で GC 起動** + 古い from-space を `mprotect(PROT_NONE)` +
  `madvise(MADV_DONTNEED)` で**恒久 retire**。 仮想アドレスは予約継続、
  物理ページは即解放
- 過去 GC 由来の stale pointer を deref すると確実に SIGSEGV
- 新しい to-space は毎 GC で `mmap` 取り直し (アドレス使い捨て)
- PRE-MARK 不変条件チェック: scan range の `IS_PTR(v)` が必ず現在の
  from-space を指す事を mark 前に検証

### 摘発したバグ

semi-space に切り替えた瞬間 `bench/binary_trees` が clobber data で
クラッシュ。 stress mode + verbose assert で次の根本パターンを発見:

- **C local rooting 漏れ** — `VALUE l = EVAL_ARG(c, lhs); VALUE r =
  EVAL_ARG(c, rhs);` で rhs eval が GC を引くと `l` が stale C local の
  まま。 該当箇所:
  - `baruby_ary_push`: x が realloc 後に stale → `VALUE *x_ref` に変更
  - `node_eq`, `_neq`, `_lt`, `_le`, `_gt`, `_ge`, `_mul`, `_spaceship`,
    `_call_aget`, `_call_aget2`: heap-typed operand を sp[] spill に統一
- **Helper 内部の C local** — `baruby_str_concat(VALUE av, ...)` の `av`
  が内部 alloc 後に stale。 → `VALUE *av_ref` に変更し、 alloc 後に
  `VAL2STR(*av_ref)` で post-GC アドレスを再取得 (`baruby_ary_plus`,
  `baruby_str_repeat`, `baruby_ary_repeat`, `baruby_str_append`,
  `baruby_str_concat`)

### `baruby_str_concat` 最適化

ref pattern 移行のついでに、 旧版で「source bytes を malloc 領域に
バッファコピーしてから alloc」 と書いていた回避コードを撤去。
source は ref で post-GC 再取得できるので malloc/memcpy/free を 1 set
削減 → **string_concat ベンチ 1.468 s → 1.160 s (-21%)**。

### ASTRO_ASSERT / ASTRO_DEBUG

framework 共通の assertion macro を `runtime/astro_debug.h` に新設:

```c
#if ASTRO_DEBUG
#  define ASTRO_ASSERT(expr) assert(expr)
#else
#  define ASTRO_ASSERT(expr) ((void)0)
#endif
```

baruby_precise では `ASTRO_DEBUG=1` がデフォルト (context.h)、
`make ASTRO_DEBUG=0` で release-shape build が可能。 gc.c の検証コード
(alloc 時 kind validity, process_object の type タグ、 stress mode の
PRE-MARK / FORWARD STALE 検出) は全て ASTRO_ASSERT に統一、
release build では完全に compile out。

### 検証

全テスト stress mode で PASS:

| Test | plain | stress |
|---|---|---|
| `test.ba.rb` | ✓ | ✓ |
| `test_ary.ba.rb` | ✓ | ✓ |
| `test_eq.ba.rb` | ✓ | ✓ |
| `bench/binary_trees` | ✓ (0.54 s) | (時間がかかるので未) |
| `bench/list_alloc` | ✓ (1.15 s) | (時間がかかるので未) |
| `bench/string_concat` | ✓ (1.16 s) | (時間がかかるので未) |

precise vs conservative の比較は [perf.md §2](perf.md) に。

## 2026-05-10 — bench 拡充 (GC stress 3 種追加)

既存の binary_trees / list_alloc / string_concat に追加で:

- **gc_combined** — 50k 要素配列を保持しつつ 10M 回の 4 要素配列 churn。
  「長寿命 + 短寿命チャーン」の **generational-friendly** 形 (今 libgc が
  非世代別なので差は出ないが、世代別 GC 投入時のベースライン)。
- **substr_churn** — 18 MB の text String を保持して、毎オフセットで
  `[i, 5]` slice。**fine-grained substring alloc + 1 long-lived**。GC
  回数は 52 と最低 (heap が text サイズで安定するため)。
- **fib_pair** — 再帰 fib が毎フレームで `[a, b]` 2 要素配列を返す。
  **frame-escape + deep stack** (depth 28、~317k フレーム peak)。precise
  GC を入れたとき frame iterator のスループットがここで効く想定。

各々 plain で ~1 s 持続、AOT 比 1.78〜2.74× 速い。perf.md §2 / §3 に
全 6 bench の表 (実測値 + 寿命プロファイル + GC 頻度) を整理。

## 2026-05-10 — A+B バッチ (`<=>` / `*` / `<<` / escape / AOT/PG verify / JIT 撤去)

### A — 残り P1 機能

- **`<=>`** (`node_spaceship`)。Int+Int / Str+Str は `-1`/`0`/`1`、
  混合型は `nil` (Ruby 互換)。`is_binop` / `alloc_binop` に追加。
- **`String#*` / `Array#*`** (`baruby_str_repeat` / `baruby_ary_repeat`)。
  `node_mul` を type branch に拡張。負の N は空。
- **`<<`** (`node_lshift`)。Int+Int は bit shift、Array は push、
  String は in-place append (`baruby_str_append`)。`is_binop` /
  `alloc_binop` に追加。`a << x << y << z` が左結合チェインで動く。
- **`p` の inspect 表示**。`baruby_print_value` / `to_s_inner` の String
  分岐で `\n` / `\t` / `\r` / `\\` / `\"` / `\xNN` (制御文字) を escape。
  prism の `unescaped` 経由のリテラル (`"a\nb"` 等) が
  正しく確認できるようになった (見た目は Ruby の `p` と同じ)。

### B — モード検証

- **AOT (`-c`)** 全 5 テスト + 3 bench 通過、plain と出力一致。新ノード
  (`node_str_lit` の `const char *` operand、`node_call_*`、`<=>` 等)
  も `code_store/SD_<hash>.c` 内で `EVAL_<name>(...)` 形に展開される。
  test_p1b のような複雑な script で SD は 1 ファイル内 inline 静的
  関数 ~400 個、public エントリ 4-5 個。
- **PG (`-p`)** も同様に通過。`PGSD_<hopt>.c` が出る。bench 結果は
  perf.md §2 に追記。
- **JIT (`-j`)** は `lstation.rb` ワーカーなしでは UDS 接続できないので
  パーサで `-j` 受信時に明示エラー + exit(1) させた。`astro_jit.c` の
  hooks は再有効化に備えて残置。

### モード別ベンチ結果 (perf.md §2 抜粋)

| bench         | plain  | aot    | pg     | aot 比 |
|---|---:|---:|---:|---:|
| binary_trees  | 0.96 s | 0.64 s | 0.94 s | 1.51× |
| list_alloc    | 1.16 s | 0.51 s | 0.50 s | 2.27× |
| string_concat | 1.02 s | 0.88 s | 0.88 s | 1.16× |

PG が plain と差が出にくい bench (binary_trees) は 1 回ループで
終わる構造 — prof-driven inlining 余地が小さい。alloc 量は libgc
の `GC_get_total_bytes` 由来で、モード間で不変 (~320MB / ~764MB /
~1.1GB)。

## 2026-05-10 — P1 言語拡張バッチ

`true` / `false` / `nil` リテラル、`to_s` / `to_i`、String 順序比較、
String / Array slice (2-arg `[]`)、文字列 interpolation を一気に入れた。

- **VAL_NIL を VAL_FALSE から分離** (raw 4 singleton)。`IS_FALSY` /
  `IS_TRUTHY` macro 追加、`node_if` / `node_while` を `IS_TRUTHY` 経由に
  書き換え (raw 4 は C 上 truthy なのでプレーン `if` だと nil が
  truthy 扱いになるバグを回避)。`IS_PTR` から VAL_NIL を除外。
- **`node_nil` ノード追加**。parser で PM_TRUE_NODE / PM_FALSE_NODE /
  PM_NIL_NODE を `node_true` / `node_false` / `node_nil` に流す
  (これまで全部 `unsupported` で死んでいた)。
- 既存の「nil 相当」フォールバック (if 無 else / 空 parens / 範囲外
  read / pop empty / aset auto-extend) を `VAL_FALSE` から `VAL_NIL` に
  切り替え。
- **`node_call_to_s` / `node_call_to_i`**。`baruby_to_s(v)` を node.c に
  追加 (libgc-backed StrBuf builder で配列の inspect 風文字列を組む。
  `open_memstream` + libc free は `free` macro shadow と相性が悪く
  leak 化するので使わない)。`p` 出力の inspect 表示と to_s top-level
  の string-without-quotes / nil→"" を分けて実装。
- **String 順序比較**。`baruby_str_cmp` を node.c に追加、`node_lt` /
  `node_le` / `node_gt` / `node_ge` を Int+Int / Str+Str の type branch
  に拡張。
- **`node_call_aget2`** (recv, idx, count)。String / Array 両方で
  サブスライス。clamp と negative index 込み。parser で
  `[]` の args_cnt==2 を分岐。
- **`PM_INTERPOLATED_STRING_NODE`**。parts 列を walk して、PM_STRING_NODE
  はそのまま、それ以外は `node_call_to_s` で wrap、左結合の `node_add`
  で連結。Empty parts は `""` 相当。`PM_EMBEDDED_STATEMENTS_NODE` も
  実装 (内側 statements を recurse、空 `#{}` は nil)。

検証は `test_p1.ba.rb` で全項目 (43 行)。fib / test_ary / test_eq の
regression なし、bench の alloc/GC も不変。

## 2026-05-10 — Ruby っぽい value semantics

`String#==` / `Array#==` / `Array#+` を実装、`true` / `false` を表示
できるよう singleton を分離。

- `baruby_value_eq(VALUE, VALUE)` を `node.c` に追加。raw 等価で
  fixnum / singleton / ポインタ identity を一発カバーし、違うときだけ
  String の byte 比較 / Array の再帰的要素比較に降りる。
- `node_eq` / `node_neq` を 2 段 fast path + helper に書き換え。
  int loop の hot path (`l == r` 直撃) は同じ命令数のまま。
- `node_add` の type branch に Array+Array (`baruby_ary_plus` で新配列
  を返す concat) を追加。
- `VAL_TRUE` を `INT2VAL(1) = 3` から **独立 singleton (raw 2)** に
  変更。`p (1 == 1)` が `1` ではなく `true` と表示されるようにし、
  `nil`/`false` と `true` が分かれるよう将来分離 ([todo.md](todo.md))
  への足場も用意。
- `IS_PTR` から `VAL_TRUE` を除外。`baruby_print_value` で `true` 表示
  対応。
- `PM_PARENTHESES_NODE` を実装 (空 `()` は `false`、それ以外は body を
  そのまま透過)。`(...)` を含む式が parser に通るようになった。

検証は `test_eq.ba.rb` で:
- 整数値比較 / mixed-type / String value-eq / Array value-eq
  (空・ネスト含む) / Array+Array (空配列・チェイン込み)。
- 既存テストの fib (10946) と test_ary も regression なし。
- 3 ベンチの alloc/GC 数は不変、wall は noise レンジ内。

## 2026-05-10 — 初期フォーク

`sample/naruby` から `sample/baruby` を切り出し、Array + String + libgc
を導入。GC testbed として独り立ちさせた。

### 言語面

- naruby の int64-only から **LSB-tagged VALUE** に拡張 (1 = fixnum、
  0 = ptr、raw 0 = false/nil)。
- ヒープ型 **Array (BaArray)** と **String (BaString)** を追加。
  共通 `ObjectHeader` に type tag。
- 比較 / `&&` / `||` を `VAL_TRUE` / `VAL_FALSE` 正規化に変更。
  既存の `&&` 実装が `node_num(0)` (= INT2VAL(0) = raw 1, truthy) を
  false 相当として使っていた潜在バグを修正。
- 専用ノード `node_true` / `node_false` 追加。

### ノード追加

- `node_ary_new` / `node_ary_push` — リテラル評価のチェイン展開用。
- `node_str_lit(const char *, uint32_t)` — eval 毎に fresh alloc。
- メソッド desugar 用 dispatch nodes:
  `node_call_size`, `node_call_aget`, `node_call_aset`,
  `node_call_push`, `node_call_pop`。型タグで Array/String を branch。

### パーサ

`PM_ARRAY_NODE` / `PM_STRING_NODE` の "unsupported" stub を実装に置換。
`PM_CALL_NODE` で receiver が non-NULL かつメソッド名が builtin 表に
ある場合は対応する dispatch ノードに lower。
`PM_OR_NODE` も実装 (`PM_AND_NODE` と同型)。

### 値表現と既存ノードの調整

- `node_num`: `INT2VAL(num)` で wrap。
- `node_add`/`sub`/`mul`/`div`/`mod`: untag → op → tag。`node_add` のみ
  string concat (`baruby_str_concat`) も runtime branch で受け持つ。
- `node_lt`/`le`/`gt`/`ge`/`eq`/`neq`: tagged 値のまま signed 比較
  (untag 不要)、結果を `VAL_TRUE`/`VAL_FALSE` に正規化。

### libgc 統合

- `context.h` で全 system header の後ろに `malloc` / `calloc` /
  `realloc` / `strdup` / `free` を `GC_*` macro で wrap (asom と同じ
  パターン)。
- `main.c` 冒頭で `GC_INIT()`。
- Makefile の link line に `-lgc`。
- `BARUBY_GC_STATS=1` で `__GC_STATS__` 行を出力 (alloc_bytes /
  heap_bytes / gc_count、libgc の `GC_get_*` 由来)。

### ベンチ

`bench/binary_trees.ba.rb` (depth 21、~1s)、`bench/list_alloc.ba.rb`
(10M iter、~1s)、`bench/string_concat.ba.rb` (5M iter、~1s)。
ランナー `bench/run.rb` が plain/aot/pg を選んで全 bench を順に実行、
時間 + GC 統計を表示。`make bench` でも一発実行可。

### 動作確認 (`--plain` のみ)

- `test.ba.rb` (fib 20) で再帰 + 整数演算 OK (10946)。
- `test_ary.ba.rb` で配列 / 文字列 / index / size / push / pop /
  concat の挙動が期待通り。
- 3 ベンチがすべて完走、時間が ~1s スケールで GC が走っていることを
  確認 (12〜1700 collections)。

AOT / PG / JIT モードでの新ノード動作は未検証 ([todo.md](todo.md) P0)。

### 削除した naruby 資産

- `naruby_codegen.rb` (本人コメントで obsolete)
- `naruby_code.c` (生成済み AST のテストダンプ)
- `lstation.rb` (JIT サーバ — `-j` 自体を unwired にした)

## 過去の経緯

baruby 命名: naruby = "**n**ot **a** ruby"、abruby = "**a b**it ruby"
の中間 — "**ba**rely a ruby" → baruby。
