# Array moving-GC 移行 (payload-as-VALUE) — 完了報告

2026-06-01。koruby_precise の Array を **完全な moving GC object** に移行した記録。
設計詳細は [array_payload_value.md](array_payload_value.md)、callsite 変換ルールは
[payload_transform_spec.md](payload_transform_spec.md)、STRESS rooting は
[stress_rooting_spec.md](stress_rooting_spec.md) を参照。

## 成果

| 指標 | clean HEAD | 旧 WIP (struct-only) | **payload-as-VALUE** |
|---|---|---|---|
| rubyspec PASS (182-file list) | 7492 | 2922 | **12973** |
| default suite | 25/25 | 25/25 | **25/25** |
| STRESS+PURGE suite | 25/25 | 21/25 | **22/25** |

rubyspec が clean HEAD 比 **+5481** (旧 struct-only WIP の 4.4 倍)。Array が moving GC で
正しく動くようになり、以前 crash/abort していた多数の spec が完走するようになった。

## STRESS+PURGE rooting campaign (2026-06-01 第2弾)

curated 28-suite は通っても、rubyspec を **STRESS+PURGE で広く流す** と多数の latent crash が
残っていた。`tools/sp_stress_sweep.sh` (182-file fixed list を STRESS+PURGE で完走 PASS/CRASH 集計)
で計測しながら、支配的 crash site から潰した。

| 段階 | rubyspec STRESS PASS | CRASH |
|---|---|---|
| campaign 開始時 | 179 | 138 |
| dispatch recv + array builtins | 555 | 81 |
| **eval_frame_chain** + lshift + rng_each | 1092 | 80 |
| class_new/obj_dup/module/str_clone/const_path/str | 1964 | 53 |
| **yield self-chain** + str/module builtins | 2093 | 47 |
| ary_mul / str_tr_bang / instance_variables / splat root-stack | 2107 | 46 |
| **rest-gather staging** (|*xs| 修正) | 2246 | 38 |
| ary_try_to_int / str_replace / node_bit_or / fetch_values | 2605 | 34 |
| Range→arena 移行 + range.c cascade | 2739 | 29 |
| str_unpack / ary_try_to_int 等 + **Fiber stack GC scan** | **2802** | **21** |

PASS が **15.7×** (179→2802)、CRASH が **−85%** (138→21)。

### 第3弾 (2026-06-02): 実 SEGV cluster 潰し (tools/sp_segv_scan.sh で 16→6)

`tools/sp_segv_scan.sh` (182-file を SEGV / NOTR / OK に分類) の **実 SEGV** を crash site
ごとに gdb で特定して潰した。SEGV ファイル数 **16 → 6**。各 fix は 1 commit + gate (28/28・27/28) 確認。

| fix | 解消した spec | 種別 |
|---|---|---|
| **node_apply_call splat receiver** | pp / warn / proc-clone (ruby_exe 経由) | **correctness バグ** (下記) |
| Kernel#` の result string を sp park | (同上 ruby_exe backtick の leaf) | IDIOM D |
| str_start_with / str_end_with の self 再読込 | symbol/start_with | IDIOM A |
| ary_min / ary_max を synthetic frame 化 | range/max | IDIOM B |
| korb_inspect_inner T_RANGE を sp park | kernel/rand | IDIOM A |
| kernel_catch tag を synthetic frame park | (catch THROW path; UTE path は残) | IDIOM B |
| obj_extend で self/cur を korb_class_new 後再 derive | kernel/singleton_method | IDIOM C |
| kernel_eval forward[] を korb_str_new_cstr 後に詰める | (eval 入口 stale; 深部は残) | arg-order |
| IO.pipe で IO class 再読込 + io handle park | kernel/select | IDIOM C |
| **Binding を GC scan** (self/lvars/extras/cref) + eval self/result root | kernel/__dir__ | framework gap |
| **node_ensure の body 結果を ensure 節跨ぎで root** | kernel/system | framework (return-value) |

SEGV ファイル数は最終的に **16 → 6**。

#### ★ node_ensure begin/ensure return-value rooting (dominant return-value cluster)
`begin body ensure cl end` の `node_ensure` は body の RESULT (`_br.value`、moving) を C-local で
保持したまま `EVAL_ARG(ensure_body)` を実行していた。ensure 節は任意の alloc を伴うコードを走らせる
ため STRESS で body 結果が move → 返した `_br.value` が stale → caller の prologue / proc_call
epilogue (korb_proc_snapshot_env_maybe) が collected object を deref。**mspec `it` がまさにこの形**
(ensure で after_each hook を回し、その後 block の MSpecExpectation を返す) なので return-value
cluster の支配項だった。`_br.value` を yield_self_chain に park (sp slot 不可: ensure_body が
sp[0..] を scratch に使う) して解決。system_spec 完走。

#### ★ node_apply_call splat-receiver correctness バグ (GC 以前の本物のバグ)
`recv.meth(*args)` / `send(:m, *args)` を担う node_apply_call は recv を sp[0] に park してから
args_node を評価していたが、args_node は配列ビルダで **node_ary_new が結果ハンドルを sp[0] に書く**。
→ recv が args 配列で上書きされ、**cfunc dispatch が間違った receiver で走っていた**:
`[1,2,3].first(*[2])`→[2]、`"hello".center(*[11])`→NoMethodError、そして
**combination/permutation/repeated_combination の `.to_a` (= `send(method,*args)` で replay)
が全部間違った配列を combine** していた (STRESS 無しでも再現する silent な誤動作)。
非moving の sibling koruby は recv を C-local で持っていたため無傷。precise は moving GC のため
C-local も args_node の GC で stale 化する (この二重苦が「SEGV cluster」の正体だった)。
→ recv を synthetic frame の last_match に park (frame chain は sp 無関係に必ず scan、fp/self/cref
を copy するので lvar/arg staging は不変)、dispatch 前に pop。**baked arg_index は
`fp+arg_index==sp` を仮定するので sub-eval を sp+N にずらす手は不可** (inner array の element
staging が壊れる) — この罠で 1 周回した。

#### 残 SEGV (6, 深い framework / 個別 tier)
- **[FIXED] proc/method return-value rooting**: node_ensure (system 解消) +
  repeated_combination/permutation の r==0 path (下記)。
  - **[FIXED] repeated_combination/repeated_permutation**: 真因は Fiber でも closure でもなく、
    **builtin の r==0 early-return が korb_yield (empty tuple yield = GC point) 後に stale C-local
    `self` を返していた**だけ。no-block Enumerator path で generator block (`me.send(...,0){...}`,
    bootstrap.rb:4958) の send 結果が dangling 化 → `.to_a` 実行時 proc_call epilogue で SEGV。
    `sp[-argc-1]` 再読込で解決 (main path は元から re-read 済、early-return だけ漏れていた)。
    bisect で `@array.repeated_combination(0).to_a` (harness 内) に絞り込んで特定。
  - enumerable/first は別系統 (gc_bump 中の korb_inspect → bad edge、GC-internal)。
- **[FIXED] eval-with-binding frame self** (__dir__): **真因は Binding が GC で全く scan されて
  いなかったこと** (libc-registry の T_DATA case は Method/Fiber のみ、Binding 無し) → b->self /
  捕捉 lvars / extras が move で stale。Binding case を追加 (self/extra_vars/outer_vars/heap-fp
  slots/cref klass を forward、live-stack alias は skip)。加えて binding_eval_via で caller self
  と eval 結果を yield_self_chain に park (eval が c->sp_top を b->fp に移すため sp park 不可)。
  eval-clone path の register 漏れも修正。eval("self",binding) 等が完走。
- **[残] eval_spec — eval body 内の class reopen** (`eval("class X; ...; end", b)`): eval body は
  c->current_frame->fp = b->fp+base (binding の heap snapshot, names_cnt+16 slots) で実行される
  ため、**eval body 自身の alloc が park する sp slot が value-stack 範囲外で scan されない**
  (korb_class_new の super が GC 後 stale)。eval body を scannable stack で走らせるか heap-fp を
  scan 領域化する architectural fix が要る。eval-with-binding の根の制約。
- **const_lookup の stale cref/klass** (float/rationalize / range/reverse_each): 深く調査し
  **真因 = class-identity 重複**と判明 (use-after-return ではなかった)。bisect で
  `Rational(1,3) == 1` (harness 内) に絞った。`Rational#==` dispatch が **AST method**
  (Ruby 定義 0x..5790; live Comparable#== は CFUNC 0x..a940 で別物・無関係) を引き、その
  `def_cref->klass` が **purged plane の old class** を指す。`Rational->super = 0x7ff427800000` だが
  `Object::Numeric = 0x7ff4278000a8` で **Numeric class object が 2 個併存** (reopen/moving-GC で
  duplicate 化)。Rational は old/dup Numeric を継承し、その == の def_cref->klass が GC scan で
  forward されず、const_lookup が `cr->klass->constants` で stale を deref → SEGV。
  **fix は class-reopen identity の構造修正が要り high-risk。** forward_edge は to-space idempotent
  なので visit_method_table の include_depth>0 skip 自体は安全性に不要だが、orphan の klass は
  purged plane なので単純に un-skip すると forward_payload が dead ptr を読んで GC crash する。未修正。
- **kernel/clone**: koruby_scan_edges 内 (GC 中) で別 crash (singleton/frozen edge、既知)。
- **kernel/catch UTE path**: throw が lambda を脱出した時の @__throw_tag__ ivar が stale。

### Fiber/Enumerator の stack GC scan (each_byte.to_a 等の crash 解消)
Enumerator は内部で Fiber を使う (`_start` が `Fiber.new`)。visit_roots は現在実行中の
context (`c->stack_base..sp_top` + `c->current_frame`) しか scan しないため、**fiber 実行中は
suspended な resumer の stack/frame が、resumer 実行中は suspended fiber の stack/frame が
forward されず stale 化** していた (`"hello".each_byte.to_a` 等が SEGV、frame.self が死んで
`__rescue_class_check` dispatch で crash)。`korb_scan_fiber_roots` (object.c) を追加し libc
registry の T_DATA fiber case から呼ぶ: suspended fiber は自身の saved stack+frames、実行中
fiber は suspended resumer の stack+frames を scan。frame chain は self/$_/$~ のみ forward
(cref/class の生 ptr は uninit/stale で read-only code 書込み crash、garbage prev は
alignment guard で打切り)。string/each_byte ほか Enumerator 経由 spec が CRASH→完走。
残: test_fiber は別の深い stale-recv (fiber 固有 frame 管理 bug)。

### Range→arena 移行 (string range crash の根本解消)
range も array 同様 **moving (aro_gc_alloc)** に移行した。korb_range は固定長
struct (payload 無し) なので array より簡単: korb_range_new を korb_xmalloc から
aro_gc_alloc に変え、begin/end を alloc GC 跨ぎで sp park、libc registry 登録を削除。
scan_edges の T_RANGE が begin/end を walk するので registry 不要。
- **真因だった begin 破壊**: node_range_new が begin を先評価 → end eval の GC で
  begin (C-local) が stale 化 → range の begin field に stale を格納していた。
  begin を AROH_ROOT_STACK に park して解決。
- moving 化に伴い range.c の `struct korb_range *r` C-local 保持を一掃
  (rng_to_a/min/max/step/include/each_with_index、re-read + field local capture +
  return self→sp[-argc-1] + non-numeric は synthetic frame)。
- これで range each/to_a/begin/include/first/new が STRESS+PURGE で動作。FAIL/ERR が増えたのは、crash していた file が完走して
feature-gap の assertion (未実装メソッド等) を露出するようになったため (= 前進)。

### 第2弾で効いた追加 framework fix
- **yield self-chain** (CTX.yield_self_chain): korb_yield fast path は block body 実行のため
  frame->self を block の self に書換え、後で bare C-local prev_self を復元していた。enclosing
  self は書換えた frame slot 経由でしか到達できず body の GC で stale 化 → 復元後に
  c->current_frame->self を読む全箇所 (EVAL_node_func_call の implicit-self dispatch 等) が SEGV。
  C-stack save の linked list を CTX に持ち visit_roots が walk (eval_frame_chain と同 idiom)。
  slow path は既存 AROH_ROOT_STACK で対処済だった。
- **rest-gather staging** (korb_yield_slow): `|*xs|` の rest gather が args_buf (unscanned C-stack
  snapshot) の要素を korb_ary_new_capa の GC 後に push → moving した heap 要素を stale 格納。
  alloc 前に rest 引数を value stack に stage して forward させる。hash.any?{|k,v|} や
  blk.call(*xs) 経由の Enumerable 全般 (any/all/none/group_by 等) の crash を解消。
- **splat root-stack** (node_splat_to_ary): `*expr` が splat 値 v を to_a funcall 跨ぎで stale 保持。
  この node は EVAL_ARG 経由で caller と sp 共有のため sp park 不可 (recv 破壊)、AROH_ROOT_STACK に park。

### 効いた fix (大きい順)
1. **eval_frame_chain** (最大、PASS 555→1092): `korb_eval_string` の top_frame は制御フロー隔離で
   `prev=NULL` push されるため、nested require/load 中に外側 (suspended) eval top_frame が
   `visit_roots` の head chain から切れ、その間に `main_obj` が動くと `frame->self` が stale 化。
   → `DISPATCH_node_ivar_get` / `korb_class_of_class(recv)` SEGV (mspec harness 全体で多発)。
   CTX に `eval_frame_chain` stack を追加し全 eval top_frame を walk。`korb_frame.eval_prev` で linked。
2. **dispatch recv** (`korb_dispatch_to_method` AST path): rest-array/kwh alloc の GC を跨いで
   recv stale。CTX `dispatch_recv_root` に park。
3. **EVAL_node_lshift** の Array fast-path (`a<<x`): grow 後 stale C-local l を return →
   stored-closure `acc<<x` crash の真因 (`a.push` は別 path で OK だった)。
4. **builtin stale-self / class-self handle** 多数: ary_first_n/last_n/delete_at/class_brackets/
   initialize, class_new, module_methods_by_vis, obj_dup, str_clone, str_include, str_sub_bang,
   node_const_path_get の parent module 等。型は IDIOM A (GC 後 `sp[-argc-1]` 再読込) /
   B (synthetic frame) / C (class handle sp park + re-derive) の 3 つ。

### 残課題 (campaign 後、CRASH=53)
- **string 系 builtin の self-by-value impl** (str_tr_bang_impl 等) — parking 規約が無く mechanical follow-up。
- **string range の STRESS crash** — range が xmalloc (非moving) で begin/end の moving string が
  libc-obj registry forward の timing で破壊される (`spec_port_pending.md`)。range→arena 移行が要る。
- **clone_spec** — str_clone 後も singleton/frozen edge で koruby_scan_edges 内 crash。
- test_fiber (gate の残 1)。

## 設計の要点

### 旧 WIP がなぜ失敗したか
旧 struct-only WIP は `struct korb_array { VALUE *ptr; ... }` の **struct だけ arena に置き、
要素バッファ `ptr` は libc malloc のまま** にしていた。GC が `ptr` バッファを知らないため
中の VALUE を forward せず、要素が stale/誤collect → rubyspec 7492→2922 の大規模 regression。

### payload-as-VALUE (当初デザイン)
要素バッファ自体を独立 GC object にする (ascheme_precise の OBJ_VECTOR/OBJ_VEC_BACKING と同型)。

```c
struct korb_array {           // ハンドル: 住所不変・固定サイズ
    struct RBasic basic;
    VALUE backing;            // payload object への普通の VALUE 参照
    long  len;               // 論理長 (ハンドルが持つ)
};
struct korb_ary_backing {     // payload: 独立 GC object (T_ARY_BACKING)
    struct RBasic basic;
    VALUE items[];           // 要素 VALUE[] をインライン
};
```

- **len** (論理長) はハンドル、**capa** (物理容量) は backing の `header.gc_size` から導出。
- backing は `aro_gc_alloc` (SCAN版) で確保。要素 access は `korb_ary_items(a)[i]`。
- scan_edges 二段: `T_ARRAY` は `backing` 参照1個を visit、`T_ARY_BACKING` は gc_size から
  要素数 N を逆算して `items[0..N)` を walk (reader とデータが co-located、backend 非依存)。
- push は `korb_ary_push(c, sp, ary, v)` = ary/v を sp[0]/sp[1] に park し `push_sp(c, sp+2)`。
  grow は `korb_ary_grow` が大きい backing を確保 → memcpy → handle 差し替え。

### identity が保たれる理由
ハンドルが住所不変なので `b = a; a.push(x); b.last` が CRuby 通り動く。push の realloc は
backing を作り直すだけでハンドルは不動。CRuby の RArray と同じ二段構成。

## ★ c->sp_top 規約 (厳守)
- **callsite で `c->sp_top = sp + N` を書かない。** alloc helper の中で GC trigger 直前のみ。
  push(c, sp, ary, v) は内部の push_sp(c, sp+2) が処理。callsite は staging base を渡すだけ。
- 例外: iterator builtin が yield ループ前に自分の park slot を reserve する場合のみ
  `c->sp_top = sp+K` ... ループ後 `c->sp_top = sp` で戻す (下記の通り、これも不足で
  最終的に synthetic frame に移行した)。

## STRESS rooting: synthetic frame 方式

iterator builtin (`map`/`select`/`each` 等) が結果を sp slot に park しても、**korb_yield は
block body を block 自身の (低い) sp で実行 → scan 範囲 [stack_base, c->sp_top) が parked slot
より下に縮んで collect される** (旧 WIP の vstack_frontier と同じ問題)。

解決 = **synthetic frame**: root を合成 `korb_frame` の `last_line`/`last_match` slot に park し
`c->current_frame` に push。frame chain は `visit_roots` が `sp_top` 無関係に必ず walk するので、
yield が sp_top を下げても root は生存。node_plus と同手法。`KORB_ARY_YIELD_FRAME` (array.c) /
`KORB_HASH_YIELD_FRAME` (hash.c) マクロ。**全 exit path で `c->current_frame = fr.prev` 復元必須**。

その他: class/object handle を GC point 跨ぎで保持する builtin (module attr_*, struct_class_new,
NoMethodError path の recv 等) は sp slot に park + GC point 毎に re-derive。

## 残課題 (payload migration の regression では無い、pre-existing を GC 圧で炙り出したもの)

- **test_fiber**: fiber は stack 切替 (c->stack_base/sp_top swap) するので synthetic frame の
  扱いが別レイヤ。fiber 特有 edge。
- **test_alias_redef**: 未定義 method の NoMethodError path で recv (T_OBJECT = clean HEAD でも
  moving) が dispatch 入口で stale。rescue 経路特有。
- **test_hash**: compare_by_identity / array_key_identity の 3 assert (crash でない)。moving GC で
  object address が変わると identity hash が壊れる既知の semantic 問題。

## 変更ファイル
core: object.c / object.h / context.h / koruby_runtime.c / node.def。
builtins: array / hash / comparable / module / integer / kernel / object / string / range /
float / file / proc / binding / exception / math + builtins.c + main.c。
