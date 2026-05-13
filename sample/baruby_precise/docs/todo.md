# baruby_precise TODO

baruby_precise は precise mark&sweep の MVP 試作。 仕様は [spec.md](spec.md)
(baruby と同じ)、 実装は [runtime.md](runtime.md)、 ベンチは [perf.md](perf.md)。
設計の経緯は [`docs/gc_design.md`](../../../docs/gc_design.md)。

## P0 — 既知バグ

- [ ] **`bench/binary_trees` の計算結果が壊れる** (期待 4194303 → 実際 1)。
      arithmetic node (`node_add` 等) や method dispatch node で heap-VALUE
      operand の root spill が抜けている可能性が高い。 例えば node_add:
      ```c
      VALUE l = EVAL_ARG(c, lv);   // ← l が String/Array なら、 rv 評価で
      VALUE r = EVAL_ARG(c, rv);   //    alloc が走ると l が回収される
      ```
      これを sp[0] に spill する書き換えが必要。 node.def の sed では sig
      しか更新していない (BODY は手付かず)
- [ ] **toplevel sp の hardcode 64** (`main.c::create_context`)。 大きな
      toplevel フレームを持つプログラムでは scratch 領域が不足する。
      parser から toplevel locals_cnt を取って計算するべき

## P1 — 性能

- [ ] **callee frame の zero-init コストを減らす** — `node_call_<N>` で
      毎 call 時に `for (i < locals_cnt) sp[i] = 0` が走る。 parser が
      「全 local が即書きされる」 を保証できれば skip 可能
- [ ] **realloc の old/new size 差分追跡** — `baruby_gc_realloc_payload`
      が現状 new_size を total に加算するだけ。 `heap_bytes` が underflow
      する原因。 threshold 制御も精度が落ちる
- [ ] **string_concat の +32〜48% overhead** を perf record で内訳分析。
      spill / sp 更新 / sweep のどれが bottle neck か確かめる
- [ ] **GC threshold の adaptive 化** — 現在 4 MiB 固定。 live set サイズに
      応じて伸縮させたい (要 5.1 の old/new tracking)

## P2 — design / framework 統合

- [ ] **arithmetic / comparison node の rooting を全部見直す** — 上記
      binary_trees バグの根本対応。 node.def を読み返して `EVAL_ARG` で
      heap-VALUE を保持する箇所すべてに sp[] spill を入れる
- [ ] **`gc.c` / `gc.h` を `runtime/` に格上げ** — 現在は
      `sample/baruby_precise/gc.{c,h}`。 root mechanism (sp[] flat scan) と
      object link list は backend として汎用化できそう
- [ ] **astrogen.rb 拡張 `@locals` を試す** — `docs/gc_design.md` §1.3.2 で
      想定した sugar。 user が `@locals(l, r)` と書くと ASTroGen が
      `#define l sp[sp_cnt + 0]; #define r sp[sp_cnt + 1]` を emit する形。
      手書きの error-prone (binary_trees バグの原因) を減らせる
- [ ] **moving backend (semi-space) を同 interface に乗せる** — sp[] root
      列挙が moving と整合するか、 reload pattern が機能するか確認
- [ ] **`value.def` を baruby_precise で試す** — `docs/gc_design.md` §1.7
      の任意 DSL。 marker / allocator の自動生成が abruby `node_mark.c`
      流儀でできるか

## メンテ

- [ ] `bench/run.rb` を precise / conservative 両対応にして、 表で並べて
      出す (現在は手動で各サンプル動かしている)
- [ ] CRuby の参考時間と並べて (binary_trees / list_alloc / string_concat
      同等を CRuby で動かす)
