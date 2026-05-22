# baruby_precise TODO

baruby_precise は precise *moving* (semi-space) GC の testbed。 仕様は
[spec.md](spec.md) (baruby と同じ)、 実装は [runtime.md](runtime.md)、
ベンチは [perf.md](perf.md)、 完了履歴は [done.md](done.md)。 設計の経緯は
[`docs/gc_design.md`](../../../docs/gc_design.md)。

## 直近 (iter 61 状態)

- [x] ~~**fp 引数完全削除 (dispatcher signature を 3-arg 化)**~~ — 2026-05-21
      解決 ([done.md (61)](done.md))。 lget/lset.sp_offset と
      call/call2/call_static.callee_fp_offset を parse-end walker で bake。
      AOT で **prime_count -89% / call -52% / chain20 -53% / fib -39%** 等
      大幅 win。 15 backend × 35 bench × `-n 3` で plain + AOT 両方 oracle pass。
- [ ] **walker の framework 化** — `baruby_parse.c::walk_bake_sp_offset` は
      hand-write で全 NODE kind を列挙 (~200 行)。 HASH 系と同じ仕組みで
      astrogen.rb に per-kind child-walk callback の gen task を追加して
      自動生成に畳む。 lget/lset/call の operand bake は special-case
      として残す。

      **iter 71 で前提条件が整った** — call_N args の `chain +=
      callee_locals_cnt` 特殊扱いを削除済 (args を @child 化したため
      generic chain += slot_count で済む)。 walker は完全に
      `child_chain = chain + slot_count` の structural recursion +
      lget/lset/call 系の operand bake のみとなり、 ASTroGen の
      per-kind child enumerator + special-case visitor の組合せで
      自動生成可能。 iter 70 で試行した auto-gen (commit `54779351`
      → revert `fbaaccda`) は callee_locals_cnt 依存が framework に
      漏れて失敗したが、 iter 71 で問題が消えたので再挑戦できる。
- [x] ~~**copy 系 backend に large_alloc 経路を追加**~~ — iter 66 で gc_copy
      (commit `08dd67ad` + scan-loop fast-path 分離 `f3680742`) と
      gc_mark_compact (commit `4c9a017d`) に実装。 LARGE_THRESHOLD=4096 B
      以上は malloc 別領域 (non-moving) + linked list。 glibc が ≥128 KiB
      chunk を mmap-backed にするので free → munmap で物理メモリ即解放。
      gc_copy で sieve -8% / hash_chain -9% / 6 bench で win (geomean -2.6%)、
      gc_mark_compact で hash_chain -27% / list_alloc -6% (geomean -5%)。
      details: perf.md §4.5。 mark_compact / mark_bump_gen / mark_compact_gen
      / immix の region-bump 系へ展開する余地は残るが、 gen backend は
      pretenure 経路で既に large が tenured 直行するため win 限定的の見込み。

      **完了: resize 経路 realloc(3) 化 (iter 67-69)** — `aro_gc_realloc_in_place`
      hook を gc.h に追加、 gc_common.c に weak default、 10 backend に
      strong override を移植:
      - malloc-backed LargeObj (gc_copy / gc_mark_compact): **realloc(3)** で
        in-place、 mmap-backed chunk なら mremap が自動的に
      - mmap-backed LargeObj non-gen (gc_mark / gc_mark_freelist / gc_immix):
        **mremap MREMAP_MAYMOVE** で移動も許容 (forward_payload が新 addr を見る)
      - mmap-backed LargeObj gen (gc_mark_gen / gc_mark_gen_inc /
        gc_mark_bitmap_gen / gc_mark_card_gen / gc_immix_gen):
        **mremap (no MAYMOVE)**、 young_objs / remset の stale ptr を回避

      perf 効果 (median of 3-5、 sieve):
      | backend           | 前 → 後                       | Δ |
      |-------------------|-------------------------------|----|
      | gc_copy           | 1.186 → 1.050                 | **-11.5%** |
      | gc_mark_compact   | 1.138 → 1.067                 | -6.2% |
      | gc_mark           | 1.094 → 1.020                 | -6.8% |
      | gc_mark_freelist  | 1.116 → 1.018                 | -8.8% |
      | gc_immix          | 1.173 → 1.016                 | **-13.4%** |
      | gc_mark_gen       | 1.428 → 1.122                 | **-21%** |
      | gc_mark_gen_inc / mark_bitmap_gen / mark_card_gen / immix_gen | (移植のみ、 perf 影響は workload 依存) |

      iter 67: `4aa6f36b`, `d2b07ddf` (malloc 版)
      iter 68: `93ab6d59` (mark mremap)
      iter 69: `f6f88a59`, `cd4b77ea`, `173b94a5`, `eae4ceee` (残 7 backend)

      未適用 backend: gc_copy_gen / gc_copy_gen_inc / gc_mark_compact_gen /
      gc_mark_bump_gen / gc_bump / gc_none。 gen の bump tenured 系は
      pretenure 経路 + tenured bump ptr 管理の変更が必要なので別 design 検討。

## 直近 (iter 59 状態)

- [x] ~~**AOT loader が非 gen backend × array-write bench で silently 壊れていた**~~ —
      `aro_gc_wb` undefined symbol で `dlopen` 失敗 → `all_handle=NIL` → 全
      SD load skip。 `node.h` に `#include "gc.h"` 追加で修正。
      非 gen GC × aset-using bench (hash_chain / fannkuch / sieve / json_parse /
      remset_pressure / tokenize / dll_walk) で 4-7× の AOT 加速。
      詳細 [done.md (59)](done.md)。 副次対策として `astro_cs_reload` の
      dlopen 失敗時に dlerror を stderr 出力するようにした (silent fail 防止)。

## 直近 (iter 58 直後の状態)

- [調査済] **cons_list libgc +9% 退化** — iter 59 で perf record 実施。
      hot 関数 top: GC_malloc_kind 10.80%、 DISPATCH_node_seq 10.68%、
      DISPATCH_node_lset 9.49%、 DISPATCH_node_lt 8.83%、
      DISPATCH_node_call_aget 8.70%、 DISPATCH_node_add 7.92%。
      `[i, list]` per iter = 2 × `GC_malloc` (BaArray 24B + items 16B)
      で 20M malloc/run。 **libgc native alloc が支配的** で @child 由来の
      コードシェイプ変化ではない。 dispatcher 1 ロード余分の影響あるが
      <1% 程度。 **構造的問題なので別アプローチ要**:
      (a) BaArray の embed items (header 内に 2 slot 持つ; iter 56 で
      一度 revert したが target 限定で再挑戦の価値あり),
      (b) bdwgc の typed_alloc / kind hint, (c) cons_list 用 small-cell
      pool。
- [ ] **bench/life.ba.rb を baruby (libgc) にも copy** — 修正後 baruby
      でも動くので bench 集合に追加してよい。 iter 58 では一時 copy
      して動作確認したが、 後で削除した。
- [x] ~~**callee_sp safety guard を撤去できるか検証**~~ — iter 59 (B1)
      で撤去済。 `8d0912b4 sample/baruby_precise: remove callee_sp safety
      guard (B1)` commit 参照。 19 oracle 全合格、 perf 影響なし。

## P0

- [x] ~~**uninitialized sp scratch slot in GC scan range**~~ — 2026-05-16 解決
      ([done.md](done.md) (8) 参照)。 真因は `baruby_gc_realloc_payload`
      の memcpy-to-buf-before-alloc。 副次的に node.def の EVAL_ARG 新 sp_top
      も「初期化済みスロットのみ scan」 になるよう sp+i 段階指定に修正済。
- [x] ~~**parser bug: `binop + call(>3 args)` でオペランド競合**~~ —
      2026-05-16 (12) 解決。 `alloc_binop` 入口で `arg_index` を 4 slot
      bump して transduce、 後で rewind。 詳細 [done.md](done.md) (12) 参照。
- [x] ~~**gc_mark_compact_gen: leading-minor が tenured を溢れる
      assertion バグ**~~ — 2026-05-17 (23) 解決。 `defer_fold` で
      mark+compact を先行させ、 trailing minor で nursery を fold する
      経路を追加 ([done.md](done.md) (23) 参照)。 release build での
      silent corruption 防止のため `forward_obj` の assert も「clean
      abort + 内訳 print」 に差替え。
- [x] ~~**toplevel sp の hardcode 64**~~ — 2026-05-18 (27) 解決。
      `aro_toplevel_locals_cnt` を `baruby_parse.c::PM_PROGRAM_NODE` で
      `tc->frame->max_cnt` から設定、 `main()` で `c->sp = c->env +
      aro_toplevel_locals_cnt` する。 100 toplevel locals の test program で動作確認。
- [x] ~~**AOT mode の再検証**~~ — iter 36 で修復済 ([done.md](done.md) (36) 参照)。
      Makefile に `-DBARUBY_PRECISE_DIR=` 等の絶対パス macro を追加し、
      main.c::common_build_flags_and_link で extra_cflags 経由で `-I` を
      渡すように。 また `astro_cs_init(...)` の version 引数に `BARUBY_GC`
      を渡して backend 切替時の code_store invalidation を効かせる。
      matrix.rb は AOT/PG モードで bench ごとに `code_store/` を clean
      する (異 bench の SD pollution で fib_pair が 0.5 → 1.0s に劣化する
      問題を回避)。 iter 36 fair-AOT 結果は bench-results/aot/matrix.md。

- [x] ~~**Remset サイズ上限なし**~~ — iter 36 半解決、 iter 38 完結。 全 7
      gen backend で `MAX_REMSET=128K` cap、 **全 backend で heap-walk fallback**。
      `mark_card_gen` (#15) は page-level remset で root bounded。 iter 38 で
      `immix_gen` (`tenured_objs[]` 経由) と `mark_bitmap_gen` (per-page
      `dirty_bm` 走査) の 2 backend に fallback を追加し abort を撤去。
      これで 8 gen backend 全て bounded correctness。
- [x] ~~**配列リテラルの 1-shot 化**~~ — iter 36-final 解決済
      ([done.md](done.md) iter (36-final) 参照)。 `node_ary_lit_{1..4}` を
      node.def に追加、 parser dispatch。 plain で -9〜-12% (fib_pair /
      gc_combined / list_alloc / interp_calc)、 AOT で -16〜-37% (immix_gen)。
      baruby (libgc) にも port 済。 1 度 plain で regression と誤判定したが
      AOT で profiling し直して真因 (dispatch が plain で 50%) を特定、
      clean rebuild で win 確認。
- [x] ~~**文字列リテラル `+` の parse-time fold**~~ — iter 37 解決
      ([done.md](done.md) iter (37) 参照)。 `alloc_binop` で
      `node_str_lit + node_str_lit` を 1 つの `node_str_lit` に縮約。
      plain で string_concat -58% (immix_gen 0.48→0.20)、 AOT で -79%
      (0.34→0.07)。 本来の dynamic concat pattern を保存する
      `string_concat_dyn.ba.rb` も追加 (5_000_000 iter, oracle=45000000)。
      baruby (libgc) にも port 済。
- [ ] **動的 string `+` chain の alloc-fusion** — `s1 + s2 + s3` で
      s1/s2/s3 が変数のケースの中間 BaString + bytes 1 個削減を狙う。
      iter 51 で AST 側 `node_add3` 試したが棄却 (done.md (52) 参照)。
      AST node を増やす方向は禁止。 代替アプローチ:
      (a) SSO で小 string を 1 alloc に縮める (下の SSO 項目)
      (b) rope / cord 構造で BaString concat 自体を amortize
      (c) `aro_gc_alloc` への `__attribute__((malloc, alloc_size))`
          付与で gcc の alloc merging を *理論上* 起こす — 期待薄
          だが軽く試す価値はあり
      実装するなら (a) → (b) の順。 (c) は単独で win は出ない見込み。
- [x] ~~**SSO (small-string optimization)**~~ — iter 53 解決
      ([done.md](done.md) iter (53) 参照)。 `SSO_MAX=7` で BaString
      24 B 維持版を実装。 immix_gen で tokenize -17%、 substr_churn -2%、
      string_concat_dyn -7% (A/B median of 5)。 baruby (libgc) にも
      port (libgc は conservative なので scan 修正不要)。
      `SSO_MAX=15` は BaString 32 B に肥大 → fib_pair で +8% regress
      が出るので採用せず。 将来 short-string が頻出する workload なら
      再検討の余地あり (build flag 化が候補)。
- [x] ~~**`aro_gc_alloc` への `__attribute__((malloc, alloc_size,
      returns_nonnull))` 付与**~~ — iter 55 で試行 → 棄却
      ([done.md](done.md) iter (55) 参照)。 immix_gen / copy_gen で
      6 bench (json_parse / fib_pair / hash_chain / string_concat_dyn /
      substr_churn / tokenize) を A/B 計測、 全て noise 範囲内
      (< 3%)。 gcc の -O3 + -flto が既に function body から同等の
      aliasing 性質を推定していると推測。 ship 価値なし。
- [x] ~~**BaArray embed (SMALL_N=2, small-vector optimization)**~~ —
      iter 56 で実装 → A/B で棄却 ([done.md](done.md) iter (56) 参照)。
      binary_trees -26%、 cons_list / json_parse / fib_pair で小 win、
      但し hash_chain / interp_calc / list_alloc / list_sort / nqueens /
      tokenize / string_concat_dyn / substr_churn など 9 bench で
      +5〜14% 回帰。 BaArray 24B→32B の size growth が embed の恩恵を
      持たない workload で広範に payment され、 geomean net negative。
- [x] ~~**BaArray inline layout (embed / CONTIG / FAM 系)**~~ —
      iter 56 embed (棄却) + iter 57 CONTIG (棄却) で BaArray inline
      化路線は exhausted ([done.md](done.md) iter (56), (57) 参照)。
      embed は size growth、 CONTIG は size 維持で同じ window のはず
      だったが、 どちらも hash_chain で +35〜71% regression を生んだ。
      原因: ASTro の現状 GC interface (OBJ + 子 payload の 2-object
      model) が mark 系 backend で前提となっており、 inline 化が
      KIND_PAYLOAD_VAL amortize pattern を壊す。 FAM 案も同じ
      structural 問題があるので同様の trade-off と推測、 採用しない。
      BaArray layout 変更 は基本ボツ方向に確定。
- [ ] **inc 系 backend を真の incremental に**: VALUE stack write barrier を
      追加して、 SATB + stack-WB の組合せで mutator-与 alloc を細かく
      分割。 現状は infra のみ用意 (`mark_gen_inc` / `copy_gen_inc`) で
      実体は STW major

## graph_bfs で gen backend が non-gen より遅い (iter 70 観察)

bench/graph_bfs.ba.rb は long-lived graph (10k node) + 短寿命 BFS
working set という generational backend に有利なはずの workload。 だが
median-of-3 で:

| Backend  | graph_bfs | alloc_bytes | heap_bytes | gc_count |
|----------|-----------|-------------|------------|----------|
| copy     | 0.91 s    | 99 MB       | 16 MB      | 5        |
| mark     | 0.89 s    | 99 MB       | 16 MB      | 5        |
| copy_gen | 0.93 s    | **190 MB**  | **190 MB** | 11 minor |
| mark_gen | 1.02 s    | **188 MB**  | 11 MB      | 11 minor |

**観察**:
- copy_gen: heap_bytes = alloc_bytes (= 全部 tenured へ promote、 major
  なし)。 dead tenured が累積、 nursery が満タンになる頻度が高い
- mark_gen: alloc_bytes が 2x なのに heap_bytes は bounded。 minor で
  unmarked young は freed されている。 が、 minor の per-obj linked
  list 操作が非 gen より重い

**仮説**:
- BFS の visited / dist / queue array は minor GC のタイミングで
  まだ live なので promote されてしまう。 graph_bfs 設計時の「短寿命
  working set」 仮定が NURSERY_BYTES (16 MiB) の制約と合わない
- nursery が小さいので各 BFS の visited (10k bool ≒ 80 KB?) + queue
  + dist で minor 1 回起きる。 そのとき BFS は走行中なので全部 live

**改善案**:
- (a) graph_bfs の n_bfs を増やして「複数 BFS の間で working set 完了」
  のターン数を増やす。 但し BFS 自体が長くなって gen の win が薄まる
- (b) BFS の working set を nursery より小さく (小 graph) → gen が
  浮く設計
- (c) nursery 経由しないで visited array を別 alloc 経路に → 設計変更
- (d) `aro_gc_promote_hint(c, payload)` API で「これは長寿命」 と
  hint を渡せる API を作る → bench 側で明示

priority 低: 現状の bench も差分が出るので有用、 ただし gen を
"showcasing" する目的なら nursery と workload の関係を見直すべき

## P1 — 性能

- [ ] **callee frame の zero-init コストを減らす** — `node_call_<N>` で
      毎 call 時に `for (i < locals_cnt) sp[i] = 0` が走る。 iter 42 で
      arg slots を skip する単純化を試みたが unsafe (sp_top = sp +
      locals_cnt のため arg eval 中の GC scan が arg slot を見る、
      stale heap pointer 危険) と判明し revert。 真に安全に減らすには
      `BARUBY_EVAL_ARG(c, ai, sp + i)` のように per-arg sp_top を adjust
      する API 変更が要る — 別 iter 候補。
- [x] ~~**string_concat の残存 +20% overhead**~~ — iter 37 const-fold
      + iter 43-45 inline 化で大幅縮小。 plain string_concat immix_gen
      0.42→0.17 (-60%)、 AOT 0.29→0.06 (-79%)。 CRuby 比で plain 2.4×、
      AOT 4.8× faster。 残存 overhead は VALUE stack rooting cost が
      ほぼ全て (precise GC の固有コスト)。
- [ ] **REGION_BYTES の linked-chunk 化** — iter 28 で 64 GiB virtual に
      拡張済だが、 64 GiB を超える heap は OOM。 `mremap` か追加 mmap で
      chunk 単位 grow。 現状の bench では 1 GiB 以下なので低優先。
- [ ] **`aro_gc_alloc_byte` の 2nd alloc inline 化** — `baruby_str_new` /
      `baruby_str_concat` / `baruby_str_slice` の bytes payload 確保
      (`aro_gc_alloc_byte(r->capa, sp)`) は変数 size のため iter 43-45
      inline 化の恩恵を受けず call で残る。 specialized fast-path や
      always_inline で攻める余地あり。

## P2 — design / framework 統合

- [ ] **`gc.c` / `gc.h` を `runtime/` に格上げ** — 現在は
      `sample/baruby_precise/gc.{c,h}`。 root mechanism (sp[] flat scan) と
      semi-space を framework backend として汎用化
- [ ] **astrogen.rb 拡張 `@locals` を試す** — `docs/gc_design.md` §1.3.2 で
      想定した sugar。 user が `@locals(l, r)` と書くと ASTroGen が
      sp slot 割当を emit する形。 手書きの error-prone (sp[] spill 忘れ
      バグ群が多数) を機械的に消せる
- [ ] **`value.def` を baruby_precise で試す** — `docs/gc_design.md` §1.7
      の任意 DSL。 marker / allocator の自動生成が abruby `node_mark.c`
      流儀でできるか
- [ ] **sp mutation で `c->sp` を常時同期** — 現状の contract は
      「can-GC helper 呼ぶ前に `c->sp = sp;` を caller が手書き」。
      1 node あたり最大 1 行で済むが、 新規 NODE 追加時の書き忘れが
      root 取りこぼし silent bug 化する。 保守的に SP_PUSH / SP_POP /
      SP_ADD / SP_SUB macro 経由で sp mutation 時に `c->sp` を
      同期すれば、 helper 呼び出し側で意識不要になる。 cost: sp 動かす
      毎に CTX への store 1 回追加。 sp mutation は node frame setup
      でしか起きないので hot inner loop には影響薄い見込み。
      実装は context.h に macro 追加 → node_eval.c / node.def の sp
      mutation を macro 化 → `c->sp = sp;` を helper 呼出点から除去
      → 256/256 PASS + perf bench 確認。

## stress mode の resource limit

`BARUBY_GC_STRESS=1` での既知制限 (correctness bug ではない):

- **`gc_copy`** — 全 minor で from-space を `PROT_NONE + MADV_DONTNEED`
  で恒久 retire。 約 65k 回 GC で `/proc/sys/vm/max_map_count` を
  使い果たして `mmap: Cannot allocate memory` で abort。 短い stress
  test では問題なし。 長 bench を回したい時は max_map_count を上げるか、
  retire の circular buffer 化が必要 (TODO)。
- **`gc_mark_bump_gen`** — tenured 側に compactor を持たないので、
  long-live old object が溜まると tenured 64 GiB virtual を使い切ると
  OOM になり得る。 これは design limit (compactor 無し)、
  `mark_compact_gen` を使えば回避可。 (旧: 1 GiB cap → 2026-05-18 (27)
  で 64 GiB virtual に拡張済)

## メンテ

- [ ] `bench/run.rb` を precise / conservative 両対応にして、 表で並べて
      出す (現在は手動で各サンプル動かしている)
- [x] ~~**CRuby の参考時間と並べる**~~ — iter 42 解決。 18 bench 全てを
      CRuby 3.4 で median-of-3 計測、 perf.md §2 末尾に baruby との
      倍率付き表を追加。 plain で geomean 1.83×、 AOT で 7.77×。
      最大 list_sort AOT で 34.6×。 plain で唯一 CRuby に負けるのは
      life (0.95×) と nqueens (0.92×)。
