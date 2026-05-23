# ascheme → precise rooting migration

ascheme は元々 Boehm-Demers-Weiser conservative GC (`libgc`) で書かれた
R5RS Scheme 実装。 sample/ascheme_precise は、 ASTro precise GC framework
(`runtime/precise_gc/`) への migration を目指す fork。

migration が完了すれば、 baruby_precise が 17 GC backend を build-time
切替できるのと同様、 ascheme_precise も `make GC=copy_scramble` 等で
mark/move 漏れ audit できるようになる。

## migration status

**現状: Phase 1-4 完了**。 全 17 GC backend (= non-moving 10 種 + moving 7
種) で 16/16 ascheme test + 179/179 R5RS chibi test PASS。 binary 名は
`ascheme_precise`、 `make GC=<backend>` で切替。

Phase 4 の moving backend 対応で発覚した主な fix:

- **scm_global_define の v parking**: v は C-local だったので name buf
  alloc が GC trigger すると stale 化。 globals[i] slot に value を pre-
  set してから name alloc。 gentry.name は interior pointer (= payload
  base + header) だったので、 base を保存する `name_payload` に変更
  + `GENTRY_NAME()` accessor 経由で読む。
- **OBJ_SYMBOL / OBJ_STRING の interior char slot**: `sym.name` /
  `str.chars` は byte payload の `raw + sizeof(header)` を保持する形に
  なっていて、 moving GC 後に stale 化する。 SCAN_EDGES で base を
  visit して re-derive する `ASCHEME_VISIT_INTERIOR_CHAR_SLOT`
  ヘルパを追加。
- **OBJ_VECTOR / OBJ_MVALUES の items[]**: `aro_gc_alloc(sizeof(VALUE)*N)`
  で items を確保していたが、 framework header が items[0..1] に被って
  user write で gc_size が壊れる。 `sizeof(header) + sizeof(VALUE)*N` を
  alloc して、 items = raw + sizeof(header) に。 SCAN_EDGES も interior
  pointer ハンドリング。
- **scm_intern の o parking**: o = scm_alloc → aro_gc_alloc_byte の間で
  GC が発火すると o が stale。 SYMBOL_TABLE slot に pre-set してから
  name buf alloc + reload。 `o->sym.name = NULL` で SCAN_EDGES の
  interior visit を no-op に。
- **scm_make_string / scm_make_string_n / scm_make_vector /
  scm_make_mvalues**: 同様に c->sp[0] に sobj を park して inner
  alloc 後に reload。
- **scm_callcc の C-local rooting**: `saved_env`, `saved_tcp`, `k`, `fn`
  を全部 scont の field に移し、 scont 自身 (= aro_gc_alloc 経由) に
  ASTroObjectHeader 追加。 OBJ_CONT SCAN_EDGES で `_o->cont` typed-ptr
  forward + scont 内 VALUE/typed-ptr 全部 visit。 setjmp/longjmp 前後で
  kobj を sp[0] から reload。
- **lex_scope head 追加**: aro_gc_alloc 経由なのに header 領域が無く、
  moving GC の region walk で gc_size が parent ポインタと衝突。
  `ASTroObjectHeader head` を先頭に追加。
- **scm_alloc_min 導入**: NODE\*\* / char\*\* / VALUE\* といった host
  pointer array の小サイズ alloc (= 8 B) が `aligned -
  sizeof(header)` で underflow → 巨大 memset → SEGV。
  `scm_alloc_min(c, size)` が min サイズを sizeof(header) に丸める。
- **IS_PTR の v != 0 filter**: ascheme の IS_PTR は singleton filter
  しかしていなかった。 uninit'd loop_args[i] = 0 が SCM_IS_PTR=true で
  pass、 forward at 0x0 → SEGV。 v != 0 check を追加。

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
