# ascheme_precise: sframe → sp[] frame 大改修 migration plan

`struct sframe` (OBJ_FRAME) を完全廃止し、 closure call frame の locals
を `c->sp` 上の slot として持つ大改修の **段階的 plan**。 reference
design は `sample/baruby_precise/` の iter 50-72 (= 同種の refactor を
完了済) で、 ascheme 固有機能 (= call/cc / multi-values / delay/force
/ frame 再利用 tail-call) を整合させた 10 phase 構成。

## 0. 動機

`docs/perf.md` §4.3 (= AOT mode head-to-head) で観察された fib35 の
**libgc 0.25s vs precise (= copy) 0.61s = 2.44× 遅い** structural gap:

| bench | libgc AOT | copy AOT | copy / libgc |
|---|---:|---:|---:|
| fib35     | 0.25 | 0.61 | **2.44×** |
| sumloop   | 0.29 | 0.48 | 1.66× |
| cps_loop  | 0.19 | 0.25 | 1.32× |

dispatch-heavy CPU bench (= fib35 / sumloop / cps_loop) で **libgc が
precise の 1.3–2.4× 速い**。 GC-bound (deriv / fannkuch / matmul) は
precise が勝つので「allocator が速い」 ではなく「frame 取扱いコスト」
が dominate している。

### 主因 (= profile 結果 + source 読解)

`scm_apply` の closure dispatch (`main.c:2515-2591`) で **毎 call ごとに
sframe を `aro_gc_alloc_raw` で heap alloc**:

- `main.c:454 scm_new_frame` — `sizeof(struct sframe) + nslots * VALUE`
  を毎回 GC heap から取る。 sp parking + WB + type tag 付け + slot
  zero-init で ~30-40 命令
- `main.c:2444 build_frame_for` — has_rest 処理 + scm_new_frame を呼ぶ
  generic path
- libgc 側 (`sample/ascheme/main.c` の同等箇所) は **leaf closure で
  `alloca` で C stack に sframe を作る** (`main.c:2530-2548` の
  `#if BARUBY_GC == BARUBY_GC_NONE` arm)。 alloc コスト ≈ rsp 加算 1 命令

precise rooting + moving GC で alloca が使えない (= visitor は c->env
chain を辿るので C stack 範囲を「sframe を含む heap」 として treat
してしまい SEGV、 moving GC が forward しても C stack 上の pointer は
書き換わらない) ため、 leaf 最適化が disabled。 **構造的に frame heap
alloc が抜けない**のが現状。

### 解 (= baruby_precise の path)

baruby_precise は iter 50-72 で同じ問題に直面し、 以下の path で解決:

- sframe (= 木構造の env chain) を廃止
- locals は **共有 VALUE stack (c->env..c->sp の linear 範囲)** の slot
  として置く
- alloc は **`c->sp += locals_cnt` だけ** (= 仮想 8 GiB 予約 mmap、
  lazy paging)
- lget / lset は parse-time に `sp_offset` を bake → runtime は
  `sp[sp_offset]` 1 命令
- GC root scan は `c->env..c->sp` の **flat scan** で全 frame slot を
  網羅 (= sframe chain walk 不要)

ascheme は **lexical closure** を完全 first-class で持つので baruby (=
proc 内 lvar set/get は単純な closure) より複雑。 §2 で gap 解消方針を
列挙。

## 1. 設計方針

### 1.1 frame 表現

```
旧 (= 現状):
  c->env --[parent]--> outer --[parent]--> ... --> NULL
        sframe{slots[N], parent}        各 sframe = OBJ_FRAME heap obj

新:
  c->env  : VALUE *    指す位置は frame の locals[0]
  c->sp   : VALUE *    現在の scratch 上限 (= 次 frame の予定位置)

  共有 VALUE stack (8 GiB virtual mmap):
    +-- c->env --+--- toplevel locals ---+--- f1 locals ---+--- f2 ...
    |             |                       |                 |
    fp(toplevel)  fp(f1)                  fp(f2)            sp (現在)

         ←----- precise GC が flat scan する範囲 ----→
```

- **静的に深さがわかる lexical access** (= compile 時に
  `(depth, idx)` が固定) は **「parent frame の base 」 を実行時に
  保持** する必要がある。 → §1.2 の display 方式
- frame は **「sp 上の連続 N slot」**。 alloc cost = `c->sp += N`

### 1.2 closure の env capture: display 方式 (= 推奨)

baruby は単純 (lexical scope なし、 proc は global function だけ) なので
sp[] 一本で済む。 ascheme は **lexical closure** が必要 — lambda が
外側 frame の slot を捕獲する。 sp[] base address を closure に持たせる
と、 caller の sp が後で巻き戻った瞬間 stale。

**display 方式**:

- closure object に **「捕獲した frame base のスナップショット (= depth
  数だけの VALUE * 配列)」** を持たせる
- env は **stack 上の slot 列** だが、 closure が**生きている間だけ
  該当 region を「 captured 」 と mark** して GC 視点で hold する
- 「stack 上の slot を closure 経由で hold」 = 該当 frame の slot 列を
  **stack から hoist して heap に**コピー (= **upvalue boxing**) する
  実装が SML / Lua / Scheme 系で標準。 ASTro でも採用

### 1.3 closure capture: 2 つの実装オプション

**オプション A: 常に display heap-box (= "captured frame")**
- lambda 評価時に capture 対象 frame の slot[] を **`captured_frame`
  という heap object** (= OBJ_VEC_BACKING に近い) にコピー
- closure object は (body, captured_frame *parent_display[]) を持つ
- lget(depth, idx): depth=0 → `sp[idx - locals - chain]`、 depth>=1
  → `closure->display[depth-1]->slots[idx]`
- lset on captured slot: heap box に書く + write barrier
- pro: 設計が clean、 captured 後の caller stack 巻戻に強い
- con: lambda 評価で 1 alloc + slot copy。 baruby の sp[] 完全 alloc 0
  には届かない

**オプション B: escape analysis + lazy box (= 最適化)**
- compile 時に「この frame が closure に捕獲されるか」 を判定
- **non-captured frame** (= leaf lambda body から見える純粋 local) は
  sp[] 上に置いたまま (alloc 0)
- **captured frame** だけ heap box 化 (= A と同じ)
- ascheme の `closure.leaf` フラグが既に escape analysis 結果なので
  流用可能 (= `main.c:177 closure.leaf`)
- pro: leaf-only (= fib35 / sumloop 等) は完全 alloc 0
- con: parser 拡張 (= 「この local は捕獲される」 のフラグ)、 lset の
  boxed / unboxed 分岐 (= node_lset_box / node_lset_sp で別 NODE)

**推奨**: Phase 3 で B のうち leaf 限定 (= 「全ての lambda が leaf」 だけ
sp[] 化、 nested lambda を含む scope は A に fallback)。 Phase 7 で B
の完全形 (= per-slot escape analysis) に拡張。

### 1.4 parse-time sp_offset bake (= baruby iter 72 pattern)

baruby iter 72 (`baruby_parse.c:38-63`) と同じ方式:

- transduce_context に `chain_sum` (int32_t) を持ち回り
- `WITH_CHILD_CHAIN(kind, BODY)` macro で BODY 評価中だけ
  `chain += parent.slot_count` (= GCC statement-expression)
- `bake_lref_sp(tc, depth, index)` 等のヘルパが ALLOC 時に
  `partial = index - tc->chain_sum` を operand に焼く
- frame の `bake_list` に append → pop_frame で `*offset -= locals_cnt`

ascheme で必要な追加:

- **depth > 0 の lref** は別 path (= captured display 経由)。 sp[] bake
  対象は **depth == 0 のみ**
- depth は parser (= `lex_lookup`) が静的に出すので分岐は静的

### 1.5 root visitor 簡略化

旧:
```c
// main.c:4693
if (c->env)      ARO_GC_VISIT_EDGE_PTR(gc, edge_visit, (void **)&c->env);
if (c->next_env) ARO_GC_VISIT_EDGE_PTR(gc, edge_visit, (void **)&c->next_env);
// + sframe chain は OBJ_FRAME の SCAN_EDGES で自動辿る
```

新:
```c
// baruby_precise context.h:298 AROH_VISIT_ROOTS と同じ pattern
for (VALUE *p = c->env; p < c->sp; p++) {
    ARO_GC_VISIT_EDGE((ctx), edge_visit, p);   // 各 slot を VALUE として visit
}
// + display heap box は通常 heap obj として SCAN_EDGES で辿る
```

linear scan で **dispatch cost ~50ns/scan** が大幅低下 (= baruby iter
61 で GC pause 時間 -40% を観測)。

## 2. ascheme 固有機能の gap 分析

baruby_precise にあって ascheme_precise に対応物が無い / 違うものを
列挙し、 sp[] 化後の解法を提示。

### 2.1 call/cc (= one-shot escape continuation)

現状: `main.c:2689 scm_callcc`、 `struct scont` (= context.h:131) に
`jmp_buf + saved_env + k_val + fn_val + saved_tcp + active + tag`。
`saved_env = c->env` (sframe *) を保持し、 longjmp 時に
`CTX_SET_ENV(c, cnt->saved_env)` で復元。

sp[] 化後の問題: **sp も saved する必要**。 環境は sp の値 + sframe で
は無く、 sp 自体 + sp 上の slot で表現されるから。

解法: scont に追加フィールド:
```c
struct scont {
    AroObjectHeader head;
    jmp_buf buf;
    VALUE   result;

    /* iter NEW: sp-frame migration */
    VALUE  *saved_sp;          /* c->sp 復元用 */
    VALUE  *saved_env;         /* c->env 復元用 (= toplevel base、 typically c->env そのもの) */
    /* + 該当時点での captured display chain (= closure 経由 hold) */
    struct sobj *saved_display; /* OPTIONAL: A 方式なら不要、 B 方式で要検討 */

    VALUE   k_val, fn_val;
    int     saved_tcp, active, tag;
};
```

ただし **「sp を巻戻したら巻戻し範囲の値が消える」**問題:
longjmp で sp を巻戻すと、 巻戻し範囲にあった heap obj が他からも root
されていなければ次 GC で死ぬ。 R5RS の one-shot 限定なら問題なし
(= scont が active な間だけ root 範囲を保持する設計)。

**未解決 (= open question 1)**: full continuation (= multi-shot)
サポートする時、 巻戻し範囲全体を heap copy する必要 (= "stack chunk"
コピー)。 Phase 5 で one-shot のみ実装、 multi-shot は Phase 後の
別 iter で。

### 2.2 multi-values (= `(values a b c)`)

現状: `OBJ_MVALUES` = scm_make_mvalues で box (`main.c:343`)、
`call-with-values` で unbox (`main.c:4194 prim_call_with_values_p`)。

sp[] 化後: **無変更で良い**。 MVALUES は heap object として独立、 sp
frame と独立に動く。 `call-with-values` 内の scm_apply 呼出は新 frame を
sp 上に作って unbox 値を bind するだけ。

ただし `prim_call_with_values_p` の sp 利用 (`main.c:4196-4213`):
producer 結果 → sp 退避 → consumer apply、 を新 sp 規約 (= argv は
sp 上の連続 slot) に書き換える必要あり。

### 2.3 delay / force (= promise)

現状: `OBJ_PROMISE` (`main.c:4017 prim_delay`, `4028 prim_force_p`)。
thunk + value + forced flag を持つ heap obj。 force は thunk を
scm_apply で呼んで memoize。

sp[] 化後: **無変更**。 promise 自体は heap obj、 force は scm_apply
呼出で sp 上に新 frame を作るだけ。

### 2.4 closure 内 env capture pattern

baruby は OO 無し / proc 無しなので「closure capture」 が存在しない。
ascheme は **全 lambda が完全 lexical closure**。 §1.3 で 2 オプション
提示。 推奨は **Phase 3 で leaf-only sp[] 化、 Phase 7 で escape
analysis + lazy box**。

ascheme 既存の `closure.leaf` フラグ (= `main.c:177`、 「body has no
inner lambda → 自分の frame は escape しない」) は **真の escape
analysis ではなく conservative な近似** (= leaf でも closure に
渡された frame が caller 経由で escape する可能性は残る)。 sp[] 化
する時には:
- `leaf == true` → sp[] 上の frame で OK (= caller の scope 終了で
  消えてよい)
- `leaf == false` → display heap box (= A 方式) 強制

### 2.5 tail-call frame 再利用

現状: `main.c:2611-2659 scm_apply_tail_slow` の self-tail-call path。
「同じ shape の closure に tail call する時、 c->env をそのまま再利用
してslots だけ上書き」 で alloc 0。

sp[] 化後: **fundamentally easier**。 sp 上の frame をそのまま使う、 sp
は動かさない。 baruby iter 71 の `node_call_N` (`node.def:218-267`) と
同じ pattern: caller の sp[0..N-1] に new args、 callee body を
`sp + locals_cnt` で再 dispatch するだけ。

ascheme 固有の `node_loop` + `node_self_tail_call_K` (= `node.def:336-
424`) も sp[] 化と整合する。 既に loop_args[] → c->env->slots に copy
する path が「frame slot を上書き」 と等価なので、 sp[idx] への直書きに
書換えるだけ。 ASCHEME_LOOP_MAX_PARAMS=8 制限は維持。

### 2.6 dynamic-wind

未実装 (= `grep -n dynamic-wind sample/ascheme_precise/main.c` 該当無し)。
Phase 後追加なら sp 復元タイミングで before/after thunk を呼ぶ仕組みを
入れる (= scont と同等の薄い frame 抜けトリガー)。

### 2.7 chibi R5RS test (= 179/179 PASS 維持)

各 phase で `make test` (= 17 test/*.scm) + chibi R5RS adapt
(`test/r5rs_chibi.scm`、 既存 PASS 数 = 179) を回す。 1 phase でも
regression したら revert / 設計見直し。

## 3. アーキテクチャ図

旧 (= 現状、 sframe chain):

```
                       heap
   c->env  ─────►  sframe{parent, nslots, slots[N]}  ← OBJ_FRAME
                        │
                        ▼
                   sframe{parent, nslots, slots[M]}
                        │
                        ▼
                       NULL

   closure ────► sobj{closure.env = sframe*}
```

新 (= sp[] frame + display heap box):

```
    c->env  ───────────► (= virtual stack base、 toplevel locals 起点)
    c->sp   ─────────────► (= scratch top)
    │
    ▼
   [ toplevel locals  | f1 locals    | f2 locals      ...]
   ▲                  ▲              ▲                   ▲
   env                fp(f1)         fp(f2)              sp

   captured-display:                heap
   closure ────► sobj{closure.display[0..depth-1] = captured_frame*}
                              │
                              ▼
                     captured_frame{slots[N]}  ← OBJ_VEC_BACKING tagged
```

leaf lambda (= closure.leaf=true):
- 自分の frame は sp[] 上、 alloc 0
- captured display は parent frame の captured_frame (= 上位 lambda 時に
  既に確保済) を pointer copy
- exit 時に sp 巻戻すだけ

non-leaf lambda (= closure.leaf=false):
- 自分の frame も sp[] 上に作るが、 **lambda alloc 時に slots を heap
  に hoist (= captured_frame として保存)**
- 内側の closure capture が起きる時、 そっちは captured_frame を見る
- sp 巻戻しても heap 側は GC root で生き残る

## 4. 各 phase の詳細

phase は **side-by-side で進める** (= 旧 sframe path と新 sp[] path を
共存させ、 build-time フラグ `ASCHEME_SP_FRAME=1` で切替可能)。 全
phase 通過後に旧 path を Phase 7 で削除。

### Phase 1: parser に sp_offset bake (= lref/lset 用、 旧 path 並存)

**変更**:
- `parse.c` (= ascheme_precise の parse.c は reader 専用; compile は
  main.c) — main.c の `compile()` 周辺
- main.c に baruby と同等の `transduce_context` 相当 (= compile-time
  state) を追加。 既存の `lex_scope` chain に `chain_sum` フィールド
  を抱かせる
- `bake_lref_sp(scope, depth, index)` ヘルパ (= depth==0 のみ sp_offset
  焼く、 depth>0 は旧 lref のまま)
- `lex_scope` に `frame_high_water` (= 各 frame の最大 sp 利用量) を
  追加、 lambda 終端で `bake_list` を flush (= baruby `pop_frame` と
  同じ)

**規模**: main.c +200 行、 node.def の lref/lset に `sp_offset`
operand 追加 (+ 旧 depth/idx は維持)。

**risk**: lex_scope chain と chain_sum の整合 — `let` / `letrec` /
`lambda` の各 push_scope で chain reset するか継続するかの判定。
**verify**: 既存 test 17 + R5RS = PASS。 新 NODE は dispatch されない
(= 旧 lref 経由で動く)。

**工数**: ~6h、 +200 行。

### Phase 2: 新 NODE_DEF を併設

**変更**:
- `node.def`: `node_lref_sp(c, n, sp, sp_offset)` / `node_lset_sp(c, n,
  sp, sp_offset, val)` 追加。 baruby `node_lget` / `node_lset` と完全
  同形 (= sp[sp_offset] 直読)
- compile() の lref 生成箇所: `if (depth == 0) ALLOC_node_lref_sp(...)
  else ALLOC_node_lref(depth, idx)` で分岐
- build-time フラグ `ASCHEME_SP_FRAME=1` で `lref_sp` を有効に。
  default は `0` (= 旧 lref のみ使う)

**規模**: node.def +30 行、 compile() 内 +50 行、 ASTroGen 側に変更
無し (= 既存の `int32_t sp_offset` operand pattern を使う)。

**risk**: depth==0 path しか変えていないので機能 regression は最小。
ただし frame_high_water 計算ミスると `sp_offset` がズレて読み外す。
**verify**: `ASCHEME_SP_FRAME=1` で 17 test + R5RS PASS。 stress mode
(`BARUBY_GC_STRESS=1` 相当) で moving GC との整合確認。

**工数**: ~4h、 +80 行。

### Phase 3: closure call path を sp[] frame 化 (= leaf 限定)

**変更**:
- `main.c scm_apply` (= 現状 `2494-2593`) の closure dispatch を分岐:
  - `closure.leaf && depth==0 lref のみの body` → 新 sp[] frame path
    (= sp に locals 確保、 args を sp[0..N-1] に書く、 body を
    `sp + locals_cnt` で dispatch)
  - それ以外 → 旧 build_frame_for + scm_new_frame
- `node_lambda` (= `node.def:200`): leaf flag に加え「全 lvar が
  depth==0」 (= captured 無し) も判定して `body_is_sp_safe` flag を
  closure に持たせる
- new helper `scm_apply_sp(c, fn, argc, argv, sp)` (= sp[] path
  専用)。 sframe を作らず c->env / c->sp を進めるだけ
- AROH_VISIT_ROOTS に **`c->env..c->sp` の flat scan を追加** (= 既存の
  sframe chain scan と並存。 重複は singleton filter で害なし)

**規模**: main.c +300 行 (= 新 apply path + visitor 拡張)、 node.def
+50 行 (= closure flag 拡張)、 node.h の scm_apply_tail に sp[] arm
追加 +60 行。

**risk**: **closure capture 漏れ** が最大リスク。 leaf 判定が conservative
で正しいか (= 動的に escape する frame を sp[] に置かない) を test で
検証必須。 stress mode で moving GC 全 alloc に挟む。
**verify**: 17 test + R5RS PASS、 特に `06_higher_order.scm` (= 高階)、
`09_tco.scm` (= tail-call)、 `16_alloc_root_stress.scm` (= moving GC
stress) で PASS。

**工数**: ~16h、 +400 行。 一番 risky な phase。

### Phase 4: tail-call frame 再利用を sp[] で実装

**変更**:
- `node_self_tail_call_K` (= `node.def:351-424`): `c->loop_args[i] →
  c->env->slots[i]` を `c->loop_args[i] → sp[i - locals_cnt]` に書換え
  (= sp_offset bake 経由)
- `node_loop` (= `node.def:336`): loop_args → frame slot copy を sp[]
  上の slot copy に
- `scm_apply_tail` (= node.h:82) の self-tail-call frame reuse arm を
  sp[] frame 用に: c->env (= sp base) を維持したまま sp[0..N-1] に
  new args 書込み
- ASCHEME_LOOP_MAX_PARAMS=8 制限維持 (= API 変更なし)

**規模**: node.def +80 行 (= sp[] 版 self_tail_call_K 並存)、 node.h
の inline path +40 行。

**risk**: tight tail loop の perf が落ちないか (= 旧 frame reuse path
は既に alloc 0 だった)。 sp[] 化で更に減ることを bench で確認。
**verify**: `09_tco.scm` + nqueens / fannkuch perf bench、 plain mode
で +0% 以上 (= regress 無し) を milestone。

**工数**: ~6h、 +120 行。

### Phase 5: call/cc を sp slice の保存・復元で書き換え

**変更**:
- `struct scont` (= `context.h:131`) に `saved_sp` (= VALUE *) 追加。
  `saved_env` の意味を「sframe *」 から「VALUE * (= sp base 復元用)」
  に転換 (= 旧 path との並存中は両方 持つ)
- `scm_callcc` (= main.c:2689): `cnt->saved_sp = c->sp` を保存。
  longjmp 後に `c->sp = cnt->saved_sp` で復元
- one-shot 限定 (= 既存 ascheme と同じ)。 multi-shot は未対応のまま
  (= 別 iter)
- `scm_apply (continuation arm)` (= main.c:2506): VALUE *argv を
  longjmp 直前に scont 内に copy (= sp 巻戻し範囲外に退避)

**規模**: context.h +20 行、 main.c +80 行。

**risk**: longjmp 後の sp 復元タイミングで「もう死んでる slot を生かす」
shape mismatch があると debug 困難。 saved_sp range の値を heap copy
する必要があるかは test しながら判断 (= 17 test の `07_callcc.scm` で
回す)。
**verify**: `07_callcc.scm` + R5RS の call/cc test 群 PASS。

**工数**: ~8h、 +100 行。 fragile な path。

### Phase 6: multi-values を sp slot に置く形へ

**変更**:
- `prim_call_with_values_p` (= main.c:4194) の sp 利用を sp[]-frame
  規約に合わせて書直し
- `scm_make_mvalues` (= main.c:343) の sp parking pattern は維持 (=
  既に sp[] root)、 caller 側の sp 規約を新規約に
- `OBJ_MVALUES` の SCAN_EDGES 変更なし

**規模**: main.c +30 行。

**risk**: low (= MVALUES は既に heap obj として独立)。
**verify**: `02_control.scm` (= values / call-with-values 含む) PASS。

**工数**: ~3h、 +30 行。

### Phase 7: sframe / OBJ_FRAME / c->env (sframe *) を段階的に削除

**変更**:
- 全 lref/lset を sp_offset 経由に統一 (= depth>=1 も capture display
  経由に)
- `struct sframe` 定義削除 (= context.h:200)
- `OBJ_FRAME` enum 削除 (= context.h:106)
- `scm_new_frame` 削除 (= main.c:454)
- `build_frame_for` 削除 (= main.c:2444)
- `AROH_SCAN_EDGES` の OBJ_FRAME arm 削除
- `aro_scheme_visit_roots` から sframe chain scan 削除
- `CTX.env` を `VALUE *` (= sp base) のみに統一
- `CTX.next_env` を `VALUE *` に
- `ASCHEME_LREF_CACHE_SIZE` 関連 (= env_chain[] 等) も廃止 (= sp_offset
  で常に O(1))

**規模**: 大規模 cleanup。 main.c -800 行、 context.h -200 行、
node.def -100 行。 net negative diff (= -1100 行)。

**risk**: ここで 1 つ漏れがあると全 break。 既に Phase 3-6 で sp[] path
が dominant pass しているはずなので、 残された旧 path の delete は
mechanical。
**verify**: 17 test + R5RS PASS、 plain + AOT 両方。

**工数**: ~10h、 -1100 行。

### Phase 8: SCAN_EDGES + root visitor の更新

**変更**:
- `context.h AROH_SCAN_EDGES`: OBJ_FRAME arm 完全削除 (= Phase 7 と
  同期)、 OBJ_CLOSURE arm を新 closure.display 構造に対応
- `aro_scheme_visit_roots` (= main.c:4690): sframe chain scan 削除 +
  flat sp[] scan を主に
- closure.display は通常 heap pointer なので OBJ_CLOSURE の SCAN_EDGES
  内で `for (i=0; i<depth; i++) visit(closure.display[i])` で十分

**規模**: context.h +40 行 (= 新 OBJ_CLOSURE display 対応)、 main.c
-50 行 (= sframe chain scan 削除分)。

**risk**: GC 視点で「captured_frame は誰が hold するか」 を明確に。
closure が hold、 closure が死ねば captured も死ぬ。
**verify**: 16_alloc_root_stress.scm + stress mode、 16 backend 全部で
PASS。

**工数**: ~5h、 +/-90 行。

### Phase 9: AOT code_store の SD signature 確認 + cache invalidation

**変更**:
- `node.def` の operand 変更で HASH が変わる → 既存 SD は invalid に
  なる → `--clear-cs` で code_store/ を一旦消す手順を README に追加
- `code_store/*` の dlopen キャッシュは Phase 9 で完全 rebuild
- SD signature (= `EVAL_node_lref_sp(c, n, sp, sp_offset)` 等) が
  正しく ASTroGen で emit されているか objdump で確認 (=
  `feedback_aot_no_speedup_diagnosis` の手順)

**規模**: README 更新 +20 行、 code_store/ rebuild script はそのまま。

**risk**: `feedback_ccache_disable` の通り、 `CCACHE_DISABLE=1` を
必ず付ける手順。
**verify**: `--aot-compile --run` で AOT が plain より速くなる (=
旧 perf.md の 1.85× 加速比を超えるか同等)。

**工数**: ~3h、 ドキュメント中心。

### Phase 10: bench + perf.md 更新

**変更**:
- `bench/aot_matrix.sh` で 9 bench × 15 backend × {plain, AOT} を再測
- `docs/perf.md` 全面更新: 新 sp[] 化前後の比較を §1.4 / §4.3 に追記
- 目標: fib35 AOT で libgc 0.25s 並み (= copy AOT 0.30s 以下)、 plain
  でも geomean -20〜-30% 程度

**規模**: docs/perf.md 全面書き直し +500 行、 bench data 再採取
1 日。

**risk**: 期待 perf に届かなかった場合、 §7 の見送り判断に沿って
revert / 再設計。
**verify**: §6 の数値目標と比較。

**工数**: ~6h、 +500 行 docs + bench data。

## 5. 各 phase の risk + verify 表

| Phase | risk level | verify | revert cost |
|---|---|---|---|
| 1 | low | 17 test + R5RS PASS、 旧 path 経由なので機能 regression なし | +200 行 revert |
| 2 | low | `ASCHEME_SP_FRAME=1` で 17 test + R5RS PASS | +80 行 revert |
| 3 | **high** | 17 test + R5RS、 特に 06/09/16 + stress mode | +400 行 revert |
| 4 | medium | 09_tco + perf bench (= regress なし確認) | +120 行 revert |
| 5 | **high** | 07_callcc + R5RS call/cc 群 | +100 行 revert |
| 6 | low | 02_control PASS | +30 行 revert |
| 7 | medium | 全 test PASS、 mechanical だが scope 大 | hard revert (= -1100 行を戻すのは git 経由) |
| 8 | medium | 16_alloc_root_stress + 16 backend stress | +/-90 行 revert |
| 9 | low | AOT speedup 維持 | docs revert |
| 10 | n/a | bench 数値が §6 目標達成 | n/a |

各 phase 完了後に **必ず git commit を切る** (= revert を容易に)。
特に Phase 3, 5, 7 は前後で AOT cache を `--clear-cs` で消す。

## 6. migration 後の想定 perf

target 値 (= libgc AOT との head-to-head で gap 解消):

| bench | 現状 copy AOT | 目標 | 改善幅 |
|---|---:|---:|---:|
| fib35     | 0.61 | **0.30** | -51% |
| sumloop   | 0.48 | **0.32** | -33% |
| cps_loop  | 0.25 | **0.20** | -20% |
| nbody     | 0.36 | 0.34 | -6% (= 既に勝ってる) |
| sieve_big | 0.48 | 0.40 | -17% |
| deriv     | 0.91 | 0.80 | -12% |
| nqueens   | 1.06 | 0.80 | -25% |
| fannkuch  | 0.81 | 0.70 | -14% |
| matmul    | 4.73 | 4.50 | -5% (= 既に勝ってる) |
| **geomean** | **0.71** | **0.50** | **-30%** |

plain mode も同程度 (= geomean 1.32 → 0.95 程度) の改善を見込む。

最重要 KPI: **fib35 AOT で libgc 比 1.2× 以内** (= 現状 2.44×)。 これが
達成できなければ Phase X で見送り判断 (§7)。

## 7. 見送り判断のクライテリア

各 phase で以下のいずれかが起きたら revert + 設計見直し:

- 17 test または R5RS test で regression を再現的に観測 → 該当 phase
  内で fix できなければ revert
- Phase 3 完了時点で fib35 AOT が現状から -10% 以下 → 設計根本見直し
  (= display 方式が overhead 大すぎる可能性、 escape analysis 強化が
  必要)
- Phase 5 完了時点で 07_callcc.scm が PASS しない → call/cc 戦略
  (multi-shot 想定したか one-shot 限定か) の見直し
- 16 backend のいずれかが stress mode で SEGV 多発 → root visitor の
  漏れを debug、 fix できなければ該当 backend の skip (= matrix から
  外す)

最終的に **Phase 10 で geomean -10% 未満** なら全体 revert を検討 (=
ROI 不足)。

## 8. 工数見積もり

| Phase | hours | LOC delta |
|---|---:|---:|
| Phase 1 (parser sp_offset bake) | 6h | +200 |
| Phase 2 (新 NODE_DEF 併設) | 4h | +80 |
| Phase 3 (closure sp[] 化、 leaf 限定) | **16h** | +400 |
| Phase 4 (tail-call sp[] frame reuse) | 6h | +120 |
| Phase 5 (call/cc sp 保存復元) | 8h | +100 |
| Phase 6 (multi-values 整合) | 3h | +30 |
| Phase 7 (sframe / OBJ_FRAME 削除) | 10h | -1100 |
| Phase 8 (SCAN_EDGES / visitor 更新) | 5h | +/-90 |
| Phase 9 (AOT cache 整合) | 3h | +20 (docs) |
| Phase 10 (bench + perf.md) | 6h | +500 (docs) |
| **合計** | **~67h (= ~8.5 work days)** | **+/-500 net** |

実際は debug + perf 計測の iter で **1.5-2 weeks** を見込む。

## 9. 未解決の open question

実装着手前に判断すべき / 着手中に決める point:

### Q1: closure capture 方式 (= display A vs lazy box B)

§1.3。 推奨は Phase 3 で leaf-only A、 Phase 7 で B 拡張。 ただし
Phase 3 完了時点で perf が頭打ちなら B を Phase 4 で前倒し。

### Q2: call/cc multi-shot サポート

§2.1。 現状 ascheme は one-shot 限定 (`scont.active` flag)。 sp[] 化で
multi-shot 対応するなら stack chunk copy 機構が必要。 **R5RS は
multi-shot を要求しない** (= chibi の標準実装も one-shot)。 ascheme で
multi-shot を諦める判断を user に確認。

### Q3: chain_sum bump site の網羅

§1.4。 baruby iter 72 で 30 種類以上の binop / call / array_lit を
列挙して `WITH_CHILD_CHAIN` を貼る必要があった。 ascheme は AST kind が
baruby より少ない (= 35 種類程度) ので網羅は容易だが、 漏れがあると
silent な offset ズレ → SEGV になるので、 **NODE kind ごとの
`@child / @child-parent` 区別を全 NODE_DEF で audit する pass** を
Phase 1 で実施。

### Q4: env_chain[] cache 廃止の影響

context.h:284-323 で実装されている lazy parent-chain cache (= depth>=1
lref の高速化)。 sp_offset 化後は depth==0 が圧倒的多数なので cache は
廃止予定。 ただし R5RS test の一部 (= 深い lexical nest) で cache
依存があるかを Phase 7 で確認。

### Q5: AOT specialize の引数 type 整合

`@ref` (= struct gref_cache *) operand は AOT bake 時に literal address
として焼かれる。 新 `node_lref_sp` の sp_offset (= int32_t) も literal
で問題ないが、 既存の depth/idx operand との HASH 衝突に注意 (=
`feedback_aot_no_speedup_diagnosis`)。

### Q6: 16 backend × sp[] flat scan の per-backend tuning

baruby 経験では `copy` / `copy_gen` / `immix_gen` は flat scan に強い
(= linear traversal 得意) が、 `mark_card_gen` 等 card-marking 系は
sp[] root scan のために dirty card を追加 mark する必要がある。 各
backend の AROH_VISIT_ROOTS 呼出順序 (= card scan 前 / 後) に注意。

### Q7: `lex_scope` chain と `chain_sum` の整合

§1.4。 ascheme の lex_scope は libc-malloc + compile 中だけ生きる
(= context.h::lex_scope は GC heap ではない)。 chain_sum / max_cnt の
state を lex_scope に紐付ける形 (= baruby `frame_context` と同じ
pattern) で OK か確認。

## 10. 参考実装の file:line ref

### baruby_precise reference

- `sample/baruby_precise/baruby_parse.c:13-33` — WITH_CHILD_CHAIN macro
- `sample/baruby_precise/baruby_parse.c:38-63` — sp_offset bake 設計 comment
- `sample/baruby_precise/baruby_parse.c:390-530` — frame_context +
  push_frame + pop_frame + bake_X helpers
- `sample/baruby_precise/baruby_parse.c:435-460` — bake_list_finalize
- `sample/baruby_precise/node.def:33-52` — node_lget / node_lset
  (sp_offset operand)
- `sample/baruby_precise/node.def:218-267` — node_call_0..3 (sp[]
  frame の callee dispatch)
- `sample/baruby_precise/context.h:251-263` — CTX struct (env, sp 並存)
- `sample/baruby_precise/context.h:297-313` — AROH_VISIT_ROOTS
  (linear sp[] scan)
- `sample/baruby_precise/main.c:198-227` — create_context (mmap 8 GiB
  virtual)
- `sample/baruby_precise/main.c:371-376` — toplevel locals_cnt 反映
- `sample/baruby_precise/docs/done.md:305-381` — iter 61 (= fp 引数完全
  削除) の経緯
- `sample/baruby_precise/docs/done.md:51-110` — iter 72 (= walker 削除、
  parse-time bake) の経緯
- `sample/baruby_precise/docs/runtime.md:155-235` — sp_offset bake の
  詳細解説

### ascheme_precise 現状の関連箇所 (= 大改修対象)

- `sample/ascheme_precise/context.h:200-205` — struct sframe 定義
- `sample/ascheme_precise/context.h:106` — OBJ_FRAME enum
- `sample/ascheme_precise/context.h:131-153` — struct scont (= call/cc)
- `sample/ascheme_precise/context.h:291-345` — CTX struct (= env /
  next_env / loop_args 等)
- `sample/ascheme_precise/main.c:343-372` — scm_make_mvalues
- `sample/ascheme_precise/main.c:453-474` — scm_new_frame (= heap alloc
  hot point)
- `sample/ascheme_precise/main.c:2444-2491` — build_frame_for
- `sample/ascheme_precise/main.c:2494-2593` — scm_apply (= closure
  dispatch、 leaf alloca arm は `BARUBY_GC == NONE` で disabled)
- `sample/ascheme_precise/main.c:2599-2675` — scm_apply_tail_slow (=
  self-tail-call frame reuse)
- `sample/ascheme_precise/main.c:2689-2743` — scm_callcc
- `sample/ascheme_precise/main.c:4194-4214` — prim_call_with_values_p
- `sample/ascheme_precise/main.c:4017-4046` — prim_delay / prim_force_p
- `sample/ascheme_precise/main.c:4690-4783` — aro_scheme_visit_roots
- `sample/ascheme_precise/node.def:100-141` — node_lref / node_lset
  (depth ベース)
- `sample/ascheme_precise/node.def:199-209` — node_lambda (leaf flag)
- `sample/ascheme_precise/node.def:220-303` — node_call_0..N
- `sample/ascheme_precise/node.def:307-316` — node_callcc
- `sample/ascheme_precise/node.def:336-424` — node_loop +
  node_self_tail_call_K
- `sample/ascheme_precise/node.h:82-152` — inline scm_apply_tail
  (= leaf alloca path)
- `sample/ascheme_precise/parse.c` — reader (= 影響少)
- `sample/ascheme_precise/main.c:904-943` — lex_scope + lex_lookup
- `sample/ascheme_precise/main.c:1527-1593` — compile_lambda
- `sample/ascheme_precise/main.c:1597-1718` — compile_let (= named-let
  → self_tail_call の desugar)
- `sample/ascheme_precise/test/` — 17 .scm test
- `sample/ascheme_precise/test/r5rs_chibi.scm` — chibi R5RS test
  (= 現状 179/179)

### perf gap source

- `sample/ascheme_precise/docs/perf.md:337-362` — §4.3 AOT mode の
  libgc vs copy head-to-head (= fib35 2.44× の出所)
- `sample/ascheme_precise/docs/perf.md:104` — plain mode の同種 gap

## 11. 実装着手の Sequence (= TL;DR)

1. **Phase 1-2** (= ~10h) で parser + 新 NODE をビルド可能に。 機能
   regression なし (= 旧 path 経由)
2. **Phase 3** (= ~16h) で sp[] closure dispatch を leaf 限定で実装。
   ここが山場。 perf 改善が見えなければ §7 で見送り判断
3. **Phase 4-6** (= ~17h) で tail-call / call/cc / mvalues を順次対応
4. **Phase 7-8** (= ~15h) で旧 sframe path を delete + cleanup
5. **Phase 9-10** (= ~9h) で AOT cache 整合 + bench + perf.md 更新

各 phase で `make test && make test_all_stress` (= 16 backend × 17
test × stress) を回し、 1 failure でも止まる。

全体 ~67h = **work day ベース 8.5 日** (= 実 calendar week 1.5-2 週間)。
