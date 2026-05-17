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
- [ ] **toplevel sp の hardcode 64** (`main.c::create_context`)。 大きな
      toplevel フレームを持つプログラムでは scratch 領域が不足する。
      parser から toplevel locals_cnt を取って計算するべき
- [ ] **AOT mode の再検証** — moving GC 移行後 `-c` 経路が回るか未確認。
      SD bake された body が `(c, n, fp, sp)` 4 引数で precise rooting を
      正しく行えているか audit
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
  long-live old object が溜まると tenured OOM。 `string_concat` のような
  promotion-heavy bench を stress で回すと 1 GiB tenured を使い切る。
  これは design limit (compactor 無し)、 `mark_compact_gen` を使えば回避可。

## メンテ

- [ ] `bench/run.rb` を precise / conservative 両対応にして、 表で並べて
      出す (現在は手動で各サンプル動かしている)
- [ ] CRuby の参考時間と並べる (binary_trees / list_alloc / string_concat
      同等を CRuby で動かす)
