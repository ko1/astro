# baruby_precise TODO

baruby_precise は precise *moving* (semi-space) GC の testbed。 仕様は
[spec.md](spec.md) (baruby と同じ)、 実装は [runtime.md](runtime.md)、
ベンチは [perf.md](perf.md)、 完了履歴は [done.md](done.md)。 設計の経緯は
[`docs/gc_design.md`](../../../docs/gc_design.md)。

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
- [ ] **AOT mode の再検証** — iter 35 で確認: `-c` で実行すると
      `astro_cs_build: make failed (exit 512)` で abort、 interpreter
      fallback で結果は正しく出る。 code_store/Makefile が `-I../../runtime`
      を含まず `astro_debug.h` を発見できないのが root cause (`-fno-plt` 等
      は入っているが include path は空)。 `astro_cs_build()` で
      `extra_cflags` 経由で `-I` を渡す必要がある (sample 側責任)。
      moving GC との SD bake compat (`(c, n, fp, sp)` 4 引数) audit は
      未着手 — まず make fail を直してから。
- [x] ~~**AOT mode の再検証**~~ — iter 36 で修復済 ([done.md](done.md) (36) 参照)。
      Makefile に `-DBARUBY_PRECISE_DIR=` 等の絶対パス macro を追加し、
      main.c::common_build_flags_and_link で extra_cflags 経由で `-I` を
      渡すように。 また `astro_cs_init(...)` の version 引数に `BARUBY_GC`
      を渡して backend 切替時の code_store invalidation を効かせる。
      matrix.rb は AOT/PG モードで bench ごとに `code_store/` を clean
      する (異 bench の SD pollution で fib_pair が 0.5 → 1.0s に劣化する
      問題を回避)。 iter 36 fair-AOT 結果は bench-results/aot/matrix.md。

- [x] ~~**Remset サイズ上限なし**~~ — iter 36 解決。 全 7 gen backend で
      `MAX_REMSET=128K` cap、 5 backend は heap-walk fallback、 残 2 は
      明示 abort。 加えて `mark_card_gen` (#15) を page-level remset で
      bounded 化。
- [x] ~~**配列リテラルの 1-shot 化**~~ — iter 36-final 解決済
      ([done.md](done.md) iter (36-final) 参照)。 `node_ary_lit_{1..4}` を
      node.def に追加、 parser dispatch。 plain で -9〜-12% (fib_pair /
      gc_combined / list_alloc / interp_calc)、 AOT で -16〜-37% (immix_gen)。
      baruby (libgc) にも port 済。 1 度 plain で regression と誤判定したが
      AOT で profiling し直して真因 (dispatch が plain で 50%) を特定、
      clean rebuild で win 確認。
- [ ] **文字列 `+` chain の 1-shot 化** — 配列の手法をそのまま適用できそう
      だが、 parser が `add(add(s1,s2),s3)` を `strcat_K([s1,s2,s3])` に
      畳むには pm_call_node 構造の認識が要る (PM_ARRAY_NODE と違って
      operator 名と arity を見て判定)。 string_concat / substr_churn で
      win 期待。 ary_lit と同じ AOT-friendly な構造で実装可能。
- [ ] **inc 系 backend を真の incremental に**: VALUE stack write barrier を
      追加して、 SATB + stack-WB の組合せで mutator-与 alloc を細かく
      分割。 現状は infra のみ用意 (`mark_gen_inc` / `copy_gen_inc`) で
      実体は STW major

## P1 — 性能

- [ ] **callee frame の zero-init コストを減らす** — `node_call_<N>` で
      毎 call 時に `for (i < locals_cnt) sp[i] = 0` が走る。 parser が
      「全 local が即書きされる」 を保証できれば skip 可能
- [ ] **string_concat の残存 +20% overhead** を perf record で内訳分析。
      spill / sp 更新 / copy のどれが bottle neck か確かめる
- [ ] **REGION_BYTES の adaptive 化** — 現在 512 MiB 固定。 live set に
      合わせて grow させたい
- [ ] **世代別 GC backend** — `gc_combined` (長寿命 + 短寿命チャーン) で
      明らかに効くはず。 同 interface に乗せる

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

## 動的成長の更なる改善 (低優先)

(27) で region cap を 64 GiB virtual reservation に置き換えたが、 これ
自体も上限なので「真の dynamic growth (linked chunks)」 で完全撤廃する案:

- moving backend (copy / copy_gen 等) で from-space を linked chunk list
  にして、 chunk 単位で `mremap` または追加 mmap で成長
- 64 GiB を超える heap が必要な workload で意味がある (現状の bench では
  全て 1 GiB 以下、 64 GiB は実質的に無制限)

## メンテ

- [ ] `bench/run.rb` を precise / conservative 両対応にして、 表で並べて
      出す (現在は手動で各サンプル動かしている)
- [ ] CRuby の参考時間と並べる (binary_trees / list_alloc / string_concat
      同等を CRuby で動かす)
