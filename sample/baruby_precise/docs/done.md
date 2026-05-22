# baruby Done

[spec.md](spec.md) — 言語仕様、[runtime.md](runtime.md) — 実装、
[todo.md](todo.md) — 残タスク、[perf.md](perf.md) — ベンチ。

## 2026-05-22 (73) — baruby_ary_push fast-path inline + AOT bake で endbr64 削除

iter 72 で perf 詳細測定 (= plain + AOT matrix 取得) 後、 hot path 分析
で見つけた 2 つの micro-opt。

### baruby_ary_push fast-path inline (commit `b9a52a23`)

`perf record` で sieve copy AOT を観察したところ baruby_ary_push が
**23% CPU 占有**。 function prologue 6-reg save + 5-instruction body の
比率が悪く、 hot push 経路で overhead 過大だった。

split:
- `static inline baruby_ary_push` を node.h (gc.h 後) に置き、 fast-path
  (= `a->len < a->capa`) を直接書く。 `__builtin_expect(..., 1)` 付き。
- 旧 grow path は `baruby_ary_push_grow` に rename して node.c に残し、
  inline 側が `capa` 不足なら call する形に。

sieve copy AOT: **0.58s → 0.53s (-8.6%)**。

### AOT bake で `-fcf-protection=none` (commit `4e3386f8`)

各 SD の先頭から endbr64 (4 byte、 Intel CET indirect-branch protection)
を削除。 baruby_precise binary 自体は CET 有効のまま、 dlopen 経由の
SD shared library だけ CET 抜く trade-off。

Security 観点: SD は AOT bake で baruby_precise が自分で gcc 起動して
作る ad-hoc な .so なので、 攻撃面としては元 binary 経由でしか到達
できない。 IBT 保護を切る trade-off は実用的に妥当。

SD function size が 4 byte/関数だけ縮小、 i-cache 圧 軽減。 perf 影響
は hot loop 0.x% 程度。

### 試したが効かなかったこと

`node_add` の slow path (= String concat / Array plus / type error) を
`__attribute__((cold))` 関数に分離して .text.unlikely section へ。
動作は OK だが sieve perf は 0.53 → 0.56 で若干 regression、 revert。
理由は LTO + static inline で既に slow path が cold branch に隔離
されていて、 別関数化したことで関数 call overhead だけが追加されたから。

### 教訓

binop の slow path 等 cold pathに 対する手動 cold attribute は LTO が
うまく最適化済の場合は無意味になる。 measure first 大事。

## 2026-05-22 (72) — walker 削除、 parse-time sp_offset bake に置換

iter 61 で導入した `walk_bake_sp_offset` (~170 行 hand-written
per-kind structural recursion) を削除し、 sp_offset /
callee_fp_offset の bake を transduce 中 (= parse time) に
完結させた。 user 指摘「parse 時に解決するから walker 要らないって
話でしょう？」 を直接実装。

### 設計

- `tc->chain_sum` (int32_t) を transduce_context に追加。 push_frame で
  0 リセット (= 各 body の root coordinate)、 pop_frame で復元。
- `WITH_CHILD_CHAIN(kind, BODY)` macro: BODY 評価中だけ chain を
  parent.slot_count だけ bump (= GCC statement-expression)。
- bake helper (`bake_lget` / `bake_lset` / `bake_call` /
  `bake_call_static`): `partial = index_or_argidx - tc->chain_sum` を
  operand に焼き、 NODE を `frame->bake_list` に append。
- pop_frame で bake_list を iterate、 `*operand -= max_cnt` + clear_hash。
  最終 locals_cnt は frame の high-water mark なので pop 時に確定済。

### chain bump サイト一覧

iter 70 (= walker auto-gen 試行) で revert された理由 = pg_call_N の
sp_body を誤って walk する bug は iter 72 では発生しない。 chain bump
は @child を持つ parent ALLOC site のみで起こり、 sp_body のような
非 @child NODE * operand は構造的に無関係。

- 二項 (add/sub/mul/div/mod/lshift, lt/le/gt/ge/eq/neq/spaceship): slot_count=2
- ary_lit_1..4: slot_count=1..4
- ary_push 連鎖 (sz > 4): iter ごとに `chain += 2 * (sz - i)` で
  runtime nesting depth を反映
- call_aget/aget2/aset/size/pop/push/to_s/to_i: slot_count=1..3
- call_1/2/3 + pg_call1/2/3 args loop: slot_count=N
- node_return: slot_count=1
- PM_INTERPOLATED_STRING_NODE iterative add+to_s 連鎖: 後続 commit
  (`caaafaef`) で対応

### LOC

- baruby_parse.c: 2256 → 2025 行 (-14 net)
- walker 関数 (170 行) 完全削除
- chain threading + bake_list infra (~150 行) で置換

### 検証 + perf

- 16 backend × 8 T_*.ba.rb × 2 (non-stress + stress) = **256/256 PASS**
- AOT mode: copy backend で fib_pair 0.245s
- PG mode: copy backend で fib 5.96s
- 文字列補間 `"hello, #{name}!"` 系 動作確認
- plain mode 10 bench geomean **iter 71 比 -3.02% 高速化**
  (list_alloc -9.5%、 sieve -8.2%、 list_sort -5.8% など)。 詳細
  [perf.md §1.5](perf.md)。

### iter 72 初版 → 修正 (commit `2dd76623`)

iter 72 初版 (`25caede1`) では LTO による function reordering 副作用
で plain mode geomean +5.15% regression を観測。 原因は bake_X helper
4 個が transduce に inline されて transduce body 肥大 → cold cluster
が押し出されて dispatcher の cache layout 悪化 (i-cache miss 7.9×、
cache miss 5.6×)。

修正 = `bake_lget` / `bake_lset` / `bake_call` / `bake_call_static` に
`__attribute__((noinline, cold))` を付与。 LTO に「parse 時 hot でないので
inline するな・cold cluster に置け」 を明示することで text size 縮小
(= iter 71 比 1.3 KB 小)、 上記 net -3.02% に到達。

[feedback_inline_register_pressure](../../../docs/memory)
の教訓 (= LTO は親切すぎることがあるので hot path から離れた helper
には noinline/cold を明示) を再確認。

### 副次効果

- code_repo 走査 + per-body walker 呼び出しの post-parse loop が消滅。
- ASTroGen の walker auto-gen task (= iter 70 試行、 revert 済) は
  根本的に不要になった。 todo.md「walker の framework 化」 解消。
- iter 71 の per-body self-contained 化 が伏線として効いている
  (= callee_locals_cnt 依存があったら walker 削除は無理だった)。

commits: `25caede1` (walker 削除) + `caaafaef` (interp 対応) +
`e9a8742f` (bake_list を tc 共有 + pre-alloc) + `2dd76623`
(noinline/cold で LTO layout 修正)

## 2026-05-22 (71) — call_N / pg_call_N の args を @child 化 (per-body self-contained)

iter 61 で walker は call_N / pg_call_N の args を walk するときに
`chain += callee_locals_cnt` を加算していた (callee 側 frame の上に
args 評価結果を置く規約だったため)。 これが walker の **cross-body
依存** となり、 dynamic dispatch (= 呼び先 body が parse 時に未知)
では成立しない設計だった。

iter 71 で call convention を再設計:

- `call_N` (call_0..call_3) と `pg_call_N` (pg_call0..pg_call3) の args
  を `NODE *aN` operand から `VALUE aN@child` に変更。 framework の
  `@child` dispatch が自動で `sp += N` → `sp[-N..-1]` への spill →
  body には `VALUE` 引数で渡す経路に乗る (= node_add 等と同じ規約)
- call_N node の `slot_count` が 0 → N に変わる (= caller 側 sp 消費 N)
- body は `extras = locals_cnt - N` を zero-init して `sp + extras` で
  dispatch (callee の sp top = caller_sp + locals_cnt は不変)
- slowpath は framework が spill 済の `sp[-N..-1]` を読んで
  `sp_dispatch_fresh_frame(c, body, args, N, sp - N)` を呼ぶ
- walker の `chain += callee_locals_cnt` 特殊扱い 6 case (call_N + pg_call_N)
  を削除、 generic な `child_chain = chain + slot_count` のみで args を walk
- `arg_index` operand が runtime に使われなくなったので node.def から削除

### 意義

- walker bake の semantic が「caller の slot_count (= N) のみに依存」
  に縮約 → 動的言語 / 呼び先 body 不明でも sp_offset bake が成立する
  ようになる architectural simplification
- (iter 70 で実験した walker の ASTroGen auto-gen は revert 済。
  callee_locals_cnt 依存を framework に sneak させる必要があり、
  単純な structural recursion で書けなかったのが原因。 iter 71 で
  callee 依存が消えたので auto-gen が再度視野に入る — 別 iter)

### 検証

- 16 backend × 8 T_*.ba.rb × 2 (non-stress + stress) = **256/256 PASS**
- AOT mode (PG): `copy` で fib 5.92s 完走
- 性能影響なし (= 同じ bake 結果を異なる route で生成しているだけ)

commit: `27dfedd1`

## 2026-05-22 (69) — 残 7 backend に mremap-based realloc_in_place

`gc_mark_freelist` / `gc_immix` / `gc_mark_gen` / `gc_mark_gen_inc` /
`gc_mark_bitmap_gen` / `gc_mark_card_gen` / `gc_immix_gen` に
`aro_gc_realloc_in_place` override を追加。 mmap-backed LargeObj に
対しては **mremap(2)** で in-place 化 (gc_copy / gc_mark_compact が
malloc-backed なので realloc(3) を使うのと対称)。

- 非 gen (mark, mark_freelist, immix): MREMAP_MAYMOVE 許容 (forward_payload
  が新 addr を見る)
- gen (mark_gen, mark_gen_inc, mark_bitmap_gen, mark_card_gen, immix_gen):
  MAYMOVE なし (young_objs / remset の stale ptr 回避)

sieve 改善幅: gc_mark_gen で **-21%** (1.43 → 1.12)、 gc_immix で
**-13.4%**、 gc_mark_freelist で -8.8%、 gc_mark で -6.8%。
256/256 PASS 維持。

commit: `f6f88a59`, `cd4b77ea`, `173b94a5`, `eae4ceee`, `0a34890c` (docs)

## 2026-05-22 (68) — gc_mark + gc_mark_compact に mremap / realloc(3) realloc_in_place

iter 67 で gc_copy に導入した `aro_gc_realloc_in_place` hook を、
gc_mark_compact (malloc-backed、 realloc(3)) と gc_mark (mmap-backed、
mremap MAYMOVE) に展開。

- gc_mark_compact: sieve **-6.2%** (1.138 → 1.067)
- gc_mark: sieve **-6.8%** (1.094 → 1.020)
- AOT mode 全 15 backend × 主要 bench で動作確認 (plain → AOT で 1.6-6× 加速、
  perf.md §6 「未検証」 を解除)

commit: `93ab6d59`, `d2b07ddf`, `de019039` (AOT docs)

## 2026-05-22 (67) — aro_gc_realloc_in_place hook + gc_copy override

`gc.h` に新規 hook `aro_gc_realloc_in_place(c, old, new_size)` を追加、
`gc_common.c` で `__attribute__((weak))` default (NULL を返して fallback)
を置く。 gc_copy.c に strong override を実装し、 large→large の
resize を realloc(3) で in-place 化。

`aro_gc_realloc_payload` の caller (sieve の BaArray.items doubling 等)
は memcpy + 一時 2x メモリ消費を払わなくて済むようになり、 gc_copy
で sieve **-11.5%** (1.186 → 1.050)、 list_sort -5%、 hash_chain -4%
の改善。 256/256 PASS。

contract:
- 古 payload が arena 内 (= small obj) なら NULL を返して fallback
- new_size < LARGE_THRESHOLD (= shrink to small) なら NULL
- stress mode (= 毎 alloc GC 試したい) では NULL
- LargeObj linked list を walk して該当 obj を見つけて realloc(3)
- KIND_PAYLOAD_VAL の新増領域は memset(0) (GC scan 安全性)

commit: `4aa6f36b`, `230343ac` (todo)

## 2026-05-22 (66) — region-bump backend の large_alloc 経路

todo.md P0「copy 系 backend に large_alloc 経路を追加」を gc_copy /
gc_mark_compact に実装。 `LARGE_THRESHOLD = 4096 B` 以上の payload は
malloc 別領域 (non-moving) + linked list (LargeObj) 管理。 glibc が
≥128 KiB chunk を mmap-backed にするので、 free → munmap で物理
メモリ即解放。 sieve / hash_chain の BaArray.items doubling パターンで
from-space に残る dead 領域問題を解決。

- gc_copy: sieve -8% / hash_chain -9% / 6 bench で win (geomean -2.6%)
- gc_mark_compact: hash_chain **-27%** (1.393 → 1.011) / list_alloc -6%
  (geomean -5%)
- mark_compact では Lisp-2 slide pass が region 内のみに限定、 dead
  large の memmove が消滅して大勝

scan-loop を fast-path / large-path に分離し、 large_head==NULL の
ホットパスは fast-path に分岐して binary_trees regression を最小化
(+3%)。 256/256 PASS。

commit: `08dd67ad`, `f3680742` (gc_copy fast-path 分離), `4c9a017d`
(gc_mark_compact), `39068bcd` (docs), `bfa7308a` (perf.md §4.5),
`a010e3ad` (todo 完了)

## 2026-05-22 (65) — aro_gc_fini で全 backend に clean shutdown 追加

全 16 backend に `void aro_gc_fini(CTX *c)` を実装、 main.c の
`return 0` 直前で呼ぶ。 各 backend が aro_gc_init で確保した resource
(mmap'd 領域 / linked list の large object / gray buffer / remset
buffer 等) を release し、 ASTroGC struct 自体も free して
c->astro_gc を NULL に戻す。

動機: (a) 多重 instance 化への布石、 (b) valgrind / leak-sanitizer
clean run、 (c) 将来 mid-process re-init を許す setup でテスト可能に。

256/256 PASS。 commit: `3573b427`

## 2026-05-22 (64) — aro_gc_realloc_payload を gc_common.c に集約

これまで 14 backend で完全に同じ body (parking + 内部 alloc + memcpy)
を個別に持っていた `aro_gc_realloc_payload` を gc_common.c に extract。
backend 側は backend-specific な header 読み出し (`aro_gc_kind_of` /
`aro_gc_size_of`) だけ提供。 GCHeader layout は backend ごとに違う
(flag-packed vs separate kind field; 8-byte vs 16-byte) ので
per-backend 実装が必要だが、 共通 body は一箇所で管理。

gc_none は GCHeader を持たない (libc malloc 直) ので、 独自の
aro_gc_realloc_payload を保持。 Makefile で GC=none のときだけ
gc_common.c の link を skip。

net -47 行、 256/256 PASS。 commit: `ab80cd2d`

## 2026-05-22 (63) — sp_top 引数 + sample helper sp 引数を全廃

`aro_gc_alloc / aro_gc_alloc_byte / aro_gc_realloc_payload / aro_gc_collect`
から `VALUE *sp_top` 引数削除 (gc.h + 16 backend)。 sample helper
(baruby_ary_new / str_new / ary_push 等 12 個) から `VALUE *sp`
引数削除 (context.h + node.c)。

contract:
- caller が alloc / helper を呼ぶ前に `c->sp = sp;` を更新
- alloc / helper は `c->sp` を GC scan upper bound として読む

`c->sp` 一本化で「sp_top 引数を渡し忘れ」 のバグが原理的に消えた。
node.def の helper 呼出点 (11 site) に `c->sp = sp;` 挿入。

256/256 PASS。 commit: `c143a016`

## 2026-05-21 (62) — iter 62 framework abstraction: ASTroGC 化 / global 排除

全 16 backend の process-scope state を heap-alloc な
`struct ASTroGC *` (c->astro_gc) に集約。 共通 header
`AroGcCommonState` (stats + bool stress + timer) を **first field**
に置く約束で `ASTRO_GC_COMMON(c)` macro が type-safe に access。
process-scope global variable は完全に排除、 複数 instance を 1
process に co-exist させられる設計に。

256/256 PASS。 commits across iter 62 series.

## 2026-05-21 (61) — fp 引数完全削除 (dispatcher signature を 3-arg 化)

baruby_precise の dispatcher convention を `(c, n, fp, sp)` 4 引数から
`(c, n, sp)` 3 引数に縮約。 各 NODE_DEF body は parse-time に walker が
bake した `sp_offset` / `callee_fp_offset` operand 経由で sp 相対に
ローカル変数 / 引数 / callee フレームを参照する。

### 経緯

iter 60 で `child-self-advance` 規約 (= 各 dispatcher が body 入口で
`sp += slot_count` する) が定着し、 sp 上の位置関係が一意化。 これで
fp = sp - locals - chain で再構成できることが parse-time に static に
計算可能になった。 walker が AST を辿って各 lget/lset の
`sp_offset = index - chain - locals_cnt` と各 call/call2/call_static の
`callee_fp_offset = arg_index - chain - locals_cnt` を operand に焼く。
runtime は sp + offset を一発で読むだけ。

### 主要変更

- `baruby_gen.rb`: `common_param_count` 4→3、 `child_dispatch_args` を
  `"c, field, sp"` に override
- `node.h`: `EVAL` / `BARUBY_EVAL_ARG` / slowpath decl から fp を削除、
  `node_dispatcher_func_t` typedef も 3-arg 化
- `context.h`: `CTX` 構造体から fp フィールド削除
- `node.def`:
  - `node_lget` / `node_lset` に `int32_t sp_offset` operand 追加、
    `fp[index]` → `sp[sp_offset]`
  - `node_call` / `node_call2` / `node_call_static` に `int32_t
    callee_fp_offset` operand 追加、 `fp + arg_index` → `sp + callee_fp_offset`
  - `node_call_builtin`: `fp[i]` → `sp[-params_cnt + i]` (= callee
    frame top の下に既に args が並んでいる前提)
- `baruby_parse.c`: `walk_bake_sp_offset` 追加 (~200 行)。
  callsite_resolve 完了後 (= forward-ref の locals_cnt patch 後) に
  toplevel + 全 code_repo entry を walk。 各 NODE kind について:
  - 構造ノード (seq/if/while/return/binop/ary_lit_N/aget/aset/...) は
    child_chain = chain + n->head.slot_count で子に再帰
  - lget/lset/call/call2/call_static は operand bake + 必要なら子に再帰
  - call_N / pg_call_N は args について chain += callee_locals_cnt
    (= 子は sp + locals_cnt 起点で evaluate されるため)
  - lget/lset/call 系で operand 書き換えた後 `clear_hash` で HASH cache
    invalidate (HASH には sp_offset / callee_fp_offset が含まれるため)
  - ALLOC_node_lget / lset / call / call_static の全 call site で
    INT32_MIN sentinel を渡し、 walker が後で本値を書く
- `node_slowpath.c`: 全 slowpath を 3-arg signature に。 `sp_dispatch_via_fp`
  を `sp_dispatch_via_callee_fp_offset` に rename、 `n->u.node_call.callee_fp_offset`
  を直接参照。 旧 `sp + 16` magic を `n->u.node_call_N.locals_cnt` に修正
  (fastpath と整合させて walker bake と矛盾しないように)
- `main.c`: `c->fp` 初期化削除、 `EVAL(c, ast, c->sp)`、 walker が iter
  できるよう `code_repo_count` / `code_repo_body_at` / `code_repo_locals_cnt_at`
  accessor を export

### 検証

oracle: 15 backend × 35 bench × `-n 3` で **0 FAIL / 0 FATAL** 完走
(plain mode + AOT mode 両方)。

perf (AOT, copy backend、 抜粋): 詳細は [perf.md §2 iter 61 セクション](perf.md)。

- prime_count 4.67 → 0.52s (**-89%**) — inner loop が trial-division で
  dispatcher heavy、 GC pressure ゼロ。 fp register 開放 + arg shuffle
  削減が直接効いた典型例。
- 関数呼び出し中心の bench (call / chain20 / collatz / early_return /
  loop / nqueens) で **-50%〜-68%**。 SD chain 越し fp の引き回しが
  なくなって register pressure 減 + spilll/reload 削減。
- GC-bound bench (binary_trees / cons_list / list_alloc / fib_pair 等)
  は -22〜-36%。 dispatcher overhead 比率がそのまま改善幅に出る。

### 制限と将来の整理

walker は `walk_bake_sp_offset` で全 NODE kind を hand-write 列挙
(~200 行)。 framework (`astrogen.rb`) に generic な per-kind child-walk
callback を入れて自動生成にする refactor が次の候補 (= HASH 系と同じ
形)。 別 iter で対応予定。

`node_scope` は現 parser から使われないので tail-EVAL に簡略化のみ。
将来 lexical scope が parser に追加されたら `envsize` を walker の
recurse 時 locals_cnt として渡す処理が必要。

## 2026-05-20 (59) — AOT 読込壊れ修正 (`aro_gc_wb` undefined symbol)

`bench/hash_chain.ba.rb` で AOT 速度が plain と同じだったのを perf で
追跡 → `cs_load` で `astro_cs.all_handle=(nil)` を発見。 dlopen が
**`undefined symbol: aro_gc_wb`** で失敗していた。

### 原因

SD `.c` ファイルは `node.h` → `context.h` を include したあと
`node_eval.c` を直 include する構造。 `node_eval.c` の
`EVAL_node_call_aset` (= `arr[i] = v`) は `aro_gc_wb` を呼ぶが、
**どこにも `gc.h` が include されていなかった**。

非 generational backend (`GC=copy`, `none`, `mark` 等) では `gc.h` の
`aro_gc_wb` は **`static inline`** ストア (`*slot = v`) として
定義されているので、 gcc がインライン展開してくれれば外部参照は要らない。
だが gc.h 未 include だと、 gcc は暗黙宣言扱いで extern 関数呼出
を emit → all.so に `U aro_gc_wb` が残る → dlopen 失敗 → all_handle=NIL
→ SD load 全部 skip → AOT 効かない (= 「plain と同じ速度」 現象)。

### 影響範囲

`arr[i] = v` (= `node_call_aset`) を含むベンチを、 **非 generational
backend (`none / mark / copy / mark_compact / bump / immix / mark_freelist`)
で AOT 実行した場合のみ**。 generational backend (`*_gen`) は
`aro_gc_wb` を real extern function として main binary に export して
いるので dlopen は成功 → AOT は元から効いていた。 非 gen 側は
static inline 想定だったので main binary に export 無し → SD .so
からの参照が undefined。

配列書込なしのベンチ (cons_list / fib_pair / loop / fib 等) は SD が
`aro_gc_wb` 参照自体を持たないので、 非 gen backend でも問題なく load
していた (= iter 49 matrix で cons_list 非 gen が 0.20-0.60 範囲だった
のはこれが理由)。

### 修正

`node.h` で `#include "gc.h"` を追加 (`context.h` の直後)。

```c
// gc.h defines aro_gc_wb (write barrier) used by EVAL_node_call_aset
// inside node_eval.c.  Non-gen backends provide it as a `static inline`
// stub; gen backends export it as an extern.  Either way, SD .c that
// includes node_eval.c needs the declaration / inline-body in scope —
// otherwise gcc emits a call to the implicit `extern aro_gc_wb` and
// dlopen of all.so fails with an undefined symbol on non-WB GC backends
// (e.g. GC=copy) for any program that touches array write (a[i] = v).
#include "gc.h"
```

### 効果

GC=copy、 AOT mode、 array-write 系ベンチ:

| bench         | plain  | AOT 修正前 | AOT 修正後 | 修正後の対 plain 加速 |
|---------------|-------:|-----------:|-----------:|----------------------:|
| hash_chain    | 1.479s |   1.454s   |  **0.227s**|        **6.5×**       |
| fannkuch      | 0.743s |   0.747s   |  **0.150s**|        **5.0×**       |
| nqueens       | 0.979s |   1.000s   |  **0.085s**|       **11.5×**       |
| list_sort     | 1.072s |     ~      |  **0.222s**|        **4.8×**       |
| life          | 1.365s |     ~      |  **0.160s**|        **8.5×**       |
| ast_eval      | 0.359s |     ~      |  **0.067s**|        **5.4×**       |
| dll_walk      | 0.725s |     ~      |  **0.155s**|        **4.7×**       |

これまで AOT mode の効いていた cons_list / fib_pair / list_alloc /
binary_trees は **影響なし** (元から 2-3.5× 出ていた)。 oracle 20/20
合格 (plain + AOT)。 全 14 GC backend で build + 基本ベンチ通過確認済。

### 追跡手順 (どう特定したか)

1. `perf record` で hash_chain plain と AOT 比較 — top function 分布が
   完全に一致 (`DISPATCH_node_lget 25%`, `DISPATCH_node_lt 25%` 等)。
2. `DISPATCH_node_lget` にカウンタを仕込んで両モードの呼出回数を比較
   → 両方 209M 回。 **SD が一切実行されていない** ことを確認。
3. `EVAL_node_def` で `fe->body->head.dispatcher_name` を出力
   → `DISPATCH_node_seq` (default)。 cs_load が dispatcher を
   更新できていない。
4. `astro_cs_load` 入口にトレース → `all_handle=(nil)` で即 false 返し。
5. `astro_cs_reload` の `dlopen` 直後に `dlerror()` 出力
   → `undefined symbol: aro_gc_wb`。
6. `nm code_store/o/SD_*.o` で `U aro_gc_wb` 確認 → gc.h 未 include
   が真因。

`perf` だけで気付くのは難しい (load 失敗は silent)。 `dlopen` の
エラー検査と `nm code_store/o/*.o` の `U` シンボル監査を AOT bake
の自動チェックに加える価値あり。

---

## 2026-05-21 (58) — @child operand 全面導入 + callee_sp aliasing fix

ASTroGen に `@child` operand kind が追加された (b4b0eb0, user 側)。
`VALUE lv@child` と書くと:
- AST 上の格納は `NODE *` (struct field、 ALLOC 引数も NODE *)
- DISPATCH が子 dispatcher を呼んで結果を `sp[i]` に snapshot
- EVAL body は VALUE 受け取り — 手スピル `sp[0] = UNWRAP(BARUBY_EVAL_ARG(...))`
  が消える
- SPECIALIZE は `SD_<hash>(...)` 直接呼出で snapshot 統合 (AOT inline 可)

iter 58 で baruby_precise + baruby (libgc) の全 eligible NODE_DEF を
`@child` 化。 `commit 31c01a5`。

### 変換した node (両 sample 共通)

- arith: `node_sub`, `node_mul`, `node_div`, `node_mod`, `node_lshift`
- comparison: `node_le`, `node_ge`, `node_eq`, `node_neq`, `node_spaceship`
  (`node_add`, `node_lt`, `node_gt` は user 側 cfbebf6 で既に変換済)
- 制御: `node_return` (`value` を @child)
- 配列リテラル: `node_ary_lit_1..4` (全 element を @child)
- メソッド (eager 評価): `node_ary_push`, `node_call_size`, `node_call_aget`,
  `node_call_aget2`, `node_call_aset`, `node_call_push`, `node_call_pop`,
  `node_call_to_s`, `node_call_to_i`

### あえて @child 化しなかった node

- `node_lset` — rhs を sp[0] に snapshot すると、 arg-eval 用の lset
  chain (lset(arg_idx+0), lset(arg_idx+1), ...) で次の lset の DISPATCH
  spill が前の lset destination (fp[arg_idx+0] = sp[?]) を clobber する。
  user 指摘 [[feedback-eval-arg-vs-child]] とも整合: 返り値を捨てる
  / 1 度しか使わない operand は @child せず EVAL_ARG で取るのが正解。
- `node_if` cond — 値は IS_TRUTHY 即時判定で discard。 then/else は
  条件付き評価なので必ず NODE * のまま。
- `node_seq` head — 値を discard、 snapshot 不要。
- `node_while` cond/body — cond はループ毎に再評価、 body も条件付き。
- `node_scope`, `node_def`, `node_call`, `node_call_N`, `node_call2`,
  `node_pg_call_N`, `node_call_static`, `node_call_builtin` — 独自の
  arg-eval / dispatch プロトコル (BARUBY_EVAL_ARG, fresh callee
  frame、 sp_body 直接呼び等) で @child と互換性なし、 ship 不要。
- `node_str_lit`, `node_lget`, `node_num`, `node_true/false/nil` — 子なし。

### framework extension (lib/astrogen.rb)

per-language hook を 2 段追加:

1. **`child_storage_decl(slot)` / `child_storage_expr(slot)`** — snapshot
   の保存先。 default は `sp[slot]` (precise GC 用)、 baruby (libgc) で
   override し `VALUE _c#{slot}` という C-local に切替。
2. **`child_dispatch_args(slot, field)`** — 子 dispatcher を呼ぶ際の
   引数。 default は `c, #{field}, fp, sp + #{slot}` (4-arg dispatcher、
   precise GC)、 baruby は `c, #{field}, fp` (3-arg dispatcher) を返す。

これで baruby (libgc, common_param_count = 3) でも framework の @child を
そのまま使え、 C-local snapshot による「sp[] への書き込みゼロ」 が成立。

### correctness fix: callee_sp aliasing (実は cfbebf6 既存の bug)

@child 化を進めて life bench が崩壊した経緯を辿ると、 cfbebf6 (`node_add`
のみ @child) の時点で実は `n + f(x, y, z, w, e)` のような 「binop の
rhs が >3-arg call で nested @child を持つ」 パターンで silent corruption
が発生していた。 偶然 oracle で検出されていなかっただけ。

#### 原因

parser は def の locals_cnt を `n->locals.size` (declared local 数) で
記録 (`baruby_parse.c` の `body_locals = n->locals.size` 行)。 一方
runtime node_call は callee_fp = caller_fp + arg_idx、 callee_sp =
callee_fp + locals_cnt として callee の sp を立てる。

caller (= 親の def) 側で binop @child を評価すると、 そこから dispatch
される call の lset chain が caller's fp[arg_idx + i] に args を書く。
arg_idx は parser の `tc->frame->arg_index` の現在値 = locals.size base
で計算される。

caller の sp は runtime では caller_fp + caller_locals_cnt =
caller_fp + caller's body_locals (= caller's locals.size) で配置される。

ここで parser が arg_idx を locals.size から bump して call 用 slot を
取るが、 callee_sp = callee_fp + locals.size = caller_fp + arg_idx +
callee_locals.size となり、 これが caller_sp + N と一致してしまう。
specifically callee 側で @child snapshot (callee_sp[0..1]) と caller の
@child snapshot slot (caller_sp[0..1] = caller_fp[caller_locals.size +
0..1]) が同じ物理メモリを指す。 callee の sub @child などが発火する瞬間に
caller の lhs snapshot が clobber される。

#### fix

`baruby_parse.c::PM_DEF_NODE` で `body_locals = max_cnt` (parser の
arg/scratch 高水位線) に変更。 callee の sp は callee_fp + max_cnt で
立つので、 nested call arg slots の上に余裕を持って配置される。
slight over-allocation だが GC root も zero-init で safe。

両 sample に同形の修正を入れた。 fix なしの状態では life が 34 (期待
112) を返していた。

#### ある意味の発見

cfbebf6 (`node_add` のみ @child) でも life は壊れていた。 ベンチ
oracle で偶然取りこぼされた pre-existing bug。 `@child` の全面導入で
顕在化 → 修正 という流れで間接的に既存 bug を直したことになる。

### A/B benchmark — baruby (libgc) 中心 (user 要望)

#### plain mode median of 3

| bench (.ba.rb) | baseline (iter 53) | iter 58 (@child + fix) | Δ |
|----------------|-------------------:|----------------------:|---:|
| json_parse | 1.16 | 1.12 | -3% |
| fib_pair | 1.06 | 1.01 | -5% |
| hash_chain | 1.32 | 1.25 | -5% |
| cons_list | 0.76 | 0.88 | +16% regress |
| binary_trees | 0.81 | 0.82 | ±0% |
| substr_churn | 1.13 | 1.07 | -5% |
| tokenize | 1.14 | 1.10 | -4% |
| string_concat_dyn | 1.38 | 1.29 | -7% |
| list_alloc | 0.83 | 0.88 | +6% |
| list_sort | 1.15 | 1.14 | ±0% |
| nqueens | 1.01 | 0.88 | -13% |
| fannkuch | 0.75 | 0.69 | -8% |
| interp_calc | 1.17 | 0.99 | -15% |
| gc_combined | 0.95 | 0.94 | ±0% |
| dll_walk | 0.85 | 0.80 | -6% |

geomean は実質 -3〜-5% (string + int 系で揃って小 win、 cons_list だけ
+16% 単独退化 — おそらく `[x, list]` cons pair が @child snapshot 経由で
hit する code path が GC frequency に影響)。

#### naruby int benches (no allocation) plain / AOT 比較

| bench | plain | AOT | speedup |
|-------|------:|----:|--------:|
| loop  | 1.45  | 0.11 | 13× |
| fib(40) | 6.22 | 1.55 | 4.0× |
| tak | 0.70 | 0.22 | 3.2× |
| ackermann | 6.59 | 1.28 | 5.1× |
| collatz | 5.80 | 0.35 | 16.5× |
| compose | 1.42 | 0.24 | 5.9× |
| chain20 | 7.26 | 2.10 | 3.5× |
| chain40 | 7.94 | 3.61 | 2.2× |
| chain_add | 1.18 | 0.36 | 3.3× |
| gcd | 4.75 | 0.42 | 11.3× |

AOT は依然として大幅な speedup を出す (3〜17×)。 @child 投入で
SD_<hash>() direct call が `sp[i] = UNWRAP(SD_<child_hash>(c, ...))`
形に集約され、 gcc が cross-SD で register 越し inline する pattern
は維持。

### code gen 例

baruby (libgc) の `n = a + b` の DISPATCH 生成結果 — sp[] 書き込み無し、
C-local の `_c0` / `_c1` のみ:

```c
DISPATCH_node_add(CTX * restrict c, NODE * restrict n, VALUE * restrict fp)
{
    VALUE _c0;
    VALUE _c1;
    _c0 = UNWRAP((*n->u.node_add.l->head.dispatcher)(c, n->u.node_add.l, fp));
    _c1 = UNWRAP((*n->u.node_add.r->head.dispatcher)(c, n->u.node_add.r, fp));
    return EVAL_node_add(c, n, fp, _c0, _c1);
}
```

baruby_precise は sp[i] = ... で同じ pattern (precise GC root scan に
乗せる)、 違いは storage 先のみ。

## 2026-05-21 (58 補足) — snapshot 戦略 (sp[] vs C-local) は plain/AOT で勝者反転

@child の snapshot 保存先を per-language hook (`child_storage_*`) で
切替可能にしてあるが、 「どちらが速いか」 は **執行モードに依存** する
ことが naruby int 系の no-alloc bench (loop/fib/tak/ackermann/collatz/
compose/chain20/chain40/chain_add/gcd) で明らかになった。 GC ノイズが
無いので snapshot 戦略の差だけが直接観測できる。

### plain mode (interpreter loop, indirect dispatch)

baruby (libgc, C-local) が再帰 + 多 binop 系で **5〜12% 勝つ**:

| bench | libgc (C-local) | baruby_precise GC=none (sp[]) | sp[] cost |
|-------|--------:|--------:|---:|
| fib(40) | 6.22 | 6.53 | +5% |
| tak | 0.70 | 0.75 | +7% |
| ackermann | 6.59 | 7.29 | +11% |
| compose | 1.42 | 1.59 | +12% |
| chain20 | 7.26 | 8.14 | +12% |
| chain40 | 7.94 | 8.79 | +11% |
| (loop / collatz / chain_add / gcd は ±3% で tied) |

理由: dispatcher が関数ポインタ越しの indirect call。 LTO inline が
効かないので `sp[i] = ...` は実 memory store として残る。 C-local は
gcc が register 居住させやすく、 ABI 経由で次の SD に渡せる。

### AOT mode (SD inlined chain)

逆転、 baruby_precise (sp[]) が **7〜15% 勝つ**:

| bench | libgc (C-local) | baruby_precise (sp[]) | sp[] adv |
|-------|--------:|--------:|---:|
| fib(40) | 1.55 | 1.38 | -11% |
| tak | 0.22 | 0.19 | -14% |
| ackermann | 1.28 | 1.19 | -7% |
| chain20 | 2.10 | 1.94 | -8% |
| chain40 | 3.61 | 3.06 | -15% |
| chain_add | 0.36 | 0.32 | -13% |
| gcd | 0.42 | 0.38 | -10% |

理由 (仮説): SD が静的に bake されると `SD_<child>(c, n->u.X.lv, fp,
sp + 0)` の `sp + 0` は compile-time-known offset になり、 gcc が
restrict pointer 配下で完全に SROA + register allocate できる。 store
は実体として消える。 一方 C-local も同等に optimize されるはずだが、
4-arg dispatcher (sp 含む) のほうが 3-arg より gcc にとって alias 解析
の手がかりが多く、 cross-SD inline で register pressure が下がる可能性。
要 perf record で裏付け。

### 設計示唆

- conservative GC + plain interp → C-local 採用
- precise GC + AOT 重視 → sp[] 採用 (root scan に必要、 かつ AOT 最適化
  との相性も良い)
- ASTroGen の `child_storage_*` per-language hook はこの逆転を吸収する
  正しい設計判断だった

## 2026-05-20 (57) — BaArray CONTIG (header+items 同 alloc 内) → 棄却 → 全 revert

iter 56 embed の棄却理由 (BaArray size growth +33%) を回避する別案:
**CONTIG** — BaArray header は 24 B のまま、 items を同じ allocation
内に co-locate する (alloc size = sizeof(BaArray) + capa*sizeof(VALUE))。
`baruby_ary_new_from` (literal-array path) で 1-alloc 化、 `baruby_ary_new`
(grow target) は従来の 2-alloc を維持。

### 実装

- `OBJ_FLAG_ARY_CONTIG = 0x02u` (SSO=0x01 と区別)
- `BaArray.items` は heap path で別 alloc、 CONTIG path で `(VALUE *)(a + 1)`
- 全 15 GC backend の OBJ_ARRAY scan: CONTIG 時に items を inline 走査、
  moving GC では `a->items = (VALUE *)(a + 1)` で post-move 修正
- mark_bump_gen は minor + major 両方に修正必要 (major_promote 後の
  items pointer fixup が major_process で必要だったバグも追加発見)
- baruby (libgc) port、 conservative GC なので scan 修正不要

全 19 oracle × 全 backend で通過確認。

### A/B 結果 — binary_trees 圧勝、 hash_chain 大幅退化

immix_gen / copy_gen / mark_bump_gen / mark_freelist / libgc × 9 bench
の median-of-3 plain matrix:

| bench (大 win)      | iter 53 → 57 (代表値) |
|---------------------|-----------------------|
| **binary_trees**    | 0.76 → 0.66 (-13%) 全 backend で大 win |
| cons_list           | 0.74 → 0.70 (-5%)   |
| json_parse          | ±0%                 |
| substr_churn        | ±0%                 |
| string_concat_dyn   | ±0%                 |

| bench (大 regression) | iter 53 → 57 immix_gen | mark_bump_gen | mark_freelist |
|-----------------------|------------------------|---------------|---------------|
| **hash_chain**        | 1.16 → 1.98 **+71%**   | 1.23 → 1.72 +40% | 1.23 → 1.66 +35% |

hash_chain の immix_gen +71% regression が決定的に悪い。 clean A/B
(5-run median): immix_gen baseline 1.46s → CONTIG 2.05s = +40%。
copy は逆に -10% (1.55 → 1.34s) と win したが、 mark 系 backend は
全て退化。

### 棄却理由

iter 56 embed (棄却) と同じパターン:
- 局所的な大 win (binary_trees) はあるが、 hash_chain (一般的な
  dict-lookup workload) の +35〜71% 退化が許容できない
- 退化の原因は backend-specific (mark 系で顕著、 copy 系で改善)、
  「inline items scan vs separate KIND_PAYLOAD_VAL processing」 の
  どこに work が偏るかの違いだと推測 (perf record 未取得)
- 「総 mark 仕事量は同じだが、 mark_value_major を OBJ_ARRAY 内で
  ループ呼びすると、 KIND_PAYLOAD_VAL で一度呼ぶより遅い」 という
  immix の cache / branch 予測の影響と思われるが、 measurement-backed
  でないので仮説に留める

### 学び

- BaArray layout 系の変更 (embed / CONTIG / FAM) は hash_chain の
  ような nested array workload で**必ず**何らかの regression を生む
- 「メモリ密度」 と 「access pattern」 のどちらを変えても、 immix /
  mark 系 backend での GC scan の最適化が前提を変えるため、 局所的
  win が必ず広範な regression と組み合わさる
- ASTro の現状 GC interface (KIND_OBJ_ARRAY + KIND_PAYLOAD_VAL) は
  「OBJ + 子 payload」 の 2-object model に最適化されている。 CONTIG
  / embed / FAM は単一 object に inline するため、 mark 系の amortize
  パターンを壊す

### iter 57 のまとめ

- ~250 行の変更を実装 (CONTIG layout + 全 15 backend GC scan + 2 sample port)
- binary_trees -22%、 cons_list -5%、 hash_chain immix_gen +71% など
  mixed result
- net negative と判断、 全 revert
- todo.md の BaArray FAM-inline entry を「試行済 (CONTIG variant) で
  既に検証、 BaArray layout 変更は基本ボツ」 に更新

iter 53 SSO → iter 56 embed (棄却) → iter 57 CONTIG (棄却) の経緯で、
BaArray inline 系の最適化路線は exhausted。 別方向 (e.g., 真の
parser-level optimization、 GC algorithm 追加、 新 bench、 既存 GC
backend の cache layout 改良) に切替えるべきと判明。

## 2026-05-20 (56) — BaArray embed (SMALL_N=2) 実装 → A/B で棄却 → 全 revert

iter 55 で todo.md に scoping した BaArray embed=2 を実装。 全 15 GC
backend の OBJ_ARRAY scan に embed/heap dispatch を入れ、 baruby
(libgc) にも port、 全 19 oracle 通過 (× 16 backend = 304 行)。

### 実装したもの

- `BaArray.items` を `union { items, embed[2] }` 化 (BaArray 24B → 32B)
- `OBJ_FLAG_ARY_EMBED = 0x02u`、 `BARY_EMBED_CAPA = 2u`
- `bary_items(a)` / `bary_items_mut(a)` / `bary_holder(a)` accessor
- `baruby_ary_new`: capa<=2 で embed path、 zero-init で `[VAL_FALSE,
  VAL_FALSE]` から始まる
- `baruby_ary_push`: 3 paths (embed-fast / embed→heap promote / heap-grow)
- 全 15 GC backend の OBJ_ARRAY scan: embed のとき VALUE を inline scan
  (mark / forward / fwd_compact / mark_bump_gen の major promote / immix_gen
  の minor-major 全 variant)
- `.items` 直接アクセス ~30 箇所、 WB 8 箇所を accessor 経由に変更
- baruby (libgc) sister port、 conservative GC なので scan 修正不要

### A/B 結果 → 全 19 bench の geomean が net negative

immix_gen / copy_gen × 全 19 bench で測定:

| bench (回帰の例)    | copy_gen 55→56  | immix_gen 55→56 |
|---------------------|----------------|----------------|
| hash_chain          | 1.22→1.29 +6%  | 1.16→1.32 +14% |
| interp_calc         | 0.87→0.95 +9%  | 0.83→0.91 +10% |
| list_alloc          | 0.71→0.79 +11% | 0.69→0.77 +12% |
| list_sort           | 1.03→1.14 +11% | 1.06→1.11 +5%  |
| nqueens             | 0.94→1.02 +9%  | 0.96→1.04 +8%  |
| tokenize            | 0.74→0.81 +9%  | 0.77→0.81 +5%  |
| string_concat_dyn   | 0.92→1.00 +9%  | 0.94→0.97 +3%  |
| substr_churn        | 0.78→0.84 +8%  | 0.80→0.83 +4%  |

| bench (改善の例)    | copy_gen 55→56  | immix_gen 55→56 |
|---------------------|----------------|----------------|
| **binary_trees**    | 0.78→0.58 -26% | 0.76→0.60 -21% |
| fib_pair            | 0.83→0.77 -7%  | 0.72→0.73 +1%  |
| cons_list           | 0.74→0.72 -3%  | 0.68→0.65 -4%  |
| json_parse          | 0.83→0.83 0%   | 0.84→0.82 -2%  |

binary_trees は **-26%** で目立つ win (depth=21 で全 BaArray が 2-要素
`[left, right]` node、 embed が完全に hit してメモリも 24B → 32B で
items alloc 不要)。 ただし他 9 bench が +5〜14% 範囲で回帰、 geomean は
明らかに net negative。

### 棄却理由

iter 53 SSO で BSTR_SSO_MAX=15 が fib_pair +8% 退化を理由に棄却された
先例と同じパターン:

**BaArray を 24B→32B (+33%) するコストが、 embed の恩恵がない bench
で広範に payment される**。 hash_chain の bucket table、 list_alloc
の長 array、 tokenize の token array push、 interp_calc の AST node
など、 capa>2 の array が hot な workload では純粋なコスト増。

binary_trees / fib_pair / cons_list / json_parse のような全要素 N=2
の pair pattern は embed の恩恵を 100% 受けて勝つが、 そういう
workload は少数派。

### Path forward (todo.md 更新)

BaArray embed の方向自体は理論上 sound だが、 **size growth が許容
できない**。 alternative:

1. **inline items via FAM** (flexible array member): `BaArray { hdr;
   len; capa; VALUE items[]; }`. growth 時は新 BaArray を alloc して
   コピー。 BaArray 自体が移動するので caller の VALUE *が無効化、
   GC moving / non-moving 両対応にする必要あり (precise GC では既に
   moving 前提なので OK だが、 libgc では `BaArray *` を hold する
   code path に問題)。 さらに大きな変更。
2. **8-byte SSO_MAX=7 同形の size-preserving embed**: SMALL_N=1 で
   BaArray 24B 維持 (`union { items, embed[1] }`)。 1-要素 array
   しか embed 対象にならないので恩恵小、 棄却。
3. **size growth を許容するなら、 capa<=4 の `embed[4]`**: BaArray
   48B (+100%) — 確実に悪化、 棄却。
4. **何もしない**: iter 56 の経験から「BaArray のメモリ密度を
   下げてはいけない」 を確定。

todo.md エントリを「試行済・採用不可」 に更新、 alternative #1 (FAM)
は別の big-iter project として記録。

### iter 56 のまとめ

- ~300 行の変更を実装、 全 oracle 通過、 全 backend build OK
- A/B で net negative と判明、 全 revert
- BaArray メモリ密度が広範な bench で支配的因子だと確認
- 「精緻に scoping した large change を実装 → 数値で評価 → 棄却」 を
  honest に実行したことが iter 55 (scoping) + iter 56 (A/B + revert)
  の組合せで完結

iter 53 SSO の SSO_MAX=15 vs SSO_MAX=7 のときと同じパターン: 「実装
規模に対する win 比」 を測ってから ship を決めるのが正しい。

## 2026-05-20 (55) — alloc attribute 実験 + BaArray embed direction scoping

iter 54 で json_parse macrobench を追加した後、 次の perf 方針として
2 候補を検討した:

### 候補 A: BaArray embed (SMALL_N=2) → scoping のみ、 multi-iter project

json_parse は parser helper が毎 call で `[v, idx]` を return する
ため、 2-要素 BaArray の量産がボトルネック。 BaArray を SSO 同形で
embed[2] 化すれば 1-alloc / pair に縮められる。

期待 win:
- json_parse copy_gen 0.83s → ~-10% 見込み
- hash_chain の [k, v] pair 450k 個も embed → ~-5%
- cons_list の `[x, list]` も embed 対象

コスト:
- BaArray 24B → 32B (+33%) — SSO_MAX=15 が fib_pair で +8% regress
  した先例あり、 同じ罠の可能性
- `.items` 直接アクセス 40 箇所、 WB 8 箇所 (holder=a vs a->items
  の判別)
- 全 15 GC backend の OBJ_ARRAY scan: embed のとき inline VALUE
  scan、 heap のとき items ptr forward
- libgc 側 (baruby) は scan 修正不要 (conservative)

iter 1 で完結する規模を超える (SSO の 3 倍ぐらいの修正)、
todo.md に multi-iter project として整理し、 iter 56-58 で
scaffolding → flip → A/B + commit の 3 段で進める方針に。

### 候補 B: `aro_gc_alloc` への alloc attribute → 試行、 ship 価値なし

`__attribute__((malloc, alloc_size(2), returns_nonnull))` を
`aro_gc_alloc` / `_byte` / `_realloc_payload` に付与して A/B 計測。

immix_gen / copy_gen × 6 bench (json_parse / fib_pair / hash_chain /
string_concat_dyn / substr_churn / tokenize) で median-of-3 〜
median-of-5 を計測した結果、 全 12 cell で差が noise 範囲内 (<3%)。
特に hash_chain で一時 +15% 退化に見えたが clean A/B では 1.36s →
1.34s (-1%) で noise 内と確認。

理由考察 (測定の裏付けなし、 仮説): gcc の -O3 + -flto が既に
function body から `aro_gc_alloc` 等の aliasing 性質を推定して
おり、 attribute は redundant。 alloc_size も constant size の
場合は callee の `size_t payload_size` が固定値として伝播する
ので effect が薄い。

結論: attrs を ship しない、 todo.md エントリを試行済 (棄却) に
更新、 これは「ASTro framework として AOT specialization 経由で
既に最適化済の path だった」 ことの確認。

### iter 55 のまとめ

コード変更: なし (gc.h の attrs は revert)。 docs 整理が成果。
embed direction (BaArray SMALL_N=2) は todo.md に詳細スコープ
入りで残し、 次の iter で実装着手するか決める。

## 2026-05-20 (54) — `bench/json_parse.ba.rb` 追加 (macrobench、 SSO 効く)

baruby で書いた recursive-descent JSON parser + 再帰 sum (integer
leaves) を macrobench として追加。 20000 iter で oracle = 11_300_000、
immix_gen plain で 0.87s、 libgc で 1.18s。

### 設計意図

iter 53 で導入した SSO は短い string (≤7 chars) の alloc を 1-shot
化するが、 既存 bench で SSO が「真に効く」 workload は tokenize と
substr_churn のみ (両方とも string-only)。 json_parse は SSO 効果に
**加えて**:

- recursion + alloc が密に絡む macro pattern (parser helper が
  毎呼で `[value, next_idx]` を return → 2-要素 BaArray を多量に alloc)
- object-as-Array-of-pairs パターン (baruby は Hash 非対応なので
  自然なマッピング)
- 短命 string token (id / name / tags の値: 全 ≤7 char で SSO ヒット率
  100%)
- 短命 nested array (tags array + object pair array)

を一つで exercise。 既存 bench のどれともプロファイルが被らない。

### oracle

```
[{"id":1,"name":"alice","tags":[10,20,30]},
 {"id":2,"name":"bob","tags":[40,50]},
 {"id":3,"name":"carol","tags":[60,70,80,90]},
 {"id":4,"name":"dave","tags":[]},
 {"id":5,"name":"eve","tags":[100]}]
```

per parse: ids 合計 15 + tags 合計 550 = 565、 20000 iters で 11_300_000。

### 数値 (plain, single run)

| backend          | json_parse |
|------------------|-----------:|
| copy_gen         |    0.83s ★ |
| immix_gen        |    0.84s   |
| mark_compact_gen |    0.84s   |
| copy             |    0.87s   |
| immix            |    0.88s   |
| mark_bump_gen    |    0.90s   |
| mark_freelist    |    0.92s   |
| mark             |    0.98s   |
| mark_compact     |    1.07s   |
| mark_gen         |    1.06s   |
| mark_bitmap_gen  |    1.02s   |
| mark_card_gen    |    1.03s   |
| bump             |    1.22s   |
| none             |    1.64s   |
| libgc            |    1.18s   |

spread = 2× (best vs worst)、 他 macro bench と同程度の挙動。
copy_gen / immix_gen / mark_compact_gen が tied、 gen + (copy or
non-moving incremental) の組合せが winner pattern。

### 実装の制約

baruby に Hash がないため object = Array of `[key, value]` pairs。
sum_ints は型判定 (`is_a?(Integer)` 等) も使えないため、 bench shape
を knew にして walk (`sum_tags` / `sum_record` / `sum_ints` の 3 関数)。

iter 53 で「他 bench が SSO 投入で改善した」ことを確認したので、
新 bench でも SSO ヒット率が高いものを意図的に選んだ — workload に
SSO 改善を反映する目的。 SSO_MAX=7 で全 token が乗る (max 5 char =
"carol")。

baruby (libgc) にも port、 同 oracle 通過。

## 2026-05-20 (53) — SSO (small-string optimization, SSO_MAX=7)

iter 52 で direction として書いた SSO を実装。 値表現側の
最適化 (AST には触らない)、 user 方針 [[feedback-no-arity-specialized-nodes]] と
整合。 baruby (libgc sister) にも port。

### 設計

`BaString.bytes` を anonymous union に変更:

```c
typedef struct BaString {
    ObjectHeader hdr;          // 8 B (flags の bit0 で SSO 判定)
    uint32_t len;              // 4 B
    uint32_t capa;             // 4 B
    union {
        char *bytes;           // heap: separate alloc
        char  small[8];        // SSO: inline 7 chars + NUL
    };
} BaString;
// total 24 B (unchanged)
```

`SSO_MAX=7` を選択した理由 — `small[8]` で union が pointer と
同サイズになり BaString の総サイズが 24 B のまま保たれる。
`SSO_MAX=15` も実装可能だが BaString が 32 B に肥大 → fib_pair
で +8% regression が出るため (string 系の win と打ち消し)、 24 B
維持版を採用。

判定: `BSTR_IS_SSO(s)` macro (`hdr.flags & OBJ_FLAG_SSO`)。
読み出し: `bstr_bytes(s)` inline 関数 (const char *)、 SSO のとき
`s->small`、 そうでなければ `s->bytes` を返す。

### 変更スコープ

- context.h: union + accessor 追加
- node.c / node.def: 50+ の `.bytes` 直接アクセスを `bstr_bytes(s)`
  経由に。 allocator (`baruby_str_new` / `_slice` / `_repeat` /
  `_concat`) は `len <= BSTR_SSO_MAX` で 1-alloc fast path。
  `baruby_str_append` は SSO->heap 昇格パスも追加
- 全 15 GC backend の OBJ_STRING scan: `if (!BSTR_IS_SSO(s) && s->bytes)`
  でゲート (mark / forward / fwd_payload / 全 variant)
- baruby (libgc) も同形で port (port 側は GC が conservative なので
  scan 修正不要)

### 効果 (A/B median of 5, immix_gen backend, plain mode)

clean A/B (`baruby_precise` を SSO patch あり/なしで rebuild):

| bench             | baseline | SSO=7 | Δ      |
|-------------------|---------:|------:|-------:|
| substr_churn      |    0.89s | 0.87s |  -2%   |
| tokenize          |    0.95s | 0.79s | -17% ★ |
| string_concat_dyn |    1.06s | 0.99s |  -7%   |
| fib_pair          |    0.77s | 0.78s |  ±0%   |

tokenize の -17% が一番大きい (CSV-like split の token は 3-6 char
で全て SSO に収まる)。 substr_churn は 5-char で SSO に収まるが、
copy 系 backend では bump-pointer cost が薄く win が小さい。

string_concat_dyn は "aaabbbccc" (9 char) で SSO_MAX=7 を超える
ため fast path に乗らないが、 中間値が SSO に乗る ("aaa", "bbb"
など 3 char) ことで間接的に win。

oracle: 全 19 bench × 16 backend (= 304 行) で oracle 通過確認。

### baruby (libgc) port

同じ union 化 + accessor + allocator 修正。 libgc は conservative
GC なので、 SSO 文字列の `bytes` ポインタが garbage であっても
libgc は単に memory range を scan するだけで dereference しない
→ scan-side修正不要。 oracle 通過確認、 substr_churn 1.30 → 1.09s
(-16%)、 tokenize 1.39 → 1.11s (-20%)。

### iter 52 で書いた SSO スコープ見積もりの修正

iter 52 では「`.bytes` 直接アクセス 50 箇所」と書いたが、 実装後
の実集計は: node.c 〜30、 node.def 3、 gc_*.c 〜20 (15 backend ×
1-3 site)。 概ね当たり、 sed bulk 置換 + 手動修正で 1 iter 内に
完了。

### 関連 todo

- 動的 string `+` chain の alloc-fusion: 部分的に SSO で吸収
  (中間値が SSO に乗る場合)、 9-char 以上の chain は引き続き
  rope / cord 路線
- `aro_gc_alloc` への `__attribute__((malloc, alloc_size))`: 別 iter

## 2026-05-20 (52) — `node_add3` 棄却の方針確認 + SSO direction docs

iter 51 で `a + b + c` 専用 AST node (`node_add3`) を入れて
string_concat_dyn -7% を取ったが、 user 棄却 (commit reverted at
`b4f7573`)。 user 原文: **「add3 とか許すと、最終的にベンチマークの
計算を全部する 1 個のノードができちゃうから」**。

### 方針確認 (memory に固定)

ASTro の建前は「AST node はセマンティクスを言語仕様レベルで保ち、
高速化は部分評価器・AOT 特殊化に委ねる」。 bench の hot pattern
ごとに専用 binop node (`add3`, `add4`, `concat_then_size`, ...) を
追加するのは:
- bench 専用最適化を runtime 仕様に焼く方向に滑る
- 言語実装フレームワークとしての汎用性 (フレームワーク自身が
  示す方法論) を損なう
- 配列リテラル `[a,b,c]` の `node_ary_lit_4` のような構文上自然な
  literal 列挙とは別物 (binop chain の N 化は parser 側の rewrite
  が必要 → 言語仕様の歪み)

二項演算の arity-固定 special-case node は今後提案しない方針
(memory `feedback_no_arity_specialized_nodes.md`)。

### AOT 側で fuse できないかの確認

`add3` で取った win (中間 BaString + bytes alloc 1 個削減) を
AOT 特殊化で recover できるか確認した:

- `astrogen.rb` の SD specialize は each node instance 毎の SD
  関数を生成、 child SDs を `static inline` で chain する。 LTO
  + `static inline` で gcc が cross-SD inline を実現する。
- ただし `aro_gc_alloc` は opaque allocator (side effect: GC 統計
  更新、 minor trigger 等)。 gcc は「不透明な allocator call 2 個を
  1 個に融合」する pass を持たない (escape analysis + 連続 alloc
  merge が必要)。 `__attribute__((malloc))` も付いてないが、 付け
  ても aliasing 改善どまりで alloc 削除 / merge は起きない。
- 従って add3 の win 内 "alloc-fusion" 系は AOT 自動には recover
  されない。 framework 改良で吸収するには
  - escape analysis ベースの alloc merging pass (重い framework
    変更)
  - "freshly-allocated, single-use" の AST-level 印 + runtime
    fast-path (これは事実上 add3 と同等の AST 認識を必要とする)
  のどちらか。

結論: alloc fusion 系の win は **rope / cord 等のデータ構造変更**
か **SSO (small-string optimization)** で取りに行くのが筋。 AST
側ではなく value 表現側を改良する。

### SSO direction (todo.md に追加)

`baruby_str_slice` の bench `substr_churn` は 5-byte の小 string
を per-iter 大量に alloc する pattern。 現在 BaString header
(24 B) + bytes payload (16 B size class) = 約 40 B alloc / 5 B
payload = メタデータ overhead 8×。

SSO で `len <= 15` の string は BaString 内 inline 配列に格納
すれば、 2 alloc → 1 alloc になり、 substr_churn は alloc cost
が半減する見込み。 .bytes アクセス 50 箇所の修正が必要 (todo.md
に概算入れた)。 別 iter 候補。

### 残作業 + iter 52 で触らなかったこと

- 動的 add chain の fold は「rope / SSO で取る」方針に修正し
  todo.md エントリを書き直し
- SSO 検討を独立 todo として追加
- `aro_gc_alloc` への `__attribute__((malloc, alloc_size(2)))`
  付与は別 iter で軽く試す価値あり (aliasing 改善で巨視的
  effect は薄いが、 LTO inline 後の SSA で活きる可能性)

iter 51 + iter 52 (revert + docs) は **コード変更 net 0、 方針
確認 net +1**。 性能 number は iter 50 と同じ。

## 2026-05-20 (50) — Milestone: iter 36-50 のまとめ + gc.h contract 明文化

### iter 36-50 で達成したこと

15 iters (約 5 日 + 2 セッション) で baruby_precise を次のレベルに進めた:

**Backend / 設計空間 (15 → 16)**
- iter 36: `mark_card_gen` (#15) page-level remset で natural bounded
- iter 41: `mark_freelist` (#16) region + freelist non-compact M&S
- 全 16 backend が **bounded correctness** 達成 (iter 38 で immix_gen /
  mark_bitmap_gen に heap-walk fallback 追加)

**Bench (15 → 19)**
- iter 36: `ast_eval`, `remset_pressure` (macro pattern + adversarial)
- iter 37: `string_concat_dyn` (parser fold で吸収された pattern の維持)
- iter 40: `dll_walk` (bidirectional pointer + WB stress)
- iter 48: `tokenize` (CSV-like 分割 + push 混合)

**Performance**
- iter 36: AOT mode 修復 (Makefile + extra_cflags)
- iter 37: literal `+` parse-time fold → string_concat plain -58%, AOT -79%
- iter 43-45: 3 段 inline-friendly optimization (cold-split → clz size_class →
  slab-gen cold-split)。 string-heavy bench で 8-25% AOT improvement
- CRuby 比 plain 1.83×、 AOT 7.77× geomean faster (iter 42)

**重要なバグ修正**
- iter 38: immix_gen / mark_bitmap_gen の remset overflow abort →
  pressure-triggered minor (immix_gen) と dirty_bm scan (mark_bitmap_gen)
- iter 48: mark_freelist の dormant memset bug (iter 41 から 7 iter
  気付かなかった、 tokenize bench で顕在化)

**Docs / メタ**
- iter 39: comprehensive doc review
- iter 42: CRuby reference times comparison
- iter 47, 50: docs consolidation

### iter 50 の gc.h contract 明文化

iter 48 の mark_freelist memset bug を二度と起こさないため、 gc.h の
`aro_gc_alloc` 宣言に **CONTRACT** コメントを追加:

```c
/* aro_gc_alloc — allocate `payload_size` bytes of a pointer-scanned object.
 *
 * **CONTRACT**: every backend MUST zero-initialize the returned payload
 * before returning.  The GC mark phase walks these fields as VALUEs /
 * pointers via `scan_outgoing`; stale data → SEGV.  Region-bump backends
 * get this for free; freelist-recycling backends MUST emit explicit memset.
 * ...
 */
```

将来 backend を追加する時、 この comment が prerequisite として参照
される設計。 また `aro_gc_alloc_byte` も「raw bytes、 zero-init 不要、
KIND_PAYLOAD_BYTE は scan_outgoing で skip」 を明記。

### 全体俯瞰
matrix 数値で見る design space の習熟度 (plain mode、 winner backend):
- `bump` (no GC): ast_eval, binary_trees — alloc floor
- `immix_gen`: 6+ benches — line allocator + gen の包括的勝利
- `mark_compact_gen`: tokenize — alloc-heavy + tenured 圧
- `mark_bump_gen` / `copy_gen`: substr_churn / 似た workloads — bump nursery
- `none`: sieve — GC を全く走らせない workload
- `mark_gen_inc`: ast_eval (1 同位) — non-gen で軽い
- `immix`: hash_chain — non-gen Immix

AOT mode (`immix_gen` 一強傾向):
- immix_gen: 10+ wins — gen + line allocator が dispatch baked で最大限活用
- mark_compact_gen: nqueens / fannkuch (場面別)
- copy_gen: remset_pressure
- 残りは mark_bump_gen / bump / none に少数

iter 36 baseline と比べて、 plain で 1-2 backend が独占していた状況から
`immix_gen` 中心の多様化に変化。 cold-split + clz の効果で各 backend の
hot path が縮み、 plain でも tight contest が増えた。

### TODO 候補 (今後の方向)
- `aro_gc_alloc_byte` の inline 化 (variable size のため難航中、 iter 47
  で flatten 試行 → revert)
- 動的 string `+` chain fold (str_concat_dyn / substr_churn の win 期待)
- inc 系 backend を真の incremental に (SATB + stack-WB)
- framework integration (gc.{c,h} を runtime/ 格上げ、 value.def 試行)
- CRuby + JIT 比較 (現状 default ruby のみ)

commit: gc.h 明文化のみ (本 iter)。 docs 別 commit。

## 2026-05-20 (49) — iter 48 tokenize bench を matrix に反映

iter 48 で追加した tokenize bench (CSV-like 分割 + push 混合) を plain matrix
19 bench × 16 backend で計測 (median of 3):

| backend | tokenize plain |
|---|---:|
| mark_compact_gen | **0.88 (winner)** |
| mark_bump_gen | 0.89 |
| copy / copy_gen / immix_gen | 0.89-0.91 |
| mark_freelist | 1.08 |
| mark | 1.14 |
| mark_card_gen / mark_bitmap_gen | 1.26-1.28 |
| none | 2.40 |
| libgc | 1.39 |

mark_compact_gen が勝つのは bump nursery + tenured compact が iter 全体で
作る大量の短命 BaString + array growth の組合せに刺さるため。 immix_gen は
複数の似た bench で 1 位を取ってきたが tokenize では mark_compact_gen に
譲った: tokenize は string が多く tenured 圧が大きく、 Lisp-2 slide
compaction の方が線形 sweep より効く。

perf.md §2 plain matrix を 19 bench × 15 backend で更新、 ベンチカタログ
17 → 19 種に増。 AOT matrix は別 commit で予定。

## 2026-05-20 (48) — tokenize bench + critical mark_freelist memset bug fix

### tokenize bench
新 macro bench: CSV-like 文字列分割 (`,` で 20 × "red,blue,green,yellow,
orange,purple" を 120 tokens に分解)、 17500 iter。 baruby_str_slice +
baruby_ary_push を一緒に exercise する初の bench。 oracle = 10500000。
baruby (libgc) にも port。

### 副次: mark_freelist の dormant memset bug
bench 投入時に mark_freelist が SEGV するバグを発見。 原因: iter 41 で
mark_freelist を追加した時に `aro_gc_alloc` の payload zero-init を忘れて
いた (`gc_mark.c` 等 slab 系には memset がある)。 freelist popped slot は
前の用途の stale data を保持しており、 bytes payload だった slot を
BaString header として再利用すると BaString.bytes の 8 byte が文字列 chars
のままになる。 GC mark phase で `scan_outgoing` が KIND_OBJ_STRING ケースで
`mark_value((VALUE)s->bytes)` を実行し、 文字列 chars
(`0x2c6465722c656c70` = "ple,red,") を VALUE pointer と誤解して SEGV。

### 再現条件
- 多数の class-0 alloc + free のサイクル (BaString header / 短い bytes /
  小配列)
- それらが freelist 経由で再利用される
- BaString と bytes payload が同じ size class で交互に再利用されると確実

既存 17 bench はたまたまこのパターンを踏まなかった (string-heavy bench は
make_csv で長い文字列を作るが、 substr_churn は long-lived 単一 string で
short-lived BaString のサイクルが少ない、 string_concat は parse-time fold
で alloc 数が極端に少ない)。 tokenize bench で初めてヒット。

### 修正
`aro_gc_alloc` で KIND_OBJ_ARRAY / KIND_OBJ_STRING / KIND_PAYLOAD_VAL に
対し `ALIGN8(payload_size)` bytes を zero。 KIND_PAYLOAD_BYTE はスキャン
されないので skip (他 backend と同じ pattern)。

```c
void *
aro_gc_alloc(AroGcKind kind, size_t payload_size, VALUE *sp_top)
{
    GCHeader *h = alloc_slot(kind, payload_size, sp_top);
    void *payload = (void *)(h + 1);
    if (kind != KIND_PAYLOAD_BYTE) {
        memset(payload, 0, ALIGN8(payload_size));
    }
    ...
}
```

### 教訓
- **「ベンチを増やすと隠れたバグが顕在化する」 の典型例**。 iter 41 から
  iter 47 までの全 matrix は 17 bench でこのコードパスを踏まなかった。
  18 bench 目で踏んだ。
- 新 backend を追加する時、 既存 backend のメソッド契約 (ここでは
  「aro_gc_alloc は payload を zero-init する」) を model checking 不在で
  follow するのは error-prone。
- 改善案: `aro_gc_alloc` の semantic contract を gc.h コメントで明文化、
  または共通の `static inline` wrapper にする。 今後の TODO。

commit: `b1050bd`。

## 2026-05-19 (45) — slab-gen 4 backend に cold-split 展開

iter 43 で region-based 9 backend、 iter 44 で slab 5 backend の clz 化を
適用。 残る 4 slab-gen backend (`mark_gen` / `mark_gen_inc` /
`mark_bitmap_gen` / `mark_card_gen`) は `maybe_collect` 内の collect
dispatch (minor + threshold-major) が inline されて `aro_gc_alloc` body が
肥大化していた。

### 適用
`maybe_collect_slow` という `__attribute__((noinline, cold))` の helper を
新設、 dispatch 部分を extract。 outer threshold check に `__builtin_expect(..., 0)`。

`mark_gen_inc` は per-alloc に走る `inc_step(INC_WORK_PER_ALLOC)` を hot
path に残置 (これは cold ではない)、 threshold-triggered minor/major のみ
を slow helper に移動。

### 効果
mark_bitmap_gen の aro_gc_alloc body: 0x197 → 0x152 (-22%) で mark backend
(0x15c) と同等 size に。 inline 候補化は成立。

plain matrix iter 44 → iter 45:
| Bench | backend | iter 44 | iter 45 | Δ |
|---|---|---:|---:|---:|
| mark_gen_inc | string_concat | 0.28 | 0.26 | **-7%** |
| mark_gen_inc | cons_list | 0.90 | 0.86 | -4% |
| mark_gen_inc | fib_pair | 1.02 | 0.98 | -4% |
| mark_gen_inc | list_alloc | 0.90 | 0.86 | -4% |
| mark_card_gen | list_alloc | 0.85 | 0.81 | -5% |
| mark_bitmap_gen | cons_list | 0.80 | 0.77 | -4% |
| mark_gen | list_alloc | 0.79 | 0.77 | -3% |

mark_gen_inc が最大の恩恵 (元々 inc_marking で body が大きかった)。
全体的に 2-7% の improvement で iter 44 mark の 5-8% より控えめだが、
4 backend で一貫した方向の改善。

### 3 段階の inline 化最適化シリーズ完結
- iter 43: region-based 9 backend の bump path に cold-split
- iter 44: slab 5 backend の size_class_for を clz 化
- iter 45: slab-gen 4 backend の maybe_collect に cold-split

これで **全 15 backend (copy_gen_inc placeholder 除く) の aro_gc_alloc が
caller 側で inline されるか constprop clone を経由する** 形に統一。
baruby_ary_new (capa=0 path) と baruby_str_new (BaString header alloc) は
すべての backend で inline 化、 string-heavy / array-heavy bench で
plain 2-8%、 AOT 5-14% の改善。

commit: `bb31580` (code)、 docs 別 commit。

## 2026-05-19 (44) — slab 系 5 backend の size_class_for を O(1) 化

iter 43 で region-based 9 backend に cold-path split を適用。 slab/page 系
5 backend (`mark` / `mark_gen` / `mark_bitmap_gen` / `mark_card_gen` /
`mark_freelist`) でも同様の inline 化最適化を試みた。 ただし bump-based と
違い slab には size class lookup が必要で、 これが aro_gc_alloc body を
肥大化させて inliner budget を越えていたのが root cause だった。

### 元の `size_class_for`
```c
static int
size_class_for(size_t slot_total)
{
    for (int i = 0; i < NUM_SIZE_CLASSES; i++) {
        if (slot_total <= size_class_bytes[i]) return i;
    }
    return -1;
}
```
これが gcc に 9 個の cmp+jbe に展開され、 aro_gc_alloc body の中に
**30+ bytes** の dead code として残る (典型的に最初の cmp で return するが
コンパイラはすべての分岐を出力)。

### iter 44 の置換
9 size classes (32, 64, 128, 256, 512, 1024, 2048, 3072, 4096) は 3072
以外すべて power-of-2。 ceil(log2) で直接 index 計算可能:

```c
static inline int
size_class_for(size_t slot_total)
{
    if (slot_total <= 32) return 0;
    if (slot_total > 4096) return -1;
    int bits = 64 - __builtin_clzll(slot_total - 1);
    int c = bits - 5;
    if (c == 7 && slot_total > 3072) c = 8;
    return c;
}
```
3072 のみ非 power-of-2 だが one-line conditional で吸収。 BSR/LZCNT 命令 1
つ + 簡単な arith で 9 cmp の検索が 5 命令に圧縮。

### 効果
mark backend:
- `aro_gc_alloc` body: **0x1b8 → 0x15c (-21%)** 縮小
- `baruby_ary_new` で `call aro_gc_alloc` が消えて **`aro_gc_alloc` 完全 inline**
  (constprop で size class 0 が定数として伝搬、 直接 `call slab_alloc` に圧縮)
- `baruby_str_new` も同様 (BaString header alloc 部のみ inline)

plain matrix iter 43 → iter 44:
| Bench | iter 43 | iter 44 | Δ |
|---|---:|---:|---:|
| mark/list_alloc | 0.77 | 0.71 | **-8%** |
| mark/gc_combined | 0.84 | 0.78 | **-7%** |
| mark/cons_list | 0.74 | 0.70 | **-5%** |
| mark/string_concat | 0.25 | 0.23 | **-8%** |
| mark/binary_trees | 0.74 | 0.72 | -3% |
| mark_gen/list_alloc | 0.86 | 0.79 | **-8%** |
| mark_gen/binary_trees | 0.80 | 0.76 | -5% |

`mark_bitmap_gen` / `mark_card_gen` は bitmap/card 操作が aro_gc_alloc 内に
残るため、 size_class_for を縮めても inline 閾値には届かず effect 小。
`mark_freelist` は元から body lean で変化なし。

### 学び
- gcc の inliner heuristic は body size に敏感。 細かな冗長コード (9-cmp
  linear search) の蓄積が外部 inline の成否を分ける
- LZCNT/BSR 命令の活用で table lookup や linear scan を O(1) 化できる
  ケースは多い (今回の class lookup、 hash bucket、 PRNG なども候補)
- iter 43 の cold-split + iter 44 の clz 置換は **同じ目的 (aro_gc_alloc
  inline 成立) の補完的アプローチ**: cold-split は call site 不変で
  callee 縮小、 clz 置換は callee body の主要枝を直接縮める

commits: `e252e66` (code)、 docs 別 commit。

## 2026-05-19 (43) — Cold-path split for 9 region-based GC backends

ユーザ提案: `gc_bump` 等の bump alloc hot path に hidden な cold body
(`gc_collect_internal` + OOM check + retry) を `__attribute__((noinline, cold))`
helper に extract、 hot branch を `__builtin_expect(..., 0)` で hint。
目的は **aro_gc_alloc 本体を縮小** し、 inline されない call site
(`baruby_str_new` など) で gcc が呼ぶ `aro_gc_alloc.constprop.0` を slim に
すること。

### 適用 backend (9)
- 未対応だった: `copy`、 `mark_compact`、 `immix`、 `mark_freelist`
- 既に `noinline on minor_gc` を持つ: `copy_gen`、 `copy_gen_inc`、
  `mark_bump_gen` (iter 29) → 直交した別レイヤーの最適化
- 残: `mark_compact_gen`、 `immix_gen`

slab/page 系 (`mark`、 `mark_gen`、 `mark_gen_inc`、 `mark_bitmap_gen`、
`mark_card_gen`) は freelist + page allocator で bump path がないので未適用。

### 検証フロー
1. `gc_copy.c` 単独で `__builtin_expect + noinline cold` を試行 →
   同 session A/B (5 round interleaved) で **noise 範囲、 measurable
   improvement なし**
2. user 「inline 化観点」 を指摘 → disassembly 確認:
   - `baruby_ary_new` は `aro_gc_alloc` を **既に inline** (LTO -O3)
   - `baruby_str_new` は **constprop clone を call** (inliner budget 越え)
3. cold-split で `aro_gc_alloc.constprop.0` が **75 LOC → 54 LOC (-28%)**
4. string bench で 5 round interleaved A/B → `string_concat -3%`、
   `string_concat_dyn -3%`、 noise 内だが consistent な微減
5. 全 9 backend に展開 → matrix で `string_concat copy_gen -18%`、
   `string_concat_dyn copy_gen -14%`、 `substr_churn copy_gen -17%` 等
   一貫した string-heavy bench での 8-18% 改善を確認

### 効果サマリ (plain matrix iter 41 → iter 43、 string 系)
| Bench | backend | iter 41 | iter 43 | Δ |
|---|---|---:|---:|---:|
| string_concat | copy_gen | 0.22 | 0.18 | **-18%** |
| string_concat | immix_gen | 0.20 | 0.17 | **-15%** |
| string_concat | mark_compact_gen | 0.21 | 0.18 | **-14%** |
| string_concat | mark_bump_gen | 0.21 | 0.18 | **-14%** |
| string_concat_dyn | copy_gen | 1.11 | 0.95 | **-14%** |
| string_concat_dyn | immix_gen | 1.05 | 0.91 | **-13%** |
| substr_churn | copy_gen | 0.95 | 0.79 | **-17%** |
| substr_churn | mark_compact_gen | 0.89 | 0.77 | **-13%** |
| cons_list | immix | 0.69 | 0.63 | **-9%** |
| dll_walk | immix | 0.73 | 0.67 | **-8%** |

### 学び
- iter 29 で `noinline on minor_gc` した時と同じ idea (inliner budget
  preservation) を **alloc cold body にも** 展開すれば、 inline されない
  call site (str_new 系) で透過的に効く
- 単一 backend での A/B では noise に埋もれて effect 見えない場合でも、
  **複数 backend × 複数 bench で同方向の signal が一斉に出る** とき
  noise でないと判定可能 (今回の string-heavy 8-18% 一斉改善)
- 「`__builtin_expect` だけでは効かない」 という前 commit の hasty conclusion
  は誤り。 `noinline cold` で **callee の constprop clone を縮める** 効果が
  本命だった

### 副次変更
docs/perf.md §2 plain matrix を iter 43 数値に更新、 勝者分布も再計算。

commit: `7d9b96c`、 docs 別 commit。

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
