# ascheme → precise rooting migration

ascheme は元々 Boehm-Demers-Weiser conservative GC (`libgc`) で書かれた
R5RS Scheme 実装。 sample/ascheme_precise は、 ASTro precise GC framework
(`runtime/precise_gc/`) への migration を目指す fork。

migration が完了すれば、 baruby_precise が 17 GC backend を build-time
切替できるのと同様、 ascheme_precise も `make GC=copy_scramble` 等で
mark/move 漏れ audit できるようになる。

## migration status

**現状: scaffold only**。 ディレクトリは ascheme のコピー、 binary 名は
`ascheme_precise` に rename 済。 中身は **未だ libgc 依存**。

## migration plan (= 多段階)

### Phase 1: 構造改修 (1-2 日)
1. `struct sobj` の先頭に `ASTroObjectHeader head` を追加 (= layout 変更)
2. ascheme の `int type` を `head.flags` の低 5 bits に詰める
3. **GC_malloc → aro_gc_alloc(c, size)** に全 46 site で置換
4. CTX 経由でない alloc (= module init 時等) は static fallback を用意

### Phase 2: precise root tracking (2-3 日)
5. `struct CTX_struct` に `VALUE *env, *sp` 追加 (= baruby_precise 同様)
6. 各 NODE evaluator が intermediate VALUE を sp[] に park
7. `aro_gc_alloc` の `c->sp` contract を守るため全 alloc 経路で sp 設定
8. `struct sframe` (= lexical env) の scan 経路を定義 (= 親 chain
   walking + slots[])

### Phase 3: SCAN_EDGES (1-2 日)
9. 各 object 型 (cons / vector / string / closure / continuation /
   promise / port / port-input / port-output / bignum / rational / ...)
   ごとに SCAN_EDGES dispatch を実装

### Phase 4: call/cc 対応 (~2 日, 最難所)
10. call/cc は現在 setjmp/longjmp + C stack snapshot を使う
11. precise rooting と相性悪 → 別実装が必要:
    - One-shot escaping continuation のみ → sp[] snapshot + longjmp で OK
    - Full multi-shot → moving GC との整合性で要設計

### Phase 5: GMP integration (1 日)
12. `mp_set_memory_functions` で GMP に渡す allocator を変更
13. moving GC とは相性悪い (= GMP は外部に raw ptr を保持) → bignum 用に
    non-moving sub-heap を確保するか、 GMP-side alloc は普通の malloc に
    戻す (= leak 受容)

### Phase 6: testing (1-2 日)
14. ascheme 既存の 179 R5RS tests pass まで
15. backend matrix で動作確認 (= 全 17 backend × bench × test)

**合計見積**: 1-2 週間 of focused work。

## 参考

- `sample/baruby_precise/` — Ruby サブセットの precise rooting 実装。
  ascheme_precise の reference となる。
- `docs/gc_design.md` — precise GC framework の設計仕様。
- `runtime/precise_gc/gc_copy_scramble.c` — audit backend。 migration
  途中で alloc 漏れ / decode 漏れの検出に活用予定。
