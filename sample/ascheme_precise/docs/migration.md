# ascheme → precise GC framework migration ログ

`sample/ascheme/` (= libgc / Boehm conservative GC ベース) を fork し、
ASTro precise GC framework (`runtime/precise_gc/`) に移行した経過と教訓。

## 完了状況

| Phase | 内容 | 状態 |
|---|---|---|
| 1 | ASTroObjectHeader 追加 + GC_malloc → aro_gc_alloc 全面置換 | ✅ |
| 2 | precise root tracking (= framework hook `ARO_GC_VISIT_ROOTS` 経由) | ✅ |
| 3 | SCAN_EDGES (= sframe heap obj 化 + 各 sobj 型 dispatch) | ✅ |
| 4 | call/cc + moving GC 対応 (= scont に saved state 移譲) | ✅ |
| 5 | GMP integration (= libc malloc + finalizer で leak 解消) | ✅ |
| 6 | testing 全 backend × 16 + 179 R5RS (= default mode 全 PASS) | ✅ |
| 7 | naming + alloc API unification (= ARO_/AROH_、 ARO_LOAD、 alloc returns
       encoded) | ✅ |
| 8 | WB 統合 + stress mode 全 backend で 17/17 PASS | ✅ (17/17) |
| 9 | AOT 経路 silently broken fix (= 5 bug、 §11) | ✅ |

完成 17 backend × 17 ascheme test + 179 R5RS chibi test (= 全 3315 case)
default + stress 両 mode で PASS:
- ✅ none, mark, mark_gen, mark_gen_inc, copy, copy_gen, copy_gen_inc,
     mark_compact, mark_compact_gen, bump, mark_bump_gen, immix, immix_gen,
     mark_bitmap_gen, mark_card_gen, mark_freelist, copy_scramble

`mark_gen` / `mark_gen_inc` の stress fail は framework 側 freelist
encoding bug (= slab_alloc が `(FreeSlot *)(h + 1)` を freelist に push し、
pop 時に `h = (AroObjectHeader *)fs` で payload=slot+8 を返していた。
連続再利用で payload が slot+16, +24 と shift し、 pair.cdr が次 slot の
header に書き込まれ freelist 破壊)。 sample の WB miss ではなく framework
bug だったため `gc_mark_gen.c` / `gc_mark_gen_inc.c` の slab_alloc /
new_page / free_slot を mark_freelist 同様の "freelist holds slot pointers"
convention に揃えて修正。 mark_freelist で動いている convention に合わせる
だけの局所 fix で、 他 15 backend は影響なし。

## migration 工程 (= 推奨手順)

ascheme の経験を抽象化した推奨工程は `docs/gc_design.md` §7.7 に成文化。
要点:

1. Phase 1: struct 改修 + alloc API 置換
2. Phase 2: precise root tracking (= sp[] / sframe / VISIT_ROOTS)
3. Phase 3: SCAN_EDGES 全 obj 型
4. Phase 4: 特殊機構 (= call/cc / continuation / etc.)
5. Phase 5: 外部 resource (= GMP / FILE * / 等) + finalizer
6. Phase 6: 全 17 backend × default + stress + scramble verify

**重要**: Phase 1 から `BARUBY_GC_STRESS=1 GC=copy_scramble` で全 test 回す。
gentle test だけで gap が隠れて後段で massive debug loop に陥る (= ascheme
で実際に経験した教訓)。

## 主な fix の備忘録

### Phase 1-2 (= structure + root)

- `struct sobj` の先頭に `AroObjectHeader head` を embed (= iter 75 contract)
- `SCM_TYPE(o)` / `SCM_SET_TYPE(o, t)` macro で head.flags 低 5 bits に
  type tag を入れる
- `aro_gc_alloc(c, sz)` で全 alloc 置換、 戻り値は encoded VALUE
  (= iter 76 で改修)
- 各 sobj 種別の SCAN_EDGES を `AROH_SCAN_EDGES` macro で実装
- `struct sframe` も `head` を持つ heap obj 化 (= OBJ_FRAME type tag)
- `AROH_VISIT_ROOTS` で c->env / c->globals / SYMBOL_TABLE / PORT_STD* /
  framework spill range を visit

### Phase 3 (= SCAN_EDGES)

- OBJ_PAIR / OBJ_VECTOR / OBJ_MVALUES / OBJ_CLOSURE / OBJ_PROMISE /
  OBJ_CONT / OBJ_FRAME 等の case を実装
- OBJ_VECTOR の items[] は `aro_gc_alloc(sizeof(header) + N*VALUE)` で
  header と data を確保、 visit 時に base 経由で forward
- OBJ_SYMBOL / OBJ_STRING の `name` / `chars` は byte payload base への
  interior pointer。 SCAN_EDGES で base を visit + re-derive する
  `ASCHEME_VISIT_INTERIOR_CHAR_SLOT` helper を追加

### Phase 4 (= call/cc + moving GC)

- `struct scont` に `saved_env`, `saved_tcp`, `k_val`, `fn_val` を追加し、
  scm_callcc の C-local を全て scont field に移す (= GC が scan できる場所)
- scont 自身も `AroObjectHeader` を持つ heap obj 化、 OBJ_CONT の
  SCAN_EDGES で内部 ptr を visit
- 全 PRIM / eval helper の C-local `VALUE arg[N]` を `c->loop_args` 等の
  root-visitable buffer に park
- scm_apply の saved-env が EVAL 跨いで stale 化していたバグ修正

### Phase 5 (= GMP integration)

- 初期試行: GMP allocator を `aro_gc_alloc_byte` (= GC heap) に redirect
  + ASTroObjectHeader offset 付き wrapping → moving GC で mpz の `_mp_d`
  が stale 化、 mark backend で sweep されると buffer も同時 free され
  use-after-free
- 最終解: GMP allocator は **libc malloc** に固定 (= GC heap 外、 不動)。
  OBJ_BIGNUM / OBJ_RATIONAL alloc 時に `aro_gc_finalize_register(c, o)` で
  framework finalize list に登録、 sweep 時に `mpz_clear` / `mpq_clear` で
  libc mem を free
- `aro_gc_account_external(c, ±bytes)` で GMP メモリ pressure を framework
  GC threshold に折り込む (= さもないと bignum-heavy code で GC 発火せず leak)

### Phase 7 (= naming + alloc API unification)

- `ASTroObjectHeader` → `AroObjectHeader`、 framework macro は `ARO_GC_*`、
  sample 提供 hook は `AROH_*` で接頭辞統一
- `aro_gc_alloc(c, sz)` の戻り値型を `void *` から `VALUE` (= encoded) に
  変更。 sample 側で `ARO_VAL(c, raw)` の boilerplate が不要に
- `ARO_LOAD(c, slot)` を decode の primary API に (= slot-based、 GC 文献の
  load barrier と整合、 将来 read barrier 拡張可)

## 残作業 (= Phase 8 以降)

`sample/ascheme_precise/docs/perf.md` §6.1 参照。 要点:

- ascheme の `struct sobj` 内 typed-ptr field (= `closure.env`, `vec.items`,
  `str.chars`, `sym.name`, `cont->saved_env`) と `sframe.parent` を VALUE
  化 (= encoded reference)
- 全 deref を `ARO_LOAD(c, &slot)` 経由に書換 (= 数百箇所)
- 完了後: stress + copy_scramble + 全 17 backend で PASS、
  `docs/perf.md` の ★ matrix が解消、 audit が機能する

## Phase 9 (= AOT silently broken fix)

`make bench-aot` で 17 backend × 9 workload × {plain, aot-cached} を回した
過程で **AOT が全 backend で動いていなかった** ことが発覚 (= --aot-compile
が `make failed` / `dlopen failed: undefined symbol` で all.so を作れず、
plain dispatch に sneaky fallback、 結果は正答だが速度向上なし)。

5 bug を順次 fix:

1. **`-I` baked-absolute path** (commit `e0867910`) — `-DASCHEME_PRECISE_DIR`
   / `-DASTRO_RUNTIME_DIR` を Makefile で baked、 main.c の `extra_cflags`
   経由で cc に渡す。 これ無しでは SD_*.c が `node.h` を見つけられない
2. **`-DBARUBY_GC=<num>`** (commit `96441e3e`) — SD cflags に GC backend 番号
   を伝えないと `gc_types.h` が default の `BARUBY_GC_COPY` を選び、
   AroObjectHeader / WB 経路 layout が host と食い違い、 sweep / alloc が
   garbage を読む
3. **`astro_cs_init(src_dir=ABSPATH, version=BARUBY_GC)`** (commit `b3c5f522`)
   — `astro_cs_init("code_store", ".", 0)` の "." が cwd で展開され、
   sample/ など別 dir 起動時に SD に `#include "sample/./node.h"` が embed
   されて build fail。 BARUBY_PRECISE_DIR 同等の baked path に変更、
   version に BARUBY_GC を渡して backend 切替時 cache 無効化
4. **`node.h` で `#include "precise_gc/gc.h"`** (commit `d2769de5`) —
   `aro_gc_wb` の static inline 定義が SD に取り込まれず extern call が
   emit され、 非 WB backend (= copy / mark / immix) で `dlopen failed:
   undefined symbol: aro_gc_wb` で load skip。 baruby_precise iter 59 と
   同じ fix
5. **`seen` 配列を libc malloc へ** (commit `976cea00`) — run_file_aot の
   dispatcher patch loop で `seen = aro_gc_alloc_raw(...)` し直後に
   `seen[0] = hash` で AroObjectHeader を上書き。 mark_freelist の sweep
   が gc_size = hash 下位 32-bit を読んで `p += garbage` の無限 loop。
   GC heap pointer を保持しない一時 buffer なので calloc / free が正解

これら無しでは「動いてるように見える silent fallback」 (= 正答だけ出る、
ただ実行は plain 速度のまま) が発生していて、 stderr の `astro_cs_reload:
dlopen failed` 等を見落とすと気づけない。 baruby_precise も同種の (3)
を持っていたので併せて fix (commit `8f5b30ce`)。

完了後の bench (= `make bench-aot`):
- 15 backend で AOT が plain より速く、 9/9 workload で libgc plain を上回る
- 残: `mark + matmul` のみ AOT regression (= 4.6 → 10.2s、 GC cadence
  影響、 結果は正解)、 `mark_compact` は plain と同じ 3 bench SEGV (=
  既存 sliding-compact bug)

## 参考

- `docs/gc_design.md` §7.7 — 本 migration の教訓を抽象化、 推奨工程
- `sample/ascheme_precise/docs/perf.md` — 17 backend × libgc baseline 実測
  + §10 AOT mode
- `sample/baruby_precise/` — precise GC framework の reference sample
  (= typed-ptr field uniform VALUE 化済、 stress + scramble で 8/8 PASS)
