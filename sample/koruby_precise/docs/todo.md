# todo.md — koruby Ruby 互換性ギャップ

[done.md](./done.md) は実装済み機能の一覧。 ここは **未実装 / 不完全 /
既知バグ** の作業リスト。

## Ruby 準拠 probe sweep (2026-06-20 session 4) — 修正済み

probe + 実ハーネス array spec で発見・修正 (corpus 89295/5 維持・STRESS+AOT 検証):
- **&obj block 引数の to_proc coercion + Symbol/Method#to_proc 転送**: `&method`,
  `&proc_var`, `def m(&b)` 経由の C-proc。block-trio に NODE* entry を持てない
  proc (iseq==NULL) を KORB_BLK_CPROC sentinel + KORB_BLK_FWD plumbing で運び、
  korb_block_yield 冒頭で捕捉して send dispatch (GC-safe、*captured_self から
  fresh 読込)。Method#to_proc 追加 (is_lambda + self=recv で symbol proc と判別)。
- **instance_exec / instance_eval (block form)**: self を receiver に再束縛、
  def_env 保持で closure 維持。CPROC block は固定束縛なので self-rebind skip。
  String 形式と singleton def は未対応。
- **Integer#fdiv(Integer)**: bignum/大 Fixnum 被除数を exact rational (mpq) で
  割り正しい subnormal (1.fdiv(10**323)=1.0e-323)。double/double は桁落ちで 0.0。
- **Integer#size**: Bignum で |self| のバイト数 (mpz_sizeinbase)。従来は常に 8。
- **Bignum#to_f round-to-nearest**: mpz_get_d は toward-zero truncate。nextafter
  + mpz 距離比較で nearest-even。(10**308).to_f=1.0e308。

**性能: bottom-header 化 (self-copy 排除 + magic 同居) — foundation 済、本体は残 (2026-06-21)**
狙い: self を base[-1] に常駐させ korb_invoke の self-copy (`base[fs-1]=self`) を排除 (~0.5%、
free に近い) + EP を base[-2]・magic を base[-3] に同居 (CRuby の "magic" 流)。
- [済] codegen: `@children` ノードに `@framehdr` で **先頭メタを KORB_FRAME_HDR(=2) 個予約**
  (dispatcher が `slots += cnt+HDR`、予約セルは GC 走査前にゼロ化)。node.h に KORB_FRAME_HDR。
  未使用なので no-op・green。
- 残 (結合・GC/offset クリティカル。新規セッションで集中して):
  1. `@framehdr` を 8 call ノードへ (node_call/send/kw/send_safe/call_blk/call_blkproc/
     send_blk/send_blkproc)。base[-2]/base[-3] 予約+ゼロ。
  2. **self_off を base[-1] に**: node_self 等の self_off に **bake_add を足す**
     (bake は frame_size を引く → `-1-chain` が `-1-fs-chain` = base[-1] に解決)。self_off を
     使う全箇所 (PM_SELF_NODE / 各 self-stage / super / def / attr / defined / massign /
     call_splat / send_blk / ivar) を rebake。frame_size から self セル(+1)を除去。
  3. EP を base[-2] へ: korb_invoke は base[-1]=0 をやめる (base[-2] は予約時ゼロ済)。
     node_eget/eset の chain walk を `node[-1]`→`node[-2]`。korb_make_proc/binding/
     korb_frame_escaped を base[-2]。self-copy (`base[fs-1]=self`) 削除。
  4. 内部 dispatch: korb_send_impl/korb_call_impl で [recv,args] を 2 上げて base[-2]/base[-3]
     を予約+ゼロ (全 ~36 内部サイトを一括カバー)。super・Object.new→initialize も予約。
  5. frame setup: korb_block_yield (self=bf[0]=base[-1]・EP=bf[-1]=base[-2])、eval frame、
     c->slots (toplevel)、fiber vslots ── いずれも先頭スラックを **3 セル** に、走査も -3 から。
  6. magic を base[-3] に (型/フラグ/署名、debug-gated 可)。
  7. 検証: corpus / STRESS+PURGE (closure/binding/fiber 網羅) / AOT / ベンチ。

**性能: TOPLEVEL_BINDING の return 課税 → per-frame EP cell (設計 A) で解決済 (2026-06-21)**
- open env を各 frame の base[-1] (= 受け手スロット) に置き、return は `korb_frame_escaped
  (base)` で自分の base[-1] だけ見る (グローバル不読)。open_envs/open_env_cnt/register/
  close_envs 全廃 (env は base[-1] の slot 走査で GC-root)。
- 全 call 形態が self/recv を base[-1] に積む (node_call/kw/splat/blk/blkproc は self を
  argv[0] に、super は restage、eval は fb=slots+1、toplevel/fiber は先頭スラック)。
  node_eget/eset は mixed-chain。korb_make_proc/binding は base[-1]=E (E->prev に外側リンク)。
- 結果: nested_loop 6.34→0.59、while2 4.83→0.24、ackermann 5.69→1.39、method_call
  3.65→1.21 (vs YJIT)。TOPLEVEL_BINDING は eager のまま。corpus 89295/5、STRESS+AOT green。
- 拡張余地: base[-2]=magic (型/フラグ/署名・健全性 = CRuby の magic) を載せられる (未実装)。

**defer (発覚、未着手):**
- *literal* `&:sym` block が unary 専用 (kp_symbol_block が `x.sym()`、追加引数を
  転送せず)。`reduce(&:+)` 不可 (`reduce(:+)` は可)。n-ary 化は send_splat entry
  構築が SEGV、or C-proc 経由 (per-call alloc) で要再挑戦。
- **UnboundMethod#bind_call / Method#call が名前 re-dispatch** (captured entry を
  直接 invoke しない)。BasicObject 跨ぎ bind や override 下で誤った実装を呼ぶ。
- **to_enum / enum_for**: eager enum へ collector で materialize する設計が要 (C
  collector block 機構)。enumerable probe の cascade blocker。
- 実ハーネス array_004/005 の 5 fail は hard: pack 'P'/'p' (pointer、platform
  依存)、shuffle/sample (MT 完全一致要)、`upto(Float::INFINITY)` (lazy 無限
  enum + zip lazy take)。

## Ruby 準拠 probe sweep (2026-06-19 session 3) — 修正済み

差分テスト probe で発見・修正 (全て corpus 89267/5 維持・STRESS+AOT 検証):
- **String#unpack / Array#pack 'U'** (UTF-8 codepoints)。
- **BasicObject + Kernel を MRO に** (ancestors / is_a? / Object.superclass)。
- **begin/rescue の else 節** (seq(body, else) に lowering)。
- **String#reverse / reverse!** を UTF-8 文字単位に (byte 反転バグ修正)。
- **Array#each_index / cycle(n)** の no-block Enumerator。
- **無名 splat target** `first, *, last = ...` (中間を synth local に捨てる)。

### 残る準拠ギャップ (hard / niche / out-of-scope)
- (実装済 2026-06-19) **define_method** (closure 含む)。真因は env staleness では
  なく **block-arg の def_env が tagged prev 形 (base|1)** で来るのに korb_make_proc が
  raw base を期待していたこと → tag mask で解決。force-close は不要 (shared closure を
  壊す red herring)。env は共有 open env → 定義フレーム exit で heap promote。
  KORB_METHOD_DM kind + dm_proc (GC forward) + KORB_C_CLASS に登録。
  **Class.new/Module.new も修正** (new_kind=1 で generic object 化していた → kind 2)。
- **op-aware no-block Enumerator**: select/reject/flat_map/transform_values を
  block なしで呼ぶと、後続 .with_index{} が filter でなく map になる (silent 誤答)
  → 現状は raise (honest)。正しくは Enumerator に op を保持させる必要。
- **to_enum / enum_for(:method)**: 任意メソッドを collecting block で駆動する機構が要る。
- **upcase/downcase の非 ASCII** (é→É 等): full-unicode case mapping、scope 外。
- **regex** (gsub/sub/scan/match の Regexp 形): astrorge 待ち。
- **Integer.superclass == Numeric**: Numeric を module→class 化 + 階層変更、risky・低価値。

## perf: 全マイクロベンチで CRuby+YJIT 超え (2026-06-19 進行中)

目標: aot+cached が **すべての** microbench で cruby+yjit を下回る (<1.00)。

### 本セッションで勝ち越したベンチ
- floatcalc 1.21→0.97, mathfn 1.22→0.98: mixed Float·Integer 算術を node.def の
  四則 (plus/minus/mul/div) に inline (korb_num_arith/num_to_d/float_new の PLT 回避)。
- gcd 1.39→0.97, send 1.13→0.86, object 1.07→0.94: call fast-path を SD に inline。
- gen_gc 0.99 (勝ち越し)。

### call fast-path を SD に inline (commit ebb4996c)
- korb_invoke_simple を node.h の always_inline 化 → code_store SD にも畳まれる。
  node_call/node_send が cache-hit 時に korb_call_cached/korb_send_cached への
  cross-module call を回避。korb_inlcache に kind 弁別子 (INSTANCE/SMETHOD/NEW) 追加。
- 効果: fib 1.86→1.39, ackermann 2.32→1.63, tak 2.09→1.51, method_call 1.66→1.11,
  ivar 1.94→1.39, structacc 1.71→1.34。

### 残る負け (structural, tree-walker の限界に近い)
- 再帰/per-call cluster: fib 1.39, ackermann 1.63, tak 1.51, binary_trees ~2.0,
  ivar 1.39, structacc 1.34, method_call 1.11。残コストは per-call の indirect
  body dispatch (`*body->head.dispatcher`) + 16-byte RESULT 返り + argc/stack check。
  YJIT は tiny method を loop に inline + native register call。tree-walker で
  <1.0 にするには method body の cross-call devirtualize (自己再帰の直接呼び等)
  が要る。要 framework 改造、難。

### 最大の単独勝機: kwargs hash-free (現状 2.56、未着手)
- `box(x:,y:,z:)` が呼び出しごとに Hash を heap alloc → callee で korb_hash_find
  抽出。YJIT は hash 無しで kw slot に直接渡す。
- 設計: call site が「callee の signature を知らない」ため callee-decides 方式。
  - 新ノード node_call_kw/node_send_kw: pos args + kw 値を stack に staging、
    kw_syms(mid配列) を baked operand で持つ。Hash を作らない。
  - callee (korb_invoke_method 変種): kw params があれば stack の (sym,value) を
    slot に直接 bind。**kwrest があれば必要時のみ hash 構築。kw params 無し
    (&& **rest 無し) なら CRuby 通り positional Hash に変換 (fallback、hot では稀)。
  - 空 kwsplat drop (FL_KWARGS, [[project_koruby_kwsplat]]) との整合に注意。
- 大物・risk 高。要: parser + dispatch + node.def + STRESS 検証。

### その他の負け (小〜中)
- casewhen 1.36: case/when chain が node 列 (各 indirect dispatch)。dense int when
  を jump table 化する余地 (要 parser で node_case_jump 検出)。
- ary 1.23 / array_access 1.07: `a<<x` / `a[i]` が builtin receiver (Array は
  KORB_OBJECT_P でない) → 常に korb_send_cached + dispatch_method 経由。
  builtin-receiver CFUNC の inline cache 余地 (CFUNC 多様で汎用 inline 難)。
- sprintfb 1.12 / strfmt 1.00: format()/補間。bignum 1.11, mandelbrot 1.12。

## 差分テストで発覚した未対応 (2026-06-19 session 2 probe sweep)

- (実装済 2026-06-19) **パターンマッチ `case/in`**: value/binding/fixed-array/
  array-with-rest/hash(+shorthand)/capture(`=>`)/alternation(`|`)/pin(`^`)/guard
  (`if`/`unless`)/nesting + rightward `expr => pat`。既存の korb_pat_match +
  build_pattern_desc を流用、PM_CASE_MATCH_NODE を if/elsif chain に lowering。
  残: **find pattern** `[*, x, *]` のみ unsupported (稀)。
- (実装済 2026-06-19) **`Data.define`** (Ruby 3.2+ immutable value class)。
  positional/keyword 両対応 init + with + to_h/members/==/deconstruct_keys/inspect。
- (実装済 2026-06-19) **Bignum Rational 全面対応**: KorbRational num/den を VALUE
  (Fixnum or Bignum) 化 + GC edge + korb_int_arith 経由の overflow→bignum 演算
  + 既存の生 intptr 乗算 overflow バグも修正。リテラル `99999...r` も対応
  (node_rational_big、pm_integer→decimal via mpz_import)。
- (修正済 2026-06-19) **Bignum 整数リテラル**: node_bignum が source digits を
  焼いて eval 時に korb_str_to_int で再構築 (AOT も const char* operand で OK)。
  (Bignum Rational リテラル `99999...r` も対応済 — 上記参照)。
- (修正済 2026-06-19) **`Integer.sqrt`**: mpz_sqrt 経由で fixnum/bignum 厳密。
- (修正済 2026-06-19) **Enumerator#each_slice/each_cons**, **Hash#each_with_index
  (no block)**, **Hash key eql?** ({1=>x}[1.0]→nil)。

## 差分テストで発覚した未対応 (2026-06-19)

- **`define_method` 未対応** (class body): `define_method(:foo) { }` が
  「undefined method 'define_method'」。block/proc を method body 化する機構が要る。
  ※ 2026-06-19 に実装試行→revert。判明事項:
    - インフラ: korb_method に `VALUE proc` + KORB_METHOD_DM kind、GC は owner と
      並べて forward (context.h global-fn 行 + KORB_OBJ_CLASS 行)、korb_class_method_slot
      で proc=nil リセット。dispatch は korb_dispatch_method の DM case で
      korb_block_yield(entry=p->iseq, env=p->env, self=receiver)。
    - **非クロージャ block (cap_depth==0) は動く**。**クロージャ捕捉 block が
      captured 変数 read で SEGV**。同等クロージャの proc.call は動くので、原因は
      「define_method の CFUNC 内で作った proc を保存して後で呼ぶ」と captured env
      (open KorbEnv, loc=定義フレーム) が呼び出し時に無効化していること。
    - 想定 fix: DM に格納する際 captured env を即 CLOSE/promote (vals を stack から
      コピー) して loc 依存をやめる。要 focused session。
- **`Integer.superclass` が Object** (CRuby は Numeric)。koruby は Numeric を
  module(include) 扱いで superclass にしていない。Float も同様。階層変更は要注意。
- **`&blk` 引数転送の一部が prism node 139 で未対応**: `def m(*a, &b); x.send(n,*a,&b); end`。
- (修正済 2026-06-19) **multi-value `yield a, b`**: node_yield_n / node_yield_outer_n
  (@children) 追加。直接形 `def m; yield a,b; end; m{|a,b|}` は CRuby 一致。
  残: Enumerable 経由 (custom each が multi-yield → map/select) は prelude が
  `each{|x| yield(x)}` で1値しか転送せず誤り。完全対応は |*x|+yield(*x) が要るが
  hot path に array alloc が乗るので見送り (稀パターン)。yield-splat `yield(*a)` も未対応。
- **anonymous splat target 未対応**: `first, *, last = ...`(無名 `*`)が parse 不可。
- **`Object#object_id` 未対応**: moving GC 下で安定 id が要る(アドレスは移動で変わる)。
  即値は値ベースで可だが heap object は per-object id field か GC 管理の id 表が必要。
  中途半端な address ベースは `hash[obj.object_id]` を GC で壊すので保留。
- (修正済 2026-06-19) `require`/`require_relative`/`load` を no-op true 化、
  `Set[...]` クラス index コンストラクタ、`Set#disjoint?`/`#intersect?` 追加。
- (修正済 2026-06-19) Rational#floor/ceil/round、ArithmeticSequence に Enumerable
  mixin (sum/map/select 等が each 経由で動く)。
- niche 残: `Integer.sqrt` (class method plumbing 要)、`Complex#**`、
  pack/unpack の `l`/`L` 等の directive。いずれも稀。
- (修正済 2026-06-19) method_missing / respond_to_missing? / Object#instance_variables /
  Symbol#inspect の @ivar/@@cvar/$global を bare 表示。

- **blockless enumerator が一部メソッドで未対応**: `(1..10).select.with_index { }`、
  `Hash#each_with_index`(no block) 等が NotImplementedError (REQUIRE_BLOCK)。
  ブロック無し呼び出しで Enumerator を返す機構が要る (broad / 優先度3)。
  ※ map/each/select は一部 enumerator 化済 (map.with_index 等は動く)。
- (修正済 2026-06-19) **`round(half: :even/:up/:down)`**: Float/Integer 両対応。
  残: `2.45.round(1, half: :even)` が 2.4 (CRuby 2.5) — float の十進近似精度問題
  (scaled-multiply 方式の限界、decimal-string rounding が要る。default round にも
  同種の精度限界あり、稀)。
- **Unicode case mapping 非対応** (`"café".upcase` → "CAFé")。ASCII のみ。設計範囲外。

## (修正済 2026-06-18) SEGV: `exc = SomeError.new("msg"); exc.message`

- 原因: `ExcClass.new` が generic KorbObject を作っていた (new_kind が
  例外クラスを plain user class 扱い) → Exception#message の VAL2EXC が
  別 struct を誤 cast して SEGV。
- 修正: korb_class_new_kind が例外クラスを kind 2 に分類、korb_send_impl の
  mid_new で本物の KorbException を alloc (etype/msg/exc_class/user
  initialize)。無 msg 時の message は class 名。Class#name/to_s/inspect 追加。

## 共有 runtime への koruby 専用コード漏れ (2026-06-12 発覚 → 同日解決)

`runtime/precise_gc/gc_copy.c` の forward_edge 診断 (commit ae983279,
「途中経過 + 診断」) が `koruby_bootstrap_ctx` / `korb_vm` extern と
`cc->stack_base` / `cc->stack_end` (koruby 専用 CTX field) を直書きして
おり、 baruby_precise / ascheme_precise が HEAD でビルド不能だった。

**解決**: 診断ブロックは koruby の bump 移行デバッグ作業 (v2 再構築で放棄)
の持ち物なので、 weak hook 化ではなく**丸ごと撤去**。 診断専用だった
`g_scan_owner` / `g_in_root_scan` グローバルも削除。 forward_edge の
ロジック (idempotent skip + forward_payload) は不変。 baruby_precise /
ascheme_precise / koruby_precise ビルド + smoke (STRESS / STRESS+PURGE
含む) 確認済み。

## Phase 8 進捗 — RESULT 化 & per-CTX 機械化 (2026-05-29)

user 指摘: 「ctx->state 消せた？」「VALUE を返す関数を RESULT にする / 呼び側
も直す」「korb_vm はグローバル変数で残ってる」「一気にやりましょう」。

実施 (2026-05-29):
1. `CTX` に `struct korb_vm *mch` を追加 (per-CTX 機械参照、 abruby `c->abm`
   convention)。 commit a2a13522。 これは「korb_vm global を削除する道筋」。
2. `RESULT_FN __attribute__((warn_unused_result))` を導入し、 全 RESULT-returning
   関数 (korb_raise / korb_raise_X / korb_funcall_r / korb_yield_r) に適用。
   commit 33788e6a。 RESULT を捨ててる場所を全部 build 時に検出可能に。
3. `DROP_RESULT(call)` macro 追加 (RESULT を明示的に discharge する。
   `(void)` だけだと warn_unused_result が silence しない)。 commit 97a5c618。
4. mass migration (Python script): 323 ヶ所の bare `korb_raise(...);` を
   `DROP_RESULT(korb_raise(...));` で wrap。 commit e3ddb39b。
5. macro / inline header 内の 89 ヶ所も DROP_RESULT 化。 commit 7f90f3d2。
6. node.def の 57 ヶ所も DROP_RESULT 化 → node_eval.c 再生成。 commit 45232595。

結果:
- build warning 469 → 0 (全 warn_unused_result 警告消化)。
- build green、 spec pass count 不変 (263 PASS / 7 CRASH)。
- DROP_RESULT カウントが「残 migration 対象数」 の指標になる。

残作業 (Phase 8 続き):
- `-Werror=unused-result` を有効化 → regression 防止 hard error 化。 commit
  済 (Makefile)。
- DROP_RESULT を 1 ヶ所ずつ消す = function を VALUE → RESULT に migrate、
  caller も UNWRAP / return propagation に置換。
- 最終的に `CTX::state` / `state_value` を field から削除。
- `korb_vm->X` 参照を `c->mch->X` に順次置換、 最終的に global `korb_vm`
  を削除して「1 CTX = 1 interpreter」 を可能に。
- `struct korb_vm` 自体の type rename (→ `struct korb_machine`)。

### Phase 8b — builtins cfunc 完全 RESULT 化 (2026-05-29)

tools/migrate_cfunc.py で機械的に cfunc を新 ABI 化:
- file.c: 30 → 0 (commit 115aaff4)
- exception.c + binding.c 部分: 7 sites (commit 7682707b)
- range.c: 18 → 0 (commit ad5d3a4b)
- hash.c: 18 → 0 (commit e3e5ef24)
- string.c: 22 → 6 (commit d01ce7af)  ※残 6 は VALUE helper 内
- proc/comparable/module/object/float: 63 → 13 (commit d6ffe422)
- integer/kernel/array: 134 → 8 (commit 424d94ae)

prologue_cfunc_r_inl に c->state lift safety net 追加: cfunc_r 内から
legacy VALUE-returning helper (korb_funcall, korb_yield 等) を呼んだとき
state が失われるのを防ぐ。

CHECK_FROZEN_R(c, self) macro 追加: RESULT 版 CHECK_FROZEN_RET。

合計: 469 → ~125 DROP_RESULT (object.c 29 + node.def 57 + builtins/* 残)。
残りはほぼ AST 内部 / legacy helper 内で、cfunc 層の migration は完了。

regression: test_string/hash/range/exception/array/class/block/integer/
control 全 PASS。

memory note: feedback_result_and_vm_priorities (user 要望保存)。

### Phase 8c — korb_vm global → KORB_VM(c) macro 移行 (2026-05-29)

global `korb_vm` を直接参照する 891 ヶ所中、 CTX *c がスコープにあるものを
全て `KORB_VM(c)->X` (= `(c)->mch->X`) に変換 (commit e554de81):
- context.h に KORB_VM(c) macro 定義 (transition 用)。
- builtins.c: 732 → 0、 builtins/*.c: 159 → 0、 object.c: 148 → 28。
- builtins/hash.c::hash_apply_self_class helper に CTX *c 追加。
- 残 28 ヶ所 (object.c) と main.c / exe_main.c / koruby_runtime.c は CTX を
  取らない low-level helper (korb_class_add_method_*, korb_check_basic_op_redef,
  korb_method_cache_fill, koruby_visit_roots 等)。 これらは将来:
  (a) CTX を引数追加、 (b) per-machine instance method 化、
  (c) thread-local 化 のいずれかで対応。

multi-interpreter 化への第一歩。 引き続き c->mch を経由した参照に統一して
いけば global 撤廃が現実的に。

### STRESS+PURGE crash fix — ary_mul (2026-05-29)

`[1, 2] * ","` が STRESS+PURGE で SEGV する pre-existing bug を修正 (commit
3697798d)。 ary_mul の join path で `arg = argv[0]` を ARO_ROOT_SCOPE 前で
C-local capture し、 scope 内で `rs[0] = korb_str_new()` の alloc 後に
`rs[1] = arg` していたため、 arg の指す string heap obj が GC 移動された
後の stale pointer が rs[1] に格納されていた。 PURGE で次 iter 時に SEGV。

修正: `rs[1] = arg` を `rs[0] = korb_str_new()` の前に。 同パターンを 12 builtins
+ node.def + object.c で grep → 該当なし。 ary_mul 固有。

成果: 全 10 test suite (string/hash/range/exception/array/class/block/
block_arg/integer/control) が default / STRESS / STRESS+PURGE 全 mode で PASS。
これで koruby_precise の test suite は GC モード全制覇。

### AST dispatcher の argv-clobber 修正 + Array.new subclass initialize (2026-05-29)

User 指摘: 「sp として staging + 1 を渡してるんだから、別に c->sp を設定し
ないでいいんじゃないの？ alloc 前に設定するのは callee の仕事」

問題:
1. korb_dispatch_to_method の AST 経路は new_fp = c->sp + 1 から zero-fill
   するが、 caller が argv を sp 上に staging したケースで argv が clobber
   される。
2. node_super_forward 系 (3 site) が `sm->u.cfunc.func` を直接呼び、 cfunc_r
   登録 (ary_initialize 等) では func==NULL で SEGV。
3. `Array.new(...)` が subclass の initialize を call せず、 size/default
   short-cut で結果を返していた。 spec: array/to_a_spec subclass テスト fail。

修正 (commit 832011d6, dcf28a8c):
- AST 経路で argv を VLA に snapshot してから zero-fill (callee 責任の API
  に統一)。
- cfunc_r 経路にも同じ snapshot を入れて argv-overlap を防御。
- node_super_forward / node_super (3 site) で func_r 経路を追加。
- ary_class_new: allocate-then-dispatch-initialize に refactor。 ary_initialize
  registration を _r 化 (Phase 8b で body は _r 化済だったが register が
  legacy のまま残っていた未解決 integrity 不整合)。

成果: broad rubyspec sweep (150 spec) PASS=1383 → 1615 (+232)、 CRASH=0
維持、 全 test suite regression なし、 STRESS+PURGE も完走。

### 追加 fix (2026-05-29 第 5 セッション)

- Hash#slice / #except: compare_by_identity flag を保持 (CRuby 仕様)
- Integer#gcd / #lcm / #gcdlcm: 非 Integer 引数で TypeError 投出
- Integer#~ for Bignum: 真の two's-complement で実装 (mpz_com 使用)。
  allbits?/anybits?/nobits?/complement_spec 等を ~bignum_value で
  通すように。
- ary_sort_compare: <=> が nil で ArgumentError 投出 (CRuby 仕様)
- Range#begin / Range#end: rng_first / rng_last の alias から分離して
  nil-safe (beginless/endless で raise しない)
- Range.new: #<=> が raise したら propagate (rescue しない)
- Integer#pow(neg, mod): RangeError 投出 (bootstrap.rb)
- Array.new / Hash.new / String.new: subclass の initialize を dispatch
  (allocate-then-call initialize pattern)。 大量の subclass test を
  fix。 c->sp bump + sp[0] re-read で AST dispatcher の zero-fill
  clobber 回避。
- {ary,hash,str}_class_new で self を alloc 前 capture せず、 alloc 後に
  sp[-argc-1] から re-read。 T_CLASS は arena allocated なので
  STRESS+PURGE で移動して stale 化する。
- T_STRING / T_ARRAY / T_HASH / T_RANGE / ... での @ivar 対応:
  korb_vm->generic_ivars という side table (Hash of obj-ptr → ivar Hash) を
  追加。 visit_roots で tracking。
- AST dispatcher で argv snapshot: caller が sp 上に staging した argv が
  zero-fill で clobber される問題を VLA snapshot で防ぐ。 cfunc_r 経路
  にも同じ snapshot 適用。
- super (3 site in node.def): cfunc_r 経路に対応 (ary_initialize 等が
  cfunc_r 登録のため legacy 経路で func==NULL → SEGV していた)。
- prologue_proc_method / super で proc_call の ABI 不整合修正
  (Phase 8b で proc_call は RESULT 化済だが caller がそのまま legacy
  呼び出ししていた)。 __method__ spec の crash も同時 fix。
- kernel_method_name: cfunc_r prologue が frame を push するため
  current_frame->method は __method__ 自体になる → KORB_METHOD_CFUNC
  frame を skip して enclosing を返す。

成果: broad rubyspec sweep PASS=1615 → 1634 (+19)、 CRASH=0 維持、 全
10 test suite が default / STRESS / STRESS+PURGE 全 mode で PASS。

### Curry / nested closure bug (2026-05-29 第 6 セッション)

`Proc#curry` 含む recursive Proc dispatch で nested lambda の captured
outer-var が壊れる pre-existing bug を発見 ([[project_koruby_precise_curry_bug]])。

簡易 repro: `lambda { |a,b,c| a+b+c }.curry[1][2][3]` → SEGV。

仮説: `korb_snapshot_one_proc_` が new_inner_lambda の env を snapshot する
際、 fresh_env_B (rec の proc_call clone) は accum_env_size 個 (= 親 lambda
の env size) しか valid 領域を持たないが、 new_inner->env_size の方が大きい
場合、 snap_B の slot K > accum_env_size は fresh_env_B の SLACK 領域 or
他データを copy する。 親が contained block の env_size を伝播せず、 child
が parent の env_size を超えると corrupted slot にアクセスする。

対応案: parser-side で `korb_proc_new` の env_size 算定時に contained block
の env_size を親側に伝播して max を取る。 または snapshot 経路で child の
env_size まで cover するように fp_hi を拡張する。

未解決。 rubyspec の curry_spec.rb が crash する。 他は不影響 (test_block
他は PASS)。

### Bug fixes (2026-05-29 第 6 セッション、 後半)

- Array#zip: #to_ary / #to_a 失敗時の #each fallback 実装。
  Array::__zip_each__(obj, n) ヘルパで each → break at limit パターン。
  zip_spec: 10 → 12 PASS。
- proc_call clone に FRESH_ENV_SLACK = 512 (defensive、 korb_yield に
  mirror)。

このセッション末: rubyspec sweep PASS=1634 維持、 全 10 test suite が
default / STRESS / STRESS+PURGE 全 mode で PASS、 27 benchmark が
STRESS+PURGE で 0 crash。

### Enumerable 系の改善 (2026-05-29 第 6 セッション、 終盤)

- Enumerable#sort: 1 個目の `def sort` が block を受け取らず無視していた
  bug を fix。 sort_spec: 5 → 9 PASS。
- Enumerable#count(arg) / #count { ... } をサポート。 count_spec: 3 → 8 PASS。
- Enumerable#min(n) / #max(n) / #min_by(n) / #max_by(n) (arity n) を
  サポート。 min/max_by/min_by: +21 PASS。
- block 引数の受け取り方を CRuby 仕様 (block(probe, running) で sign 確認)
  に統一。

Total +33 PASS from Enumerable fixes。 broad sweep 上では fluctuation あり
だが 150-spec sample で stable 1634。 specific enumerable spec で confirm 済。

### 第 8 セッション: yield-args bug root cause fix + Phase 8d 大規模 RESULT 化 (2026-05-29)

**Bug fix 1**: yield-args 破壊 (commit ed8dc4db)

`def my_fi(&blk); obj.each { blk.call }; end` パターンで yielding method
(each) が yielder の caller (my_fi) より深い frame で動くと、 share-env
path で yield する時 block body の `c->sp = sp + N` が active method
frame slots を上書きしていた。

Fix: `korb_yield_slow` で `prev_fp > blk->env` (method overlaps env) を
検出したら `fresh_env_path` を強制 ON。 `korb_yield` fast path も同条件で
slow path に fall。 [[project_koruby_precise_yield_args_corruption]] を
"FIXED" 化。 効果: 副次的に `Proc#curry[1][2][3] = 6` の基本ケースが動く
ようになった (curry_spec 0 CRASH → 34 PASS / 14 FAIL/ERR)。

**Bug fix 2**: Enumerable predicates が `blk.call(*xs)` に戻せた

bug fix の結果 yield-args corruption が消えたため、 `any?/all?/none?/
one?/find_index` を `blk.call(*xs)` に書き換え (multi-yield の正しい
gather/dispatch 復活)。 any_spec.rb 65 → 70、 all_spec.rb 56 → 61 PASS。

**Enumerable 改善**:
- Hash#any?, Hash#all? の override 削除 (Enumerable へ委譲)。 pattern
  arg / gather-as-pair 対応。
- Array#any?/all?/none?/one? cfunc に argc > 1 の ArgumentError check 追加。
- Array#intersect? を実装。 intersect_spec.rb 0 → 10 PASS。

**Phase 8d 大規模 RESULT 化** (user 強い要望: 「c->state 消してってば 全部 RESULT」)

- `koruby_gen.rb`: `Node.result_type = "RESULT"` を override。 全 dispatcher
  / EVAL が RESULT 返り値で gen される。
- `node.def`: 全 120 NODE_DEF を mechanical rewrite (tools/migrate_nodes_to_result.py)。
  - `return X` → `return RESULT_OK(X)`
  - `DROP_RESULT(korb_raise(...)); return Qnil` → `return korb_raise(...)`
  - `EA(c, n)` → `EVAL_ARG_UNWRAP(c, n)` (user 指摘で改名、 UNWRAP(EVAL_ARG(c, n)) 展開)
  - while/until/rescue/rescue_else/ensure を RESULT-native loop に
  - break/next/retry/redo/raise/return 終端 node は `(RESULT){v, state}` を返す
- `node.h`: `EVAL` が RESULT 返り値の canonical (旧 EVAL は `EVAL_LIFT` に rename、
  legacy c->state bridge 用)。 user 指摘で `EVAL_R` を最終 `EVAL` 名に。
- `prologues.h` / `object.c`: `mc->dispatcher(c, mc->body, sp)` 直叩きを
  `EVAL(c, mc->body, sp)` に (user 指摘「dispatcher 直接呼出しはやめてね」)。
- `context.h`: `LIFT_C_STATE` + `LIFT_C_STATE_OR_OK` macro 追加 (legacy
  helper を呼ぶ場所での c->state → RESULT bridge)。 全 helper を _r 化
  するまでの過渡期 macro。
- 全 10 test suite が default / STRESS / STRESS+PURGE で PASS 維持。

残り Phase 8d 作業:
- ~~`EVAL_LIFT` 撤去~~ **完了**。 全 caller (koruby_run_ast / proc_call /
  korb_yield_slow / Fiber start / Kernel#eval / Module#class_eval /
  Binding#eval / korb_dispatch_to_method) を RESULT-native 化。
- `korb_dispatch_call` / `korb_funcall` / `korb_yield` / 各種 `korb_node_X_slow`
  helper を RESULT 化 (今は LIFT_C_STATE 経由で c->state を read)。
- `builtins/` 全 cfunc を cfunc_r ABI に統一 (現在 ~100 ヶ所未移行)。

### 第 7 セッション: Enumerable 追加 + pre-existing yield bug 発見

- Enumerable#take(n) / #drop(n) を実装 (Integer 強制 + to_int coerce)。
  drop_spec: 0 → 9 PASS。
- Enumerable#chunk_while / #slice_when の block 必須化、 slice_when
  実装。 chunk_while: 3 → 4、 slice_when: 0 → 6 PASS。
- Enumerable methods で each yield-multi 値の gather pattern:
  each_with_object 8→17 (+9)、 group_by 5→7 (+2)、 partition 1→3 (+2)。
- Enumerable#any? / #all? / #none? / #one? に pattern arg + multi-arg
  raise + multi-yield 対応。 any 49→63、 all 40→54、 none ~25→31、
  one ~30→39。

**pre-existing bug 発見** ([[project_koruby_precise_yield_args_corruption]]):
`def each(arg, *args); yield arg; end` を block-from-outer-method 経由で
呼ぶと args が逆流書き込みされる。 predicates の blk.call(*xs) で
surface した。 curry bug ([[project_koruby_precise_curry_bug]]) と同根
の env-slot 算定問題と推定。 blk.call(x) (gather as array) で回避。

このセッション末: rubyspec sweep PASS=1634 stable、 全 10 test suite が
default / STRESS / STRESS+PURGE 全 mode で PASS。 Enumerable 全体で
個別 spec の PASS counts が大幅改善 (合計 ~+50 PASS、 sweep sample に
は含まれない specs での gain)。

## rubyspec 取れ高改善 (2026-05-28 後半)

baseline (commit 66ab6dda 時): broad sweep `PASS=238 / FAIL=178 / CRASH=11`。
直近 sweep: `PASS=263 / FAIL=148 / CRASH=7`。 第 3 セッション分の追加:

- Integer#<=> に coerce protocol + Bignum-vs-Float 精度保存 + NaN/Inf 対応。
  33 → 42 pass。
- Array#initialize を実装 (旧 ary_class_new とは別 entry)。
  array/initialize_spec 6 → 30 pass。
- Range#first / #last に to_int coerce, Float truncate, beginless/endless
  例外。 first 6 → 11、 last 7 → 12 pass。
- Range#each に beginless TypeError / endless / non-succ 対応。 8 → 15。
- Range#min / #max を専用実装 (rng_first/last からの誤エイリアス解消)。
  min 8 → 9、 max 9 → 10 pass。
- Range#size で endless / beginless / non-Numeric の挙動を CRuby に合わせ。
  4 → 8 pass。
- Array#sum に Kahan compensated summation。 25 → 26 pass。
- Integer#round に half: kwarg (:up / :down / :even) と ArgumentError。
  8 → 21 pass。
- Integer#truncate を専用実装。 3 → 6 pass。
- Array#cycle で count の Float / to_int / TypeError 対応。 11 → 18 pass。
- mspec_shim に `it_should_behave_like` 別名追加。
- Numeric#coerce のデフォルト実装。 0 → 10 pass。
- Integer#coerce で String / #to_f respondable も Float pair 化。 17 → 24。

## rubyspec 取れ高改善 (2026-05-28 後半 + 第 2 セッション分)

baseline (commit 66ab6dda 時): broad sweep `PASS=238 / FAIL=178 / CRASH=11`。
第 2 セッション末で `PASS=258 / FAIL=153 / CRASH=7`。 第 2 セッション分:

- Array#slice / #slice! を全面書き直し (Range / (idx, len) / Range の to_int
  coerce / element-wise return)。 27 → 95 pass。
- Array#bsearch に find-any (Numeric ブロック) mode と Enumerator 経路。
  10 → 20 pass。
- Array#equal_value (==) に identity check + element-wise user dispatch
  + non-Array rhs の to_ary 経由 == 委譲。 5 → 8 pass。
- mspec_shim MSpecMock#respond_to? を should_receive(:respond_to?) と
  singleton_methods に追従。
- Array#[]= の (start, len) self-alias 安全化 + 不正引数 + to_ary coerce
  + Bignum length → RangeError。 element_set 54 → 320 pass。
- Array#* で to_str → to_int の coerce 順序 / nil → TypeError / 多引数で
  ArgumentError。 multiply 18 → 24 pass。
- String#[] (slice) を UTF-8 codepoint index で書き直し。 chr 9 → 11、
  element_reference 10/10 pass。
- String#delete_prefix / #delete_suffix を codepoint 境界尊重。 prefix
  21 → 22、 suffix 21 → 22 pass。
- String#[]= で UTF-8 codepoint index と to_str coerce、 raise TypeError。
- Array#fill に Range form + grow + Bignum RangeError + argc 範囲 check。
  62 → 130 pass。
- Array#to_h に argc check + to_ary coerce + sized error。 7 → 14 pass。
- Array#values_at の Range で nil-fill + RangeError。 11 → 14 pass。
- Array#sort! / Array#sort_by! の frozen check & break-aware。
- Array#zip に to_ary/to_a coerce + block 経路。 7 → 10 pass。
- Array#each / Array#rindex で iteration 中の size 変化を再評価。
- Hash#to_h で subclass → fresh Hash + default/default_proc/CBI 引継ぎ。
  17 → 20 pass。
- Hash#[] で subclass の #default override を honor (canonical 以外で
  funcall 経由)。 element_reference 28 → 29 pass。
- Hash.[] (class method) で to_hash/to_ary coerce + argc 検証 + 要素検証。
  20 → 30 pass。
- Hash#transform_keys! の break で「処理済 transformed + 未処理 untouched」 
  + Hash#transform_values で CBI 引継ぎ。
- Symbol#casecmp / #casecmp? を追加 (String 経由)。 0 → 41 pass。
- Integer#to_s で全 base 2..36 + Bignum 経路 (mpz_get_str)。 12 → 25。
- Integer#div / #divmod で Bignum / Float / coerce protocol 対応。
  div 13 → 60、 divmod 6 → 29 pass。
- Float#round で negative-precision Integer 化、 NaN/Inf 例外、 kwarg drop。
  62 → 83 pass。
- Float#divmod に NaN / Infinity / ZeroDivisionError 検査。 9 → 14 pass。
- Float#<=> に coerce protocol + Infinity vs finite Integer + obj.infinite?
  経路。 26 → 39 pass。



主な修正 (順):
- `String#lines` に sep / `chomp:` 引数 (bootstrap.rb の override 削除 +
  C str_lines を引数 parse 込みで再実装) — lines_spec 0 → 3 pass
- `Integer#%` で Float RHS の SEGV — int_mod に Float fast path 追加。
  これで integer/{pow,gcd,gcdlcm,lcm}_spec が crash 脱出 (合計 +75 pass)。
- `Integer#pow` (bootstrap.rb) で exp/mod の type 検証 + ZeroDivision。
- `String#ljust/rjust/center` の pad cycle が CRuby と非互換 (`pad * (n-size)`
  だと長すぎる) → `__pad_to(pad, n)` helper を導入。 ljust/rjust 14 → 33、
  center 16 → 49 pass。
- `String#squeeze` を charset (range, negate, multi-set 交差) 対応に再実装、
  squeeze! に frozen 判定追加 — squeeze_spec 6 → 44 pass。
- `String#reverse/#chop/#insert` を UTF-8 codepoint 境界対応。
- `String#chomp` no-arg で `$/` 参照を追加。
- `Array#join` で sep の `to_str` coerce + empty array short-circuit。
- `Array#flatten` に depth の `to_int` coerce + 要素 `to_ary` 連携、
  `Array#flatten!` を専用 entry として実装。 flatten_spec 27 → 50 pass。
- `Array#each` でループ毎に array len を再評価 (block 内の push/shift 反映)、
  no-block で Enumerator を返す。 each_spec 4 → 10 pass。
- `Hash#to_h` で subclass からの fresh Hash 生成 + default/default_proc/
  compare_by_identity 引継ぎ。 to_h_spec 17 → 20 pass。
- `Array#sort_by!` を no-block enumerator / break-aware に。 sort_by 10 → 13。
- `Array#reject!` を delete_if と分離 (no-change → nil)、 両者に no-block
  enumerator 経路を追加。 reject_spec 25 → 38、 delete_if 6 → 8 pass。
- `Hash#transform_keys!` の break 動作 (処理済 transformed + 未処理 untouched)、
  `Hash#transform_values` の compare_by_identity 引継ぎ。
- `Array#min/#max` のブロック引数順を (probe, running) に修正 (sort と逆)、
  min 28 → 38、 max 33 → 35 pass。

残 7 CRASH の sweep:
  array/{combination,comparison,permutation,repeated_combination,
        repeated_permutation,uniq}_spec、 string/each_byte_spec。
- comparison は recursive_array 比較で stack overflow。
- combination 系は深い再帰 + 要素数で OOM。
- uniq は要素数で OOM。
- each_byte は Enumerator + Fiber 経路で SEGV。



## 残 global / korb_vm->current_ctx fallback 撤去 — **完了 (2026-05-28 thirteenth pass)**

「グローバルつくらんといて」 規約 (memory: feedback_no_globals_strict) に
従い、 段階的に CTX を関数引数経由で渡す方向に移行 → **完了**:

- `current_block` / `running_block` global → CTX field 化 (commit 8e388503)
- `korb_str_new` / `korb_str_new_cstr` に (CTX, sp) (commit 0010d621)
- `korb_float_new` / `korb_float_new_heap` に (CTX, sp) (commit 03435e7a)
- `korb_ary_new` / `_capa` / `_from_values` / `korb_hash_new` に (CTX, sp) (commit f2297121)
- `korb_str_dup` / `korb_str_concat` / `korb_object_new` に (CTX, sp) (commit 9e8905b4)
- `korb_class_new` / `korb_module_new` に (CTX, sp) (commit 68994185)
- `korb_to_s` / `korb_inspect` に (CTX, sp) (commit 007fe5af)
- `korb_cvar_names` / `method_params_for_method` / `methods_with_visibility` /
  `korb_select_collect_ready` / `str_appendf` / `korb_inspect_inner` / `korb_p` /
  `korb_runtime_init` / `korb_init_builtins` に CTX 引数化 (commit 3c00037f)
- `korb_singleton_class_of` / `korb_singleton_class_of_value` に CTX 引数化 (commit 53a99fe4)
- `korb_hash_value` / `korb_eq` / `korb_eql` / `korb_hash_aref*` / `korb_hash_aset`
  に CTX 引数化 (commit 10abd165)
- `korb_raise_frozen_modification` / `korb_ivar_set_ic` / `korb_class_add_method_ast*` /
  `korb_exc_new` / `binding_arg_to_id` / 残 object.c の `c2/c3` 内部 fallback
  に CTX 引数化 (commit 1dc64f59)

残 1 件: `koruby_setup_ctx` (= bootstrap_ctx を返す boundary entry point) のみ。
これは 「初期化フロー」 のため許容。

## c->state 撤去 (Phase 8) — 段階移行 plan

「c->state 消してね」 規約。 試行で result_type を一括 RESULT 化すると
node.def + node.h の `EVAL_ARG` / `dispatcher_t` / prologues.h / 多数の
`VALUE x = EVAL(...)` callers の 15 系統の error が連鎖し、 1 commit で
build green に到達できないことが判明。

代わりに 「段階移行」 方式を採る:

1. **dispatcher_t / EVAL を RESULT 化 + EVAL_LEGACY 提供**: 既存 callers
   は EVAL_LEGACY (pump RESULT → c->state) を使う thin wrapper を用意。
   - dispatcher 直叩き callers (prologues.h, object.c の builder, builtins
     経由の korb_yield 内部 inline) も同様に pump 経由に。
2. **node 単位で result_type=RESULT に opt-in**: NODE_DEF の per-node flag
   で result type を選べるよう koruby_gen.rb を拡張。 1 node ずつ移行し
   ながら build green を維持。 EVAL_ARG 結果も per-node RESULT or VALUE。
3. **node.def 内の `return X;` を機械変換**: Python script で
   `return RESULT_OK(X);` に置換。 `if (cond) return Y;` も対応。
4. **korb_raise / korb_funcall / korb_yield も RESULT 化**: 既に
   `korb_funcall_r` / `korb_yield_r` ブリッジは追加済 (commit fa03bd2a)。
5. **c->state を読む箇所を順次撤去**: legacy cfunc 群を Phase 4 sweep で
   新-ABI 化していくときに同時に置換。
6. **最終的に CTX から `state` / `state_value` field を削除**。

途中の試行で以下まで進めた (revert 済):
- koruby_gen.rb override で全 node の dispatcher signature を RESULT に
- node.def の `return X;` (270 箇所) を `return RESULT_OK(X);` に
- EA / CHECK_STATE マクロを UNWRAP ベースに書き換え
- 残 ~10 site (prologues.h dispatcher 直叩き、 node_eval.c の三項 EVAL_ARG、
  object.c / builtins の `VALUE x = EVAL(...)` callers) で build error

これらは next session に持ち越し。

## 現状 (2026-05-28, twelfth pass)

直近の発見: 自前 test/ の "OK Xxxx (0)" 24/24 表示は **TESTS.each
{|t| run_test(t) } という最頻出パターンが top-level の block 経由で
動かず**、 各 test メソッドが 1 回も実行されないまま `$fail == 0` で
"OK ..." を出していた phantom pass だった。

修正項目:

1. **block / proc / fiber body の sp = fp + env_size 規約整備**
   (object.h / object.c / builtins/proc.c):
   bake walker は block 内 lvar を envsize 相対の sp_offset に baking
   する一方、 korb_yield fast/slow path・proc_call・fiber entry が
   sp=fp を渡していたため、 `puts "hi"` のような最も単純な literal
   までもが nil に化けて出力が空行になっていた。 method body と同じく
   `sp = fp + env_size` を渡すよう統一。

2. **node_plus の synthetic frame で self を hijack していた**
   (node.def): GC 越しに `l` を生存させる目的で `fr.self = l` を設定
   していたが、 これが rhs 評価中の ivar 参照 (例 `"X" + " (#{@y})"`)
   を l に向けて @y を常に nil 化していた。 `fr.self = caller's self`
   に戻し、 l は `fr.last_line` に park する形へ。

3. **block_given? が cfunc frame の block を見ていた**
   (builtins/kernel.c): prologue_cfunc_inl は frame push するように
   変わっていたが kernel_block_given は古い前提 ("cfunc は push しない")
   のままで block_given? が常に false。 frame chain を walk して直近
   の AST method frame を見るよう修正。

4. **Fiber 二回目以降の resume で stale な cfunc frame chain**
   (object.c): korb_fiber_entry が frame push せず、 inner cfunc frame
   が resumer の cfunc frame (= 一度 return すると stale) を .prev anchor
   としていたため 2 回目以降の resume で SEGV。 fiber 自身の C stack に
   stable な fiber_root frame を push して inner はそこに chain する形に
   改修。 prev_top の巻き戻しも撤去 (= resume の POST-SWAP が
   resumer_current_frame で c->current_frame を上書きするため不要)。

5. **current_block / running_block が visit_roots に欠落**
   (koruby_runtime.c): ary_each などの builtin iterator が
   `korb_yield(c, 1, &v) → current_block` 経路で proc を deref するが、
   current_block は file-scope global で GC が edge を update して
   いなかった。 STRESS の下で proc が forward された途端、 次の yield
   で blk->self / blk->env が unmapped 領域を deref → SEGV。
   visit_roots の (d') section に追加。

### NORMAL mode 結果 (本セッション)

| test                       | pass |
|----------------------------|------|
| test_alias                 | 9    |
| test_alias_redef           | 3    |
| test_array                 | 72   |
| test_basic_op_redef        | 4    |
| test_block                 | 33   |
| test_block_arg             | 8    |
| test_class                 | 18   |
| test_comparable            | 21   |
| test_control               | 34   |
| test_cpu_corner            | 29   |
| test_eq                    | 58   |
| test_eq_redef              | 7    |
| test_exception             | 10   |
| test_fiber                 | 26   |
| test_float_round           | 27   |
| test_flonum                | 80   |
| test_hash                  | 52   |
| test_integer               | 105  |
| test_misc                  | 26   |
| test_object_alloc          | 19   |
| test_range                 | 27   |
| test_string                | 49   |
| test_to_s_dispatch         | 5    |
| test_yield                 | 15   |

**24/24 ファイル 全 OK、 計 ~735 assertion 全 pass**。

### STRESS mode 結果

NORMAL の後で BARUBY_GC_STRESS=1 を入れて自前 test/ を全 24 件走らせた
結果。 段階的に修正を入れて **24/24 file 完全 pass** (STRESS / STRESS+PURGE
両方とも全 OK)。 初期 14 から +10。 mode 間で異なる stale ref 経路が
表面化する pattern (test_block は STRESS のみで SEGV、 逆に test_exception
は STRESS で OK だが PURGE で SEGV) を最後の 2 つの修正で潰した:

- `koruby_visit_roots` に **gvars table を追加** (= `$!` を auto-forward
  しないと raise inside rescue で cause-link walk が SEGV; commit
  e3dfea32) — test_exception STRESS+PURGE。
- **korb_yield の prev_self を GC root stack で pin** (commit 76aa7419)
  — block body の EVAL を straddle する prev_self C-local が STRESS GC
  越しに stale 化し、 block exit の frame.self 復元で死アドレスを
  書き込む問題。 test_block STRESS。

| test                      | STRESS  | STRESS+PURGE |
|---------------------------|---------|--------------|
| test_alias                | 9       | 9            |
| test_alias_redef          | 3       | 3            |
| test_array                | 72      | 72           |
| test_basic_op_redef       | 4       | 4            |
| test_block                | 33      | 33           |
| test_block_arg            | 8       | 8            |
| test_class                | 18      | 18           |
| test_comparable           | 21      | 21           |
| test_control              | 34      | 34           |
| test_cpu_corner           | 29      | 29           |
| test_eq                   | 58      | 58           |
| test_eq_redef             | 7       | 7            |
| test_exception            | 10      | 10           |
| test_fiber                | 26      | 26           |
| test_float_round          | 27      | 27           |
| test_flonum               | 80      | 80           |
| test_hash                 | 52      | 52           |
| test_integer              | 105     | 105          |
| test_misc                 | 26      | 26           |
| test_object_alloc         | 19      | 19           |
| test_range                | 27      | 27           |
| test_string               | 49      | 49           |
| test_to_s_dispatch        | 5       | 5            |
| test_yield                | 15      | 15           |

行った修正:
- **libc-allocated proc の self / enclosing_block を visit_roots に
  追加** (koruby_runtime.c): scan_edges は arena obj のみ呼ばれる
  ため libc proc の field は forward されない。 running_block /
  current_block / 全 frame.block を walk して内部を再帰 visit。
- **korb_class_of_class を type-based redirect に**
  (object.h): T_ARRAY / T_STRING / T_HASH / T_RANGE / T_PROC /
  T_FLOAT / T_BIGNUM の basic.klass は GC 越しに stale 化するため、
  type を見て korb_vm->X_class (= visit_roots で auto-track) を
  直接返す。
- **korb_inspect_inner T_ARRAY / T_HASH / T_STRING を ARO_ROOT_SCOPE
  化** (object.c): result 文字列 / element 文字列が korb_str_concat
  越しに stale 化していた (= `[20, 30]` が `]]` に、 `"hello".inspect`
  が `""` になる現象)。
- **libc obj registry (koruby_register_libc_obj)** (koruby_runtime.c
  + object.c + builtins/{object,binding,symbol}.c): 全 libc 容器
  (korb_array / korb_hash / korb_range / korb_float / korb_bignum /
  korb_proc) + T_DATA (Method / Binding / Fiber / symbol-proc /
  method-proc) の constructor で koruby_register_libc_obj を呼んで
  singly-linked list に登録。 visit_roots 末尾の (f) phase で list
  を walk し、 各 obj の内部 heap-pointer fields (basic.klass,
  array elements, hash entries, proc env / self / cref) を
  visit_value_slot / visit_ptr_slot で forward。 これにより hash
  key/value, array elements, proc env 等に格納された arena ref が
  GC 越しに正しく追跡されるようになる。
- **node_lshift で l の synthetic-frame pin** (node.def): node_plus
  と同じ pattern で fr.last_line に l を park し、 fr.self は
  caller's self 継承。 `s << "y"` の self が GC 越しに stale 化
  していた問題を解消。
- **builtin pinning の系統的適用**: ary_min, ary_max, ary_mul
  (string-join path), str_lshift / str_concat_one, str_split,
  str_chars, str_gsub, str_sub, kernel_format (sprintf) の各 cfunc
  iterator で C-local heap pointer を ARO_ROOT_SCOPE で pin。

新規 full pass: test_cpu_corner / test_hash / test_to_s_dispatch /
test_misc / test_fiber / test_array / test_string。

残課題 (2026-05-28 完了):
- **test_exception PURGE SEGV**: gvars.vals が visit_roots に欠落
  していたため $! (= 直前 raise した exc) が stale 化、 nested raise の
  cause-link walk で SEGV。 koruby_runtime.c phase (d'') で gvars を
  walk するよう追加 (commit e3dfea32)。
- **test_block STRESS SEGV**: korb_yield の prev_self C-local が
  block body の EVAL を straddle して stale 化、 block exit 時の
  frame.self 復元で死アドレスを書き込んでしまう。 prev_self を
  AROH_ROOT_STACK_TOP / SET_TOP で root stack に park (commit 76aa7419)。

### rubyspec STRESS+PURGE 改善 (2026-05-29)

rubyspec の language sweep を STRESS+PURGE で計測した結果:

- 初期 6 PASS / 61 SEGV (前 session 終了時)
- 14 PASS / 50 SEGV (current)

追加修正 (commit 順):

- `proc_call` の prev_self を AROH_ROOT_STACK で pin (commit 5cb2938c)
- `korb_const_lookup` で cref->klass=NULL を defensive skip
  (commit 85dfcd0c) — GC forward_payload の stale-to-space 分岐で
  cref->klass が NULL 化された場合の保険。
- libc registry walker で T_PROC の `env` を walk (escape proc 用),
  proc_call / prologue_ast_general で raise/throw 時の r snapshot を skip
  (commit 827e0151)。
- `node_apply_call` の recv / args / block を 3-slot pin (commit cd60b4b0)。
- `node_method_call_block` の r / block を 2-slot pin (commit c516da2c)。
- `node_aref` / `node_aset` / `node_obj_singleton_def` を pin (commit 323b1f85)。
- `korb_singleton_class_of_value` を ARO_ROOT_SCOPE 化、 o 再 load
  (commit 323b1f85)。

### BARUBY_GC_STRESS=N 間隔指定 (2026-05-29)

`BARUBY_GC_STRESS=100 BARUBY_GC_PURGE=1` のように N alloc 毎の GC を
指定可能に (commit 763880ed)。 default の N=1 (= 全 alloc) では
重い workload (rubyspec / benchmark) が timeout するため、 100 や
1000 などで間隔を緩和して使う。 BARUBY_GC_STRESS=100 + PURGE で
benchmark 26/27 完走 (従来 timeout で 24/27)。

### node.def の sp slot pattern 統一 (2026-05-29)

ARO_ROOT_SCOPE_START を node.def 内で多用していたが、 baruby_precise
の @child / DISPATCH 自動 spill モデルに合わせて、 scope-entry での
1 回の sp shift + sp[N] 参照 + scope-exit restore パターンに refactor
(commit 1a66508c)。

```c
sp[0..N-1] = 0;     /* zero-fill */
c->sp = sp + N;     /* extend visit range — 1 回だけ */
sp[0] = EA(c, ...); /* GC 中 sp[0] auto-forward */
...
c->sp = sp;         /* restore */
```

object.c / builtins/ の C-level pinning (sp arg を持たない関数) では
ARO_ROOT_SCOPE_START を引き続き使用。

### 残課題 (要 framework 改造)

- **koruby_gen.rb に `@child` decorator 対応**: astrogen framework は
  既に `@child` を Operand level で実装済 (Node 側で build_specializer
  が auto-generate)。 koruby_gen.rb は PG_CALL_NODES 用 custom
  specializer があり、 @child 互換にするには両方の specializer に
  child spill コードを emit する必要。 baruby_precise のように
  framework に任せると node.def 側が `VALUE recv@child` と書くだけで
  `sp += N; sp[-N] = dispatch(child, sp); ...` が auto-generate される。

- **proc.self / frame.self が STRESS+PURGE で stale 化する根本原因**:
  libc registry walker は `visit_value_slot(ctx, fn, &p->self)` を毎
  cycle 呼んでいるはずだが、 magic_comment_spec.rb / array_spec.rb 等で
  proc.self が retired plane の addr (PROT_NONE 領域) を保持し続けて
  SEGV。 forward_payload が NULL を返すはずの分岐 (gc_copy.c L417) に
  到達していないか、 そもそも visit が走っていない可能性。 詳細 trace
  要。

### 2026-05-29 追加: sp-based / RESULT 返り値 ABI への大規模 refactor 着手

C-local stale 問題と c->state 側流の散漫な伝播を根本解決するため、
Lua 風の stack-based ABI に全面移行する設計を decision。 baruby_precise
/ castro / abruby と同じ規約。

#### 新 ABI 規約

| function 種類 | signature | sp の意味 |
|---|---|---|
| cfunc | `RESULT cf(CTX *c, int argc, VALUE *sp)` | sp[-argc-1]=self, sp[-argc..-1]=args, sp[0..]=scratch |
| EVAL_node body | `RESULT EVAL_node_X(CTX *c, NODE *n, VALUE *sp, ...)` | parent の staging top |
| C API helper | `RESULT h(CTX *c, VALUE *sp)` (fixed arity) / `RESULT h(CTX *c, int argc, VALUE *sp)` (varargs) | sp[-N..-1]=args |
| 即値 helper | 値で受けてよし (alloc しないもの) | -- |

「sp = この scope の staging top。 過去の値は sp[-N]、 自分の scratch
は sp[0..]」 で全関数統一。 caller は `c->sp = sp + N` で alloc 直前
sync。 例外/break/return は RESULT.state で in-band 伝播 (UNWRAP macro)。

#### 進捗 (Phase 別)

- **Phase 1 完了** (commit 03a5449f): RESULT 型と UNWRAP / CHECK /
  RESULT_OK 等のマクロを context.h に追加。 korb_cfunc_r_t / 
  korb_dispatcher_r_t typedef。
- **Phase 2 完了** (commit 4352f5f5): context.h 整理、 prologue_cfunc_r_inl
  追加。 method_cache に cfunc_r field 追加。 korb_dispatch_call_cached
  と korb_dispatch_to_method に bridge を入れて、 cfunc_r != NULL のとき
  新 ABI 経路に流す。 RESULT は c->state + VALUE 返り値に変換 (Phase 8
  まで暫定)。
- **Phase 3 完了** (commit d1ae64a4): PoC として ary_eq を新 ABI で
  書き換え、 動作確認。 self-tests 24/24 / rubyspec 17/67 維持。
- **Phase 4 進行中** (commit 3efbcb94): math.c (18 cfunc) + boolean.c
  (18 cfunc) を sweep。 全 cfunc ~680 個のうち ~37 個完了。 残り ~640
  (大物: kernel.c 45, module.c 45, file.c 49, integer.c 52, hash.c 63,
  array.c 97, string.c 102)。

#### 残作業

- **Phase 4 残**: 残 ~640 cfunc の sweep。 builtins/*.c を 1 ファイル
  ずつ。 symbol.c で test_string が silent fail する原因調査 要。
- **Phase 5**: node.def の call 系 node (node_method_call /
  node_func_call / node_method_call_block / node_apply_call) を
  sp staging 規約に。 fp[arg_index] 経由を撤廃。
- **Phase 6**: prologue_ast_general / prologue_ast_simple_inl 系を
  sp 経由 args 受け取りに。
- **Phase 7**: C API helper (korb_eq / korb_str_concat / korb_funcall 等)
  を slot pointer 規約に。
- **Phase 8**: c->state 経路撤廃、 RESULT 化。
- **Phase 9**: 動作確認 + 回帰 fix。

### 2026-05-29 追加: korb_host_class() helper と class def の pin (multiple commits)

- `korb_host_class(c)` helper を object.h に追加 (commit 07dcb17f)。
  `c->current_frame->cref ? cref->klass : current_class` パターンを
  全て置換、 cref chain を walk しながら NULL klass を skip。
- `node_class_def_in` (commit 44123967) / `node_class_def_in_strict`
  (commit 8c854955) を ARO_ROOT_SCOPE 化 — parent / super / klass を
  3-slot pin。
- `node_const_path_get` の eName / name_v / prefix を pin
  (commit 5a01892e)。
- `node_method_call` の r を pin (commit 59c15c5b)。
- `proc_call` (commit 54222f06) / `koruby_run_ast` (commit a79eaf69) の
  THROW handler で eUTE / tag / val / tag_s を pin。
- `visit_roots` の frame chain walk に depth cap 4096 (commit b9247c37)。

rubyspec STRESS+PURGE 15/67 → 17/67 PASS, SEGV 46 → 41。

### 2026-05-29 追加: fresh_env を value stack 化 (commit 8a0a68f3)

if_spec.rb 等の SEGV 解析で発見した重要な root cause:

`korb_yield_slow` で `creates_proc=true` block の fresh_env が
korb_xmalloc (libc) で確保されていた。これにより:

- fp = libc heap addr (低 0x5555... sbrk 領域)
- sp = fp + env_size = libc addr
- c->sp = sp + N (pin) = libc addr
- visit_roots phase (a) は `c->stack_base..c->sp` を walk するが、
  c->stack_base は 128 MB malloc (高 0x7fff... mmap 領域)、 c->sp が
  libc になると range walk が空 loop となり pin slot が scan されない
  → 全 stale 化

修正: fresh_env を c->sp 起点 (= value stack 上、 16M slots) で確保し、
exit 時に c->sp restore。 captured proc は snapshot_env_maybe で libc
に snapshot 化するため closure semantics は維持。

効果: rubyspec STRESS+PURGE 14/67 → 15/67 PASS、 SEGV 50→46。

## 旧現状 (2026-05-10, eleventh pass)

- **自前 test/ruby/**: **24/24 全 OK** (737 件)。
- **CRuby `test/ruby/` (全 135 ファイル)**: **89 / 135 has_pass (66%)、
  1,050,622 pass / 1,405,004 (74.8%)**。 起点 (2026-05-09 sweep #1) は
  62/135、 834,385 pass。 +27 ファイル復活、 +216k pass。
  - dumped core: 7 → 0 (super dispatch / refinement binding cref / mm
    recursion / fiber NULL body / Float SIGFPE / Integer overflow recursion
    / Comparable cmp_eq 全解消)
  - LOAD ERROR: 23 → 1 (残り test_time_tz は Regexp 必要)
  - timeout: 3 → 0 (IO.pipe を unbuffered に)
  - 残 0-pass の 46 ファイルは大半 intentional pending (TracePoint /
    ObjectSpace::WeakMap / Ractor / 真の Refinements / 非 UTF-8 Encoding /
    MJIT / 完全 Marshal / 真の ARGF / callcc)。
- **CRuby `spec/ruby/language/`**: **3,745 pass / 190 fail / 51 err**。
- **optcarrot**: AOT-cached で **~76 fps** (CRuby 43 fps の 1.8x、 yjit
  175 fps の 0.43x)。 100 frames best-of-1。
- **CRuby `spec/ruby/core/` 23 cat 集計** (array hash string integer numeric
  range comparable module proc kernel symbol float exception basicobject set
  rational random gc signal binding class enumerator regexp):
  - **pass=14,434 / fail=3,071 / err=1,014 (= 77.9% pass)**
  - **313 / 930 ファイル perfect (33.7%)**

## eleventh pass で test/ruby 互換性を全面強化

主要な fix と効果:
- **super dispatch bug** (object.c:korb_dispatch_binop): caller block の
  defining_method 漏れ。 `Class#new → user initialize → super` が
  "run_all" 等で lookup されてた致命バグ。 test_string 0 → 1965 pass。
- **UnboundMethod late-binding**: instance_method が name しか保存して
  なかったので、 後で define_method 上書きされると new body へ
  redirect され無限再帰。 captured_method を凍結。 test_super 蘇生。
- **Binding が stack-alloc cref を保存**: class body 中に binding する
  と class body 終了で dangle → SEGV。 cref chain を heap deep-copy。
  test_refinement 蘇生。
- **Struct.new {} block の cref leak**: `Struct.new(:x) do def
  method_missing; ... end end` が outer class の method_missing を
  上書き。 block->cref を一時 swap。 test_marshal 蘇生。
- **Class#clone が singleton method を copy しない**: 旧コードは
  `def AClass.cm1; end` 後 `MyClass = AClass.clone` で cm1 が消失。
  test_module 0 → 725 pass。
- **bootstrap method_missing recursion**: `self.inspect` が再 raise
  する class で `"undefined method '...' for #{self.inspect}"` 構築中
  に SEGV。 thread-local depth で 4 段以上で `(recursion)`。
- **Float SIGFPE**: `(long)(big_double)` の UB を `korb_dbl2int`
  (Bignum fallback) に。 flt_floor/ceil の n<0 div-by-0 修正。
- **Integer 無限再帰**: `2 ** -2^62` で fixnum overflow → 無限再帰。
- **A::B::C = 0 ** 0 の slot collision** (test_primitive)。
- **Array#cycle / repeated_combination / repeated_permutation を C 実装**
  (bootstrap.rb 版は break が nested block を貫通しなかった)。
- **巨大 index 防御**: korb_ary_aset / EVAL_node_aset / ary_aset で
  `LONG_MAX/sizeof(VALUE)` 超え index に IndexError raise (旧 OOM)。
- **IO.pipe unbuffered**: line-buffered だった → readpartial が無限
  block。 test_io / test_optimization の timeout 解消。
- **kwh_save_slot defaulting + FL_KWARGS peeling** in dispatch_to_method:
  `Method#call(**{})` 等で kwsplat が `*args` に紛れ込まない。
  test_keyword 613 → 791 pass。
- **dispatch arg-count error を ArgumentError に**: 旧 RuntimeError
  だったので assert_raise(ArgumentError) が拾えなかった。
- **String#[]= 実装**: 旧 stub。 IndexError raise 含む。
- **object_id を immediate VALUE に**: 旧 `(long)self / 8` は Fixnum
  小値を全部 0 に潰し Hash#hash 完全破綻。
- **Kernel#Float() に strict 検証**: "xyz" → ArgumentError、
  exception:false opt、 nil → TypeError、 Bignum 受理。
- **sprintf %b の width / precision / 0-pad / negative-prefix**。
- **tu_shim 大幅拡張**: Errno (130 const) / Encoding (.find / 70 alias /
  CompatibilityError 等) / Process (UID/GID/CLOCK_*) / Thread::Queue /
  IO 定数 / File::Constants / RubyVM::AbstractSyntaxTree / Bug /
  Socket / Dir.mktmpdir / Tempfile / FileUtils / MemoryViewTestUtils /
  ARGF / trace_var / caller_locations / refine 部分実装。

詳細は [done.md](./done.md) の「CRuby test/ruby/ 全 135 ファイル sweep」
セクション参照。
- **tenth pass で perfect 化したもの**:
  - chilled string 完全実装 (FL_CHILLED + parse.c の prism flag 読み分け +
    Symbol#to_s も chilled): `string/chilled_string_spec` `string/uplus_spec`
  - sized Enumerator + each redispatch: `hash/transform_values_spec`
  - NameError @name/@receiver は method_missing 経由でも記録:
    `exception/name_error_spec`
  - Object#<=>/initialize_copy/clone/dup 既定 hook: `kernel/comparison_spec`
  - GC モジュール (garbage_collect/disable/enable/start): `gc/{garbage_collect,
    disable,enable,start}_spec`
  - Random.new_seed の uniqueness 改善 + urandom 負サイズ: `random/{new_seed,
    urandom,seed}_spec`
  - Integer#allbits?/anybits?/nobits?/sqrt/try_convert/to_r/rationalize/
    numerator/denominator/ord: `integer/{allbits,anybits,nobits,sqrt,
    try_convert,numerator,denominator,ord}_spec`
  - Float#numerator/denominator/to_r: `float/{numerator,denominator}_spec`
  - Range#to_s/eql?/count(infinity for endless): `range/{to_s,eql,count}_spec`
  - Array#combination/permutation Enumerator + binomial size:
    `array/combination_spec` `array/permutation_spec` (+5)
  - Array#fetch/fetch_values/to_a/to_ary/deconstruct: 各 spec full
  - Hash#flatten/transform_keys{,!}/to_h/sort/replace/deconstruct_keys:
    `hash/{flatten,sort,replace,to_h,deconstruct_keys}_spec`
  - String#each_byte Enumerator/strip!/lstrip!/rstrip!: 各 spec full
  - Symbol#intern/name + inspect bare/quoted 判定 fix: `symbol/{intern,name,
    inspect}_spec`
  - Rational#integer?=false / Rational.new 禁止: `rational/{integer,
    rational}_spec`
  - Set#each Enumerator: `set/each_spec`
  - Signal::EXIT / Kernel module 関数 30 件 private: `signal/list_spec`,
    `kernel/{abort,exit}_spec`
  - Numeric#dup/clone/ceil/floor/round/truncate/fdiv: `numeric/{dup,clone,
    ceil,floor,round,truncate,fdiv}_spec`
  - Module lifecycle hook 既定 / BasicObject 既定 hook
  - Comparable#== identity 短絡 + Float 0.0 + NoMethodError swallow

## 旧現状 (2026-05-08, ninth pass)

- **CRuby `spec/ruby/core/` 14 主要 cat**: ninth pass で perfect 化:
  - `core/numeric/{ceil,floor,round,truncate,fdiv,dup,clone}` (7)
  - `core/integer/{allbits,anybits,nobits,sqrt,try_convert,to_r,rationalize}` (7)
  - `core/string/{lstrip,rstrip,strip,chomp,sum,getbyte,append,to_f}` (8)
  - `core/hash/{flatten,deconstruct_keys,replace,sort,to_h}` (5)
  - `core/array/{fetch,fetch_values,deconstruct,to_ary}` (4)
  - `core/exception/{message,inspect,name}` (3)
  - `core/symbol/{symbol,float,inspect}` (3)
  - `core/range/inspect` (1)
  - `core/comparable/equal_value` (mostly)
  - `core/module/{const_get,extended,included,prepended}` (4)
  - `core/kernel/{Array,Float,Hash,Integer,String}` (5 — private 化)
- **CRuby `spec/ruby/core/` 14 主要 cat (eighth pass)**: **13,320+ pass、 216+ ファイル perfect**
  本セッションで perfect 化したもの一覧 (代表):
  - `core/array/{any,clear,assoc,plus,try_convert,compact,rassoc,at,insert,
    pop,shift,drop,delete,rotate}` (14)
  - `core/hash/{new,try_convert,to_proc,default_proc}` (4)
  - `core/string/{hex,oct,try_convert,plus,include,prepend}` (6)
  - `core/integer/{multiply,plus,uminus,divide,digits,bit_and,bit_or,bit_xor}` (8)
  - `core/proc/{lambda,allocate}` (2)
  - `core/comparable/{lt,gt,gte,lte,clamp}` (5)
  - `core/kernel/{class,raise,instance_of,instance_variable_defined,dup,
    case_compare,throw,respond_to,respond_to_missing}` (9)
  - `core/binding/clone`
  - `core/module/{const_set,class_variable_set,deprecate_constant,lt,lte,gt,gte,
    comparison,private_class_method,public_class_method,attr_reader,
    attr_writer,attr_accessor,attr,private,protected}` (16)
  - `core/numeric/comparison`
  - `core/float/divide`
- **Binding**: **150 pass** (完全互換)。

## 旧現状 (2026-05-08, fifth pass)

- **CRuby `spec/ruby/language/`**: 3,726 pass / 35 perfect (mock-shim slot fix +301)。
- mock-shim slot bug 解消、 lambda opt args、 anon-rest+post 修正、
  hash literal string-key freeze、 missing keyword 全キー列挙、
  Proc#parameters slot indexing、 eval `__FILE__`、 clone(freeze:)、
  proc post-rest extra-drop、 case/in slot 衝突。

## 旧現状 (2026-05-08, fourth pass)

- **自前 test/ruby/**: **24/24 全 OK** (737 件)。
- **CRuby `spec/ruby/language/` (rubyspec, 65)**: **3,404 pass / 40 perfect**。
- **CRuby `spec/ruby/core/`** (13 主要 cat): **12,407 pass、 133 ファイル perfect**。
  本ラウンドで perfect 化: array (assoc/clear/dig/include/plus/take),
  hash (compact/delete/dig/empty/any/fetch/fetch_values/new/reject),
  module (case_compare/protected_instance_methods/public_instance_methods),
  class (new), range (new/hash), float (lt/le/gt/ge/uminus),
  integer (bit_length), string (bytes)。
- **Binding**: **150 pass** (完全互換)。

---

## 旧現状 (2026-05-08, second pass)

- **自前 test/ruby/**: **24/24 全 OK** (ArrayLshiftRedef 解決)。 合計 737 件 全 pass。
- **optcarrot**: CRuby と動作・出力一致。
- **CRuby `test/ruby/` (in-scope 67 ファイル)**: 1,108,357 pass / 77.5%。
- **CRuby `spec/ruby/language/` (rubyspec, 65 ファイル)**:
  **3,402 pass / 3,634 (93.6%)、 40 ファイルが 100% perfect**。
  本ラウンドで rescue / class_variable / yield / for / safe_navigator が
  perfect 化、 shim の evaluate を it block で wrap した波及で +119 pass。
- **CRuby `spec/ruby/core/`** にも改善が波及: Array.try_convert を実装、
  shim の MSpecNegatedExpectation で predicate? 系の should_not 反転を
  正しく扱う (Hash#empty? / Hash#any? が perfect)。
- **CRuby `spec/ruby/core/binding/` + `core/kernel/{eval,binding}_spec`**:
  **150 pass** (Binding 自体は 100%、 残るのは Refinements / IRB の out-of-scope のみ)。
- **CRuby `spec/ruby/core/` 主要カテゴリ**:
  - `kernel`: 6,489 pass / 293 fail / 145 err
  - `string`: 1,800 pass / 1,127 fail / 206 err
  - `array`: 1,171 pass / 436 fail / 67 err
  - `integer`: 869 pass / 172 fail / 183 err
  - `hash`: 400 pass / 97 fail / 33 err
  - `proc`: 195 pass / 60 fail / 25 err
  - `float`: 120 pass / 35 fail / 74 err
  - `symbol`: 117 pass / 69 fail / 31 err
  - `range`: 98 pass / 79 fail / 11 err
  - `binding`: 58 pass / 0 fail / 2 err (irb_spec の IO.popen など out-of-scope のみ)

## §0 範囲外 (project policy / user 指定)

これらは TODO ではなく **scope の外**。 触らない。

| 項目 | 理由 |
|---|---|
| Regexp (`=~` / `/.../` / `match` / `scan`) | astrorge 経由で integrate する方針 |
| Encoding-aware String (`String#encoding`, `force_encoding`, multi-byte succ, m17n) | byte sequence のみ |
| Thread / Mutex / Queue / ConditionVariable / SizedQueue | single-threaded only |
| Fiber Scheduler / Async I/O | 範囲外 |
| ObjectSpace 走査 / 詳細 | excluded |
| GC.* (start / stress 以外の細部) | runtime 内部依存 |
| TracePoint / RubyVM | 範囲外 |
| NaN-boxing | 値表現変更禁止 (project memory) |
| Refinements (`refine` / `using`) | 言語拡張、 範囲外 |
| Ractor | 並列拡張、 範囲外 |
| ruby2_keywords | 互換性 marker、 範囲外 |
| DidYouMean (NoMethodError 提案) | 別途 |
| Random reproducibility (`srand` で seed 一致) | 範囲外 |
| Process / spawn / fork / `ruby_exe` 子プロセス起動 | 範囲外 (子プロセスを介する spec は skip / 0 pass で許容) |
| IRB (`Binding#irb`) | 対話的 IRB は対象外 |

mspec_shim はこの一覧の constant を未定義時に skip 扱いにする。

## §A 完全 perfect 候補 (残 fail/err が 1〜4 件)

「あと数件で 100% pass」 になる language spec。 直近の作業優先度高め。
本ラウンドで rescue / class_variable / yield / for / safe_navigator は
perfect 化済み。

| spec | pass / fail / err | 原因の見当 |
|---|---|---|
| `variables_spec` | 168 / 2 / 0 | lambda 内 eval から外側 lvar 書き込み (lexical scope chain walk + multi-scope prism 連携) |
| `method_spec` | 268 / 5 / 0 | Ruby 3.x の特殊 param 系 (`def m(*, a)`、 `def m(a, **nil)` 等) |
| `class_spec` | 66 / 2 / 2 | Class.new block 内の `class X` の lexical scope (§B3) |
| `super_spec` | 117 / 1 / 2 | BasicObject 経由 super, 可視性変更後 super, define_method 経由の RuntimeError |
| `block_spec` | 180 / 2 / 2 | block の SyntaxError 系 (循環引数参照) と to_proc 周り |
| `metaclass_spec` | 22 / 1 / 1 | metaclass of metaclass (深い singleton chain) |
| `constants_spec` | 142 / 3 / 1 | private constant access、 unicode const name |
| `return_spec` | 51 / 3 / 0 | return inside class block の LocalJumpError、 toplevel return warning |
| `hash_spec` | 66 / 4 / 0 | string key freezing、 Ruby 3.x の `m(**h)` 非コピー特殊規則 |
| `keyword_arguments_spec` | 45 / 8 / 3 | `**hash` empty 扱い (§B2) |
| `regexp_spec` | 43 / 26 / 2 | astrorge 待ち |

## §B 中インパクト項目

### B1. block frame の backtrace ラベル

CRuby は block 内 raise の backtrace を `:in 'block in foo'` (or `:in 'block in <main>'`) と表示する。 koruby は呼び出し元の AST node line を貼るだけで block を独立 frame として表示しない。 `rescue_spec` / `ensure_spec` の "deepest rescue block" 系と、 backtrace API ベースの spec が複数 fail。 backtrace builder で running_block の生成位置を frame として挟む必要あり。

### B2. 空 kwargs hash の自動消失

`m({}, **{})` で空 kwargs hash が positional に流れる現象。 CRuby 3.x は empty kwargs hash を call-site で消去する。 `keyword_arguments_spec` で十数件 fail。 call site の args 構築を kwargs と positional に明示分離する必要あり。

### B3. Class.new block 内の `class X` lexical scope

`Class.new do; class X; end; end` で、 X は block 作成時の lexical scope (= 外側) に作られる。 koruby は `current_block->cref` を nk に push するため X が anon class 配下に入る。 builtins/module.c の `class_new` の cref 操作を見直し。

### B4. SyntaxError message 一致

`-> { eval "..." }.should raise_error(SyntaxError, /pattern/)` で prism と CRuby のエラーメッセージが違うため fail。 mspec_shim 側の substring matcher で半数は救えているが、 「prism は受け付けるが CRuby は SyntaxError」 のケース (例: yield in singleton class) は個別対応必要。

### B5. eval body から外側 block の lvar 更新

`eval("a = 2")` を block 内から呼ぶと、 eval body は新規 lvar `a` を作るだけで block の `a` を更新しない。 prism の depth 1+ scope への書き戻しを実装する必要あり。 `kernel/eval_spec` の "updates a local in a scope above a surrounding block scope" など。

## §C 残小バグ

- [ ] **ArrayLshiftRedef** (`test/test_basic_op_redef.rb`) — Array#<< の redef guard が発火しない (4/4 fail)
- [ ] **Float#floor(n) の Float 精度** — Float 表現の本質 (291.4.floor(2) は 291.39 になる)
- [ ] **proc/lambda の post-rest の parameter 名** が `[:req]` のまま
- [ ] **m17n strings**: encoding を真面目に処理してないので multi-byte 周りで slot wrap や split で誤動作
- [ ] **`def f(&nil)` / `def f(**nil)`** (Ruby 3.4) — PM_MISSING_NODE で吸収済みだが parameters に反映されない

## §D Module / Class 内部

- [ ] `Class#attached_object` (singleton class API)
- [ ] `Module#prepend_features` / `Module#append_features` 公開
- [ ] `Module#const_source_location` の真の実装 (line 番号を保存)
- [ ] `Module#constants(false)` の `inherit` 引数の細部
- [ ] `private constant :X` (定数の visibility)

## §E 標準ライブラリ — 残小物

- [ ] `String#bytesplice` (byte-level 編集)
- [ ] `Struct`: `keyword_init: true`、 `IndexError` on bad index、 `to_h` with block
- [ ] `Data`: 真の Data 実装 (現状 Struct 経由)
- [ ] `Complex` / `Rational`: `coerce` 経由の polar canonicalization、 NaN/Infinity の to_s 表記、 expt special angles
- [ ] `StringIO` クラス自体 (test の依存により $_ 系 spec が skip 多い)

## §F core spec の伸びしろ

`spec/ruby/core/` で **現状 60% 未満かつ実装可能** なクラス。 実装済み
基盤 (Binding / eval / proc / hash) を生かせば数百件単位で増える。

| カテゴリ | pass | fail | err | 備考 |
|---|---:|---:|---:|---|
| `string` | 1800 | 1127 | 206 | encoding 系除外でもまだ伸びしろ大 |
| `array` | 1171 | 436 | 67 | BasicObject splat、 reject_bang の余地 |
| `integer` | 869 | 172 | 183 | Float 精度系 + bignum 細部 |
| `range` | 98 | 79 | 11 | endless range step / first(n) の細部 |
| `symbol` | 117 | 69 | 31 | inspect / to_proc / encoding 関連の細部 |
| `proc` | 195 | 60 | 25 | curry / parameters / arity の細部 |
| `float` | 120 | 35 | 74 | Float 表現本質、 step / divmod 精度 |
| `hash` | 400 | 97 | 33 | merge with block / compare_by_identity |

## §G 過去セッション履歴

実装済み変更の履歴は git log を参照。 手を動かす前に直近 30 commits 程度を
ざっと眺めて既に試した方針を再走しないこと。 直近の大改修:

- 2026-05-07: Binding 完全実装 (binding TOTAL 110 → 150)
- 2026-05-07: Kernel#eval coerce + String#b + block fp shift + nested eval
- 2026-05-06: eval-with-binding (caller の lvars 参照、+99 pass)
- 2026-05-05: mock support (should_receive)、 LocalJumpError 検出
- 2026-05-04: describe→context→describe のローカル破壊修正

## §H precise GC (2026-05-28)

`BARUBY_GC_STRESS=1 BARUBY_GC_PURGE=1` (= per-alloc GC + mprotect ベース
stale-ptr 検出) 下で test 走行:

- 8-test 基本 battery (min1/def1/class1/fib10/method_chain/justmul/
  string_op/test_seq4): **normal / STRESS / STRESS+PURGE 全 8/8 pass**。
- 完全 test/ ディレクトリ (24 ファイル):
  - **normal**: **24/24 (全 OK)**
  - **STRESS**: **24/24 (全 OK)**
  - **STRESS+PURGE**: **24/24 (全 OK)**

  baseline (548e616a) では STRESS だけで 3 件 fail だったので、 全件
  解消。 test_eq_redef / test_alias_redef / test_basic_op_redef の
  全 inspect 系 SEGV / arg routing / sp restore / framework forward
  bug を fix。 3 run 連続 24/24 で deterministic。

### 残 test_basic_op_redef STRESS の root cause = Phase 3 未完

`korb_ary_new_capa` (= 配列) と `korb_hash_new` (= ハッシュ) は libc
xmalloc で確保 (= GC arena 外)。 visit_roots phase (a) が value stack の
slot を走査して forward_edge を呼ぶ際、 slot 値が libc 配列を指している
と forward_payload は arena obj として扱い:
- aligned = ALIGN8(gc_size=0) = 0 (= memset で 0 のまま)
- memcpy 0 bytes (= no-op だが to_top は事実上 進まない)
- HDR_SET_FORWARDED → libc 配列の gc_flags に bit 立て
- fwd_overlay_set で payload+8 (= klass field!) に to_top を書き込み
- *slot = to_top (= libc 配列の今後の参照先が壊れた arena 領域に向く)

結果、 配列 lvar の値が "to_top に置かれた直近の arena obj" (= 多くの
場合 main_obj) に化ける。 `a.class = Main` の理由。

#### Phase 3 試行で見つかった "shadow ref" 問題

`korb_ary_new_capa` だけ arena に migrate して試したら NORMAL は 24/24
維持されたが、 STRESS で全 24 件 SIGABRT (= forward_payload が "GC BUG
forward to-space" abort)。 原因は **mixed allocation の shadow reference**:

- 配列を arena に移行 → scan_edges T_ARRAY が a->ptr[i] (= libc buffer
  の中身) を visit して contains の arena ref を forward
- しかし libc 側 container (= ハッシュ entry chain, 一部 method
  table) が arena obj への参照を持っている → これらは visit_roots
  にも scan_edges にも触られない (= GC から「見えない」shadow ref)
- 結果: shadow ref しか持たない arena obj は collect 対象になり死ぬ
- 次の cycle で他の経路 (= migrated 配列の ptr buffer 等) から
  死んだ obj への参照を visit すると "to-space past to_top, not
  forwarded" abort

根治には **全 koruby container を一斉に arena 化** が必要 (= partial
migration では shadow ref が新たに発生する)。 対象:
- korb_array (struct + ptr buffer)
- korb_hash (struct + entries)
- korb_range (struct)
- korb_float (struct)
- korb_bignum (struct + mpz_t)
- korb_proc (struct)
- その他 libc 構造体

各 type について scan_edges per_type の visitor を追加し、 内部の
arena 参照 (klass field, content slots) を全て visit_ptr_slot で
forward する必要あり。 大規模 refactor、 1 commit 単位では収まらない。

### 解消した test_alias_redef NORMAL の logical fail

調査の結果、 `assert_equal` (= optional msg=nil 持ち method) の
prologue_ast_general に `c->current_frame->fp += arg_index;` の
shift が抜けていた pre-existing バグだった。 body が caller の
lvar slot から params を読んでいた。 commit d907a658 で fix。
prologue_ast_simple_inl / prologue_ast_full_inl_K は最初から shift
していて、 general だけ漏れていた。

### Phase 6 audit: 全 15 backend BUILD + STRESS sweep

`extern int g_in_root_scan;` を visit_roots phase 区切りログ用に
置いていたが、 これは gc_copy.c の診断専用 globals。 他 14 backend
で link 失敗していた。 撤去 (commit 75c9c9c5)。

**15 backend 全 BUILD OK + min1/fib10/class1/string_op/test_seq4 の
5-test sub-battery で run 5/5**。 全 24 test/ ファイル STRESS sweep:

| backend           | STRESS 24件中 | 備考                                 |
|-------------------|--------------|--------------------------------------|
| **copy** (default)| **24/24**    | + STRESS+PURGE も 24/24 全 OK!       |
| mark              | **24/24**    | 非 moving。 PASS                     |
| mark_freelist     | **24/24**    | 非 moving。 PASS                     |
| immix             | **24/24**    | 非 moving + line/block。 PASS         |
| bump              | **24/24**    | 非 moving + bump alloc。 PASS         |
| none              | **24/24**    | no-op GC。 PASS                      |
| mark_compact      | 23/24        | test_basic_op_redef のみ              |
| mark_card_gen     | 20/24        | _gen 系                              |
| mark_bitmap_gen   | 19/24        | _gen 系                              |
| mark_bump_gen     | 5/24         | _gen 系                              |
| copy_gen          | 0/24         | _gen + moving。 session 通じて変動なし (= 以前の audit の "17/24" は集計スクリプトの bash `!`-history 展開ノイズによる過大集計、 確認したら最初から 0/24 だった)。 |
| mark_gen          | 0/24         | _gen 系                              |
| mark_compact_gen  | 0/24         | _gen 系                              |
| immix_gen         | 0/24         | _gen 系                              |
| mark_gen_inc      | 0/24         | _gen 系 incremental                  |

非 moving + 非 generational は 24/24 全 pass、 copy は **STRESS+PURGE
含む 全モード 24/24 全 OK**。 _gen 系 backend は framework 側
write barrier 不整合があり、 別 session で深堀必要。

## §I benchmark / rubyspec sweep (2026-05-28)

### benchmark/bm_*.rb (27 ファイル)

| mode | pass | fail | SEGV | 備考 |
|---|---|---|---|---|
| NORMAL | 22 | 5 | 0 | pre-existing 仕様未実装 |
| STRESS | 19 | 8 | 0 | timeout + libc shadow ref |
| STRESS+PURGE | 19 | 8 | 1 | nbody が mprotect で死亡 |

nbody の PURGE SEGV: `korb_class_find_method` で klass deref → libc-
allocated array の `klass` field が arena class の stale addr を持って
おり、 mprotect 経由で SIGSEGV。 libc-arena shadow ref の典型例。

### rubyspec (~/ruby/src/master/spec/ruby/core/) sweep

NORMAL mode で mspec_shim 経由実行: 多くの spec で test setup 中の
`empty?` for nil 等の pre-existing koruby 仕様未実装エラー。 STRESS+
PURGE 下では SEGV (= 同じ libc-arena shadow ref パターン)。

### 根本対処 (= Phase 3 完了相当)

container を全 arena 化:
- korb_array (struct + ptr buffer)
- korb_hash (struct + buckets/entries)
- korb_range / korb_proc / korb_float / korb_bignum 等

partial migration (= 一部だけ) では shadow ref が新たに発生する
ことが実験で確認済 (4ed470fb で試行)。 一括移行が必要。 大規模
refactor、 1 commit に収まらない別 session 規模。

### Top-level closure pre-existing 不具合

```ruby
x = 10
[1,2,3].each { |i| x += i }
puts x   # 期待: 16、 実際: NoMethodError "undefined method '+' for nil"
```

method body 内では正しく動作 (test_block.rb の test_block_capture 等):

```ruby
def f
  s = 0
  [1,2,3].each { |x| s += x }
  s
end
puts f   # 6 (= 正)
```

baseline (548e616a) でも同じ症状で再現、 GC 関連ではない pre-existing
koruby_precise の top-level closure 不具合。 sibling `sample/koruby`
では top-level でも正しく動作。 koruby_precise の CTX → frame
migration 時に block env capture の semantics が壊れた可能性。

benchmark/bm_times / bm_each / bm_inject / bm_nbody がこの問題で
NORMAL から fail。 別 session の課題。

**深掘り途中メモ:**

AST は top-level と method body で identical (envsize=39、 block 構造 同じ
`1 4 7 0`)。 proc_call の self_recursion path (= prev_fp == new_fp)
にて fresh_env = c->sp に clone する。 method body 内では正しく x = 10
が読まれるが、 top-level では block.fp[0] が nil を返す。

**root cause 推定:** `korb_yield` fast path (object.h:841) と
`korb_yield_slow` (object.c:2374) は body dispatcher に `sp = bfp`
を渡している (= blk->env と同じ addr)。 一般 method body は
`sp = fp + locals_cnt` を渡すので、 block body の bake walker
offset 規約が異なるか、 もしくは現状の dispatcher 呼び出しが間違って
いる可能性。

**試行と再現結果 (2 回目):**
- `sp = bfp + env_size` に変更 → top-level closure 通る (x = 16)
- ただし NORMAL test/ が 24 → 13/24 に regression
  (test_array / test_block / test_block_arg / test_class / test_cpu_corner
  / test_fiber / test_hash / test_misc / test_object_alloc / test_range /
  test_yield)
- "EXCEPT test_reduce: NoMethodError: undefined method '+' for :test_reduce"
  のように、 一見無関係な箇所が壊れる → block 外の lvar 読み書きまで
  影響する深い path 修正が必要

**結論:** bake walker と yield 経路の規約整合 (= 全部 frame top 基準に
するか、 block だけ bfp 基準にして bake 側も追従するか) は単純な
1-liner では実現不可。 bake walker (parse.c) の offset 計算と全 yield
経路の dispatcher 呼び出しを同時に改修する必要あり、 multi-commit な
大改修。 別 session の課題。

### 一括 migration 試行ログ (2026-05-28, session 末)

`korb_ary_new_capa` / `korb_hash_new` / `korb_range_new` /
`korb_float_new_heap` / `korb_proc_new` を `aro_gc_alloc` に同時切替
+ scan_edges T_HASH 実装した試行。 結果:

- NORMAL: 24/24 維持 (= 意味的に破壊なし)
- STRESS: **24 → 3/24 (大幅 regression)**
- STRESS+PURGE: **24 → 0/24 (完全崩壊)**

bootstrap で `NameError: undefined method 'numerator' for Rational`、
`BAD SLOT abort-imminent` 連発。 推定原因:
- container migration が bootstrap 中に発生する GC を increase
- ある時点で class.methods (= korb_method_table、 hash 構造別物) や
  別の libc-allocated 構造への参照が stale 化
- 私の T_HASH scan_edges は korb_hash 用、 method_table は scan されず

container 系を一括 migration するだけでは足りず、 関連する全 libc
構造 (method_table、 const_entry chain、 includes 配列 等) も同時に
scan_edges に load する必要あり。 さらに大規模、 別 session 規模を
超える本格 refactor。 revert で 24/24 復帰。

このセッションの主要 fix (commit 686f01f0 〜 13edbcb7):

- **visit_roots phase (c+d) 統合**: frame 毎に cref chain を walk。
  method dispatch で frame.cref = mc->def_cref (= cref_dup の SEPARATE
  chain) になるため、 head 一本だけ走査では outer frame の cref->klass
  が stale 化。 PURGE で class body 内 const_set が SEGV した直接原因。
- **c->sentinel_frame + c->top_cref を visit_roots で unconditional に walk**:
  korb_eval_string が top_frame.prev=NULL で push するため、 sentinel は
  head chain から外れる。 sentinel.self (= main_obj) が bootstrap 中の
  GC で stale 化し、 user script 側の最初の puts 1 が dispatch SEGV。
- **node_class_def / node_class_reopen / node_module_def を synthetic
  frame push に統一**: 旧 impl は outer frame.current_class / cref を
  その場で書き換え、 C-local prev_class を across-body-GC で保持。
  GC 後に書き戻すと stale arena ptr が outer に書かれ、 次の GC で
  BAD SLOT。 synthetic 構造体 (.cref/current_class/self を inherit)
  を c->current_frame として push する形に統一。
- **korb_dispatch_to_method AST path で frame2 が cref/current_class/
  current_file を inherit**: frame2 の struct literal がそれらを
  初期化せず NULL のまま → body 側 const_lookup が NULL 経由で
  garbage を返して SEGV。
- **node_str_concat を ARO_ROOT_SCOPE で保護**: `korb_to_s_dispatch` が
  GC を発火するため C-local の累積 r と part が across-GC stale。
- **globals 撤去**: `koruby_top_cref` / `koruby_top_sentinel_frame` を
  file-scope static から CTX フィールドに移行。 multi-interpreter
  共存可能に。 struct korb_frame の定義を struct CTX_struct の前に
  移動して by-value embed を可能化。

### 残 pre-existing flakes (baseline からの持ち越し)

- **test_alias_redef** (normal も STRESS+PURGE も fail): const_lookup
  内で c->current_frame が stale stack 領域を指す。 cref->prev =
  korb_dispatch_to_method 内の saved RIP。 どこかの dispatch path が
  c->current_frame を local frame に push して unwind 漏れしている疑い。
- **test_basic_op_redef** / **test_eq_redef** (normal pass、STRESS / STRESS+PURGE で fail):
  GC root scan が visit_ptr_slot に libc code address (= `_int_malloc+N`)
  を渡して SEGV。 cref chain の prev が garbage stack 領域を指し、
  そこから wild ptr へ chain される。 method aliasing が同じ
  `struct korb_method *` を複数 entry に登録するため、 def_cref chain
  に corrupted node が混入する可能性。

### 組み込み型の instance variable (cont.16 で発覚)

- 現状 `@ivar` を持てるのは `KorbObject` (KORB_OBJ_OBJECT、ユーザ定義
  クラスのインスタンス + top-level main) のみ。`node_ivar_get` は
  `\!KORB_OBJECT_P(self)` で nil を返し、`node_ivar_set` は raise する。
  本来 Ruby では String/Array/Hash 等の組み込みオブジェクトも ivar を
  持てる (`"x".instance_variable_set(:@a, 1)` 等)。
- 実装するなら ivar ストアを KorbObject 専用ペイロードから外し、
  全 heap オブジェクト共通のサイドテーブル (obj identity → ivar hash)
  か、各 heap struct への ivar スロット追加が要る。Fixnum/Symbol 等の
  immediate は別扱い (CRuby は generic_ivar table)。優先度低 (corpus に
  ほぼ無し)。

### perf push 中に発覚した correctness gap (2026-06-18)

- (実装済 — todo 誤記、2026-06-19 確認) **`retry`**: node_retry +
  node_begin の KORB_RETRY ループ + parser PM_RETRY_NODE すべて存在し動作。
- **custom `hash`/`eql?` の Hash キーが効かない**: ユーザ定義 `hash`/`eql?`
  を持つオブジェクトを Hash キーにしても dispatch されず、identity 扱いに
  なる(別オブジェクトが別キー化、lookup miss)。korb_value_hash /
  korb_value_eq が組み込み型のみ。object キーは user hash/eql? dispatch が要る
  (= GC-sensitive: hash 計算が user code を呼ぶ → rehash 中の GC 安全性に注意)。
  既知の大物。
- (修正済 2026-06-19) **`{1=>"a"}[1.0]` が "a" を返していた** (eql? でなく ==):
  korb_hash_find の linear scan が korb_value_eq_fast (==) を使い、1 と 1.0 を
  同一キー扱いしていた。korb_value_eql (numeric-type-strict、既存だが Hash で
  未使用だった) に切替。Integer/Float/Rational が別キーに (Set#uniq は既に正)。
- (修正済 2026-06-18) Array#sort/min/max/min(n)/max(n) の <=> dispatch、
  AOT の -DKORB_HAVE_GMP 欠落 → 別 commit で対応済。
- (修正済 2026-06-19) **Bignum `>>`**: korb_m_int_rshift に bignum ガードが
  無く、bignum self を FIX2LONG してゴミを返していた（lshift にはあった）。
  bignum→korb_int_shift(self,-sh)、負シフト(=左シフト)の Fixnum overflow も
  Bignum 昇格するよう lshift と対称化。
- (修正済 2026-06-19) **beginless/endless range index**: `arr[1..]` / `s[..3]`
  等が「no implicit conversion into Integer」で raise していた（rbegin/rend が
  nil を弾いていた）。Array#[]/[]=/slice!、String#[]/[]= の5箇所で nil 端点を
  beginless→0 / endless→長さ として扱うよう統一。
- **`Integer == user_obj` の reverse coercion 未対応**: `42 == E.new`
  (E が `==` を true 返しで定義) が CRuby では true だが koruby は false。
  Integer#== が相手を理解できない時、CRuby は逆方向 (`E.new == 42`) を
  試す。node_eq の korb_value_eq は組み込み型のみ判定。優先度低
  (corpus 影響なし、pre-existing)。
- (修正済 2026-06-18) `send(:top_level)` / `__send__` が top-level def に
  届かなかった (private Object method 相当) → korb_send_impl の main miss で
  global function table fallback。public_send は privacy で raise。

## rubyspec mining 由来 (2026-06-19, /tmp/rspec_gen バッチ)

新規 mined assertion を CRuby と diff して発見・修正 (20 commit, 全て
corpus 89267/5 + STRESS+PURGE + AOT verify 済)。total diff 4165→3678。

**RESOLVED (session 2, 2026-06-19):** main self methods; bignum bitops;
Float#arg(-0.0)/coerce(String); Hash#delete blk; sprintf %+b;
Method#arity/owner/original_name/parameters; **UnboundMethod**
(instance_method/unbind/bind/bind_call); defined?(A::B); Numeric⊇Comparable;
Object#singleton_class + attached_object; Integer/String.try_convert;
Integer div/divmod/modulo/remainder の Rational/Bignum operand;
String#to_r (leading-dot/_/dec÷den); tr/delete lone-^ literal;
ArithmeticSequence#size analytic (HANG fix + float); Numeric#step(to:,by:);
**Complex** abs2/numerator/denominator/rationalize/infinite?/integer?/
<=>/eql?/**(pow)/(div)/polar + inspect; **3 SEGV fix** (|&b| block,
Proc.new{}, Enumerator.new{} eager yielder).

**残 (deferred — 大物 or skip):**
- **Integer#size (Bignum)**: limb 単位の実装依存値。spec も "machine
  dependent"。matching 非現実的、skip。
- **to_enum / enum_for(:method)**: block 無し Enumerator を method 名生成。
  block-from-C 駆動が要る。中規模。
- **Set#compare_by_identity / Hash#compare_by_identity** (set_000)。object_id 基盤要、moving GC で大物。
- **Method#parameters の名前** (ISEQ は node_entry に名前未保持 → 種別のみ。
  名前を出すには parser で名前テーブルを node_entry に保持する変更)。
- **nested Complex()** (kernel_000)。

**RESOLVED (session 3, 2026-06-19 — 大物バッチ):**
- Class < Module < Object MRO (Class.superclass/ancestors/is_a?(Module))。
  残: module オブジェクトの metaclass がまだ Class (M.is_a?(Class) が誤 true)
  — Module 系メソッドを KORB_C_CLASS から Module へ移す別作業。
- define_method(name, Method/UnboundMethod) — 定義を copy + ancestor チェック。
- lazy Enumerator drop/take/drop_while (無限ソース対応、per-op state)。
- Complex 残り: marshal_dump, to_r, <=>, eql?, abs(0,0)=0、Integer#to_r/
  rationalize の Bignum 精度バグ修正。complex_000 121→2 (残は float ULP)。
- **float ULP (Complex**Float の polar)**: libm の演算順序依存で bit 一致不可
  と確定 → skip。

- **Binding / TOPLEVEL_BINDING (実装済 session 3)**: KORB_OBJ_BINDING 型。
  `binding` = node_binding (parser が scope の name→sym 表 + frame base + self
  を bake、frame を closure と同じ open-KorbEnv で捕捉 → closure と共有・frame
  退出後も生存)。local_variable_get/set/defined?/local_variables/receiver
  (新規 local は extra side-hash)。eval(str, binding) は prism declared-scope
  で binding local を宣言 (prism は単一 scope を program 自身に畳み込む →
  depth-0) → eval frame を binding から seed、実行、write-back (既存→env、
  新規→extra)。TOPLEVEL_BINDING は main.c で toplevel frame を捕捉。
  GC 罠: write-back の hash alloc で GC が binding を動かす → cached VALUE が
  stale → args slot から再読込で修正。t/hand/binding.rb (28 assert) で
  interp+STRESS+AOT 検証。残: extra-local の eval 順序の細部、cref/定数解決。

**残:**
- skip 対象: regex (→astrorge), Encoding/US_ASCII, 非 ASCII case,
  Thread/Queue/Fiber 細部, IO/File/Dir/Process/Time, MT19937 完全一致, Marshal。
- module オブジェクトの metaclass が Class (M.is_a?(Class) 誤 true)。
- Method#parameters の ISEQ 名前 (node_entry に名前未保持)。
- nested Complex(); Set/Hash#compare_by_identity (object_id 基盤要)。
