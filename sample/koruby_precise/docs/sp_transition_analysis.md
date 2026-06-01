# sp の遷移 — 意図 vs 実装 の体系的監査 (2026-05-30)

「dispatch sp-threading 不可能」という結論に誤りがあるはず、という指摘を受けて
`sp` がどう遷移するか/すべきかを仕様レベルで確定し、全経路で実装と照合した記録。

## 1. 仕様 (intent) — `sp` の契約

出典: `node.def:91-98`、`node.c:122-159`(どちらも著者コメント、正典)。

> 任意の NODE body において、`sp` は **frame TOP = frame_base + scope_size** を
> 指す。scope_size はその scope の locals_cnt(method)/ env_size(block)。
> - `sp[off]`(off<0、bake walker が `index - scope_size` で焼く)→ local
>   `frame_base[index]` を読む。
> - `sp[0..]` → そのbody の部分式 staging scratch(上に伸びる)。

つまり body dispatch に渡す `sp` は **必ず `frame_base + scope_size`**。

## 2. 重要な気づき — `sp` には2つの異なる役割がある

| | (A) body sp | (B) cfunc staging base |
|---|---|---|
| 値 | `frame_base + scope_size`(per-frame の frame top) | `c->sp_top`(global high-water) |
| 用途 | locals アクセス(負 index)+ そのbody の部分式 staging | **新しい callee** の self/args/scratch を置く場所 |
| 誰が使う | NODE body(threaded `sp` 引数) | dispatch(`korb_dispatch_call_cached` 等) |

**method body / top-level では A == B**(body の frame top がそのまま global
high-water。上に live data が無いから)。→ flat な `seq.rb` は divergence 0。

**block body では A != B**。block は `bfp = blk->env`(= block を作った
**method の fp**)で実行され、それを呼んだ iterator cfunc(例 `each`)の frame は
value stack 上で **block frame より上**にある。global high-water `c->sp_top` は
iterator の scratch top(block frame top より上)。実測:
```
DIV name=empty? blk=1 nodesp_off=409(=fp400+env_size9) sptop_off=486
```
block frame=[400,409)、iterator 由来の high-water=486。差 77 slot は
iterator+呼出 method の領域。

## 3. body-dispatch 全 12 site の照合 — 全て契約通り ✓

grep で全列挙(garble なし)。すべて `frame_base + scope_size` を渡している:

| site | 式 | scope_base | scope_size | 判定 |
|---|---|---|---|---|
| prologues.h:202 prologue_ast_simple_inl | `new_fp + mc->locals_cnt` | new_fp | locals_cnt | ✓ |
| prologues.h:353 prologue_ast_simple_static_inl | `new_fp + mc->locals_cnt` | new_fp | locals_cnt | ✓ |
| node_eval.c ×3 (korb_dispatch_call AST) | `new_fp + sm->u.ast.locals_cnt` | new_fp | locals_cnt | ✓ |
| object.c:4405 korb_dispatch_to_method AST | `c->current_frame->fp + m->u.ast.locals_cnt` | fp | locals_cnt | ✓ |
| object.h:952 korb_yield 高速 | `bfp + blk->env_size` | bfp=blk->env | env_size | ✓ |
| object.c:2496 korb_yield_slow | `fp + blk->env_size` | fp | env_size | ✓ |
| proc.c:360 proc_call | `c->current_frame->fp + p->env_size` | fp | env_size | ✓ |
| object.c:5162 fiber | `fib->frame + blk->env_size` | fib->frame | env_size | ✓ |
| object.c:4987/5033 top-level eval | `c->current_frame->fp`(scope_size=0) | fp | 0 | ✓ |
| node.def:508 node_scope | `c->current_frame->fp + envsize` | fp | envsize | ✓ |

**結論: body dispatch の sp 遷移は意図と完全に一致している。**

## 4. cfunc-staging 全 3 site — 全て `c->sp_top`(global high-water)✓

| site | コード |
|---|---|
| object.h:846 korb_dispatch_call_cached cfunc枝 | `VALUE *sp = c->sp_top; sp[0]=recv; …; prologue_cfunc_r_inl(…, sp+1+argc, …)` |
| object.c:4247 korb_dispatch_to_method cfunc枝 | `VALUE *sp = c->sp_top; …` |
| prologues.h prologue_cfunc_r_inl | caller が `c->sp_top` 起点で stage |

これらは **新 callee の scratch を全 live data の上に置く**ため high-water を使う。
正しい。GC は `visit_roots` が `[stack_base, c->sp_top)` を走査(koruby_runtime.c
:137)するので、 high-water に置けば必ず root 化される。

## 5. なぜ dispatch sp-threading(私の案)は壊れたのか

私の案: cfunc staging を node の threaded `sp`(= 役割A)にする。

これは **役割A と役割B を混同**していた。block body の中の call node では
`sp`(=block frame top, 409)が `c->sp_top`(=iterator high-water, 486)より下。
そこに cfunc の self/args を stage し、cfunc が入口で `c->sp_top = sp` すると:
1. iterator の live frame([409,486))の only-conservatively-covered な領域より
   下に high-water を下げる。
2. その cfunc が GC を起こすと `visit_roots` が `[stack_base, ~412)` しか走査せず、
   iterator が間接的に握っている root を見失う。
3. = rubyspec 7492→3322 の root corruption。

block body の **部分式 staging**(sp[0..], 409+)が安全なのは、それが
「既に high-water=486 でカバー済の frame 内」に収まるから(iterator は yield 中
自分の sp より下にしか live VALUE を持たない: self は sp[-argc-1]、per-iter 値は
C local の &v)。一方 **新 callee の scratch** は high-water の上=486+ に置く必要が
あり、ここが A と B の決定的な違い。

## 6. 最終結論

- **既存実装は正しい。** body sp = frame top(全12site一致)、cfunc staging =
  c->sp_top(global high-water)。この2つは設計上別物で、両方必要。
- **divergence(node-sp != c->sp_top)は block nesting による必然で bug ではない。**
- 「間違い」は私の側にあった: 2つの sp 役割を混同して cfunc staging を per-frame
  sp に置き換えようとした。これは nesting を壊す。
- したがって **cfunc-staging path を node-sp で threading するのは不可**。ただし
  `c->sp_top` は「GC 用 global high-water」として本質的に必要で、消せない。
  (`c->sp_top` を node から register 渡しする案も、node 側が結局 `c->sp_top` を
  load するので saving 無し。)
- builtins leaf-alloc の sp-threading(出荷済 478 read 削減)は別物で、これは
  「helper に渡す staging base が frame-local に分かる」ケースなので正しく機能。
  ただし cfunc が PLT call で SD inline されないため perf は neutral。

[[project_koruby_precise_sp_threading]] [[project_koruby_precise_aot_broken]]

## 7. 追加発見 — proc_call と yield の c->sp_top bump 非対称 (意図/実装の差)

監査中に見つけた唯一の実装上の非対称(bug ではないが設計の差):

| path | body dispatch 前に `c->sp_top` を frame-top へ bump するか |
|---|---|
| `proc_call` (proc.c:338) | **する**: `if (fp+env_size > c->sp_top) c->sp_top = fp+env_size;` |
| `korb_yield` fast (object.h:952) | しない |
| `korb_yield_slow` (object.c:2496) | しない |

- proc_call の bump は **上方向のみ**(`>` guard)。proc/lambda の frame は通常
  high-water の位置 or その上なので、frame top まで high-water を確実に上げる。
- yield 2 path は逆に block frame が high-water の**下**(iterator cfunc の frame
  が上)。同じ guard を置いても no-op なので付けていない。これは正しい:
  block body の部分式 staging は `sp[0..]`(= bfp+env_size 起点)で、書く先は
  `[bfp+env_size, c->sp_top)` の **既に GC scan 範囲内**。そこから cfunc を呼べば
  dispatch が `c->sp_top`(高い方)を staging base にするので high-water は下がらない。

empirical: `[1,2,3].each { |x| x.zero? }` を `ASTRO_GC_STRESS=1 ASTRO_GC_PURGE=1`
(alloc 毎 GC + from-space mprotect)で実行 → 正常(`:ok`)。block body 内 call の
GC root scan は健全。

**意図と実装は一致している。** 非対称は frame の物理配置(block は下、proc は上)
の違いに対応した正しい設計。

## 8. アセンブラ + perf による定量解析 (2026-05-30)

「reload 削減は他が遅くて埋もれているだけ、最後に効く」という見立ての検証。

### 8.1 reload は asm 上で確実に存在 (gcc -O3 microbench)

cfunc-staging の2設計を最小再現してコンパイル:
```
reload  (現状: alloc(c, c->sp_top)):
    movq 16(%rdi),%rsi ; load c->sp_top
    call alloc_a@PLT
    movq 16(%rbx),%rsi ; ★ RELOAD (alloc_a が c->sp_top を書く可能性を gcc は排除不可。実際書く)
    call alloc_b@PLT
threaded (案: alloc(c, sp)):
    movq %rsi,%r12     ; sp を callee-saved reg に park
    call alloc_a@PLT
    movq %r12,%rsi     ; reg→reg (メモリ load なし)
    call alloc_b@PLT
```
- reload は **mandatory**(alloc が実際に c->sp_top を書くので意味的に必須)。
- threaded は memory load 2→0 を達成。**user の元の意図は asm 上で実在する**。
- ただし代償: callee-saved reg を1本余分に占有 (r12)、prologue に push/pop 1組増。
  「load 削減」は「reg-move + reg 退避」に置き換わる(完全消滅ではない)。

### 8.2 削減の理論上限 (static)

all.so 全体: `c->sp_top` load 命令 = **13,445 / 1,227,127 = 1.1%** (static)。
動的(実行加重)はこれより小さい: hot path の AST-method dispatch は
prologue_ast_simple_inl 内で `prev_sp` を **C-local** に持つので reload 不要。
reload が残るのは cfunc-dispatch staging のみ。

### 8.3 実測 — 何が実際に支配的か (perf, alloc-heavy loop)

`while i<20M: s += [i].length; i+=1`(cfunc dispatch + array alloc 毎 iter):
- **24.79G instructions / 20M iter = ~1240 insn/iter**。
- perf cycle profile: **malloc family (_int_malloc/_int_free/malloc_consolidate)
  = 39.4%**、 残り 60.6%(DISPATCH_*/korb_*/GC)。
- sp_top reload が省けるのは cfunc dispatch あたり ~1 insn。1 iter に cfunc
  dispatch ~1-2回 ⇒ ~1-2 / 1240 insn = **0.1% 前後**。

### 8.4 結論 — user の見立ては「方向は正しい、桁が遠い」

- reload 削減は **実在し、asm で確認できる**(2→0 load)。
- だが現状の支配項は **malloc/free 39%** + dispatch/GC overhead。reload 1 insn は
  「indirect PLT call の隣に置かれた hot-L1 load」で、call + malloc に飲まれる。
- 「最後に効く」= 正しい。ただし効くようになるには **まず (a) アロケータ
  (libc malloc → bump/arena)、(b) cfunc を PLT call でなく SD inline 化、(c) GC
  overhead** を削る必要がある。それらが済んで初めて 0.1% が相対的に顔を出す。
- 特に (b) が鍵: cfunc が SD inline されれば、staging の sp は call を挟まず
  複数 alloc を跨いでレジスタ常駐でき、reload 削減が straight-line で効く
  (microbench の threaded 形が実現)。現状 cfunc は別TU PLT call なので不可。
- → sp-threading 自体は「正しいが時期尚早」。malloc/inline/GC を先に削るべき。
