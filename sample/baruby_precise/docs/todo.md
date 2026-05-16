# baruby_precise TODO

baruby_precise は precise *moving* (semi-space) GC の testbed。 仕様は
[spec.md](spec.md) (baruby と同じ)、 実装は [runtime.md](runtime.md)、
ベンチは [perf.md](perf.md)、 完了履歴は [done.md](done.md)。 設計の経緯は
[`docs/gc_design.md`](../../../docs/gc_design.md)。

## P0

- [ ] **uninitialized sp scratch slot in GC scan range** — `bench/hash_chain.ba.rb`
      で `copy_gen` / `copy_gen_inc` / `mark_compact_gen` が落ちる。
      原因: `node_call_push` (および類似ノード) は val 引数評価時に
      `sp + 2` を新 sp_top として渡すが、 そのとき `sp[1]` (val スロット)
      は未初期化のまま GC scan 範囲に入る。 過去フレームの leftover
      nursery ptr が残っていると forward_obj が stale ヘッダを follow
      して to-tenured に corrupt copy → `process_object: unknown kind`
      で abort。 minor 既存の高水位 zeroing (sp_top retreat 時の追従) は
      retreat 経路のみで救えず、 sp_top が高い状態で uninit slot を
      拾うケースは未保護。
      対策案:
      - (a) val eval を `sp + 1` で呼ぶ (scan を sp[0] のみに絞り、
        sp[1] は eval 戻り値で上書きされるので scan 不要)
      - (b) val eval 直前に `sp[1] = 0` で zeroize
      - aget も recv eval 前に `sp[0]` 未初期化のまま `sp + 1` を渡す
        対称な穴がある。 ノード全種を審査して fix。
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

## メンテ

- [ ] `bench/run.rb` を precise / conservative 両対応にして、 表で並べて
      出す (現在は手動で各サンプル動かしている)
- [ ] CRuby の参考時間と並べる (binary_trees / list_alloc / string_concat
      同等を CRuby で動かす)
