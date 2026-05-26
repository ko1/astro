# ascheme runtime — ソフトウェアアーキテクチャ

ascheme は ASTro framework 上に build した R5RS Scheme 実装。 本 doc は
`.scm` source が動作する program になるまでの流れ — reader から AST、
dispatcher chain を通って optional な AOT (および PGO) code-store、 evaluator
の trampoline までを描く。

user 向けの案内は [`../README.md`](../README.md) を参照。 ASTro framework
全体については [`../../../docs/idea.md`](../../../docs/idea.md) に記載。

## 1. パイプライン全体像

```
   .scm source
        │
        ▼
   reader (main.c)            S-expression → tagged scheme VALUEs
        │
        ▼
   compile() (main.c)         scheme VALUE → AST
        │   ALLOC_node_xxx         node_lambda, node_call_K, node_arith_*, …
        │   ───────────────►   OPTIMIZE(n)    ← opportunistic SD_<hash> lookup
        │                                        in code_store/all.so
        ▼
   AST in heap (linked NODE *, GC-managed)
        │
        ▼
   eval_top(c, ast)            EVAL(c, n, sp) = (*n->head.dispatcher)(c, n, sp)
        │
        ▼   per-node dispatch chain (node_seq → node_arith_add → …)
   VALUE  (return value of the top-level form)

   Closure calls go through `scm_apply`, which trampolines TCO via
   c->tail_call_pending / next_body / next_env.
```

3 つの実行 mode で同じ AST と同じ生成 dispatcher を共有する:

| Mode | `code_store/` の中身 | hot-path dispatch |
|---|---|---|
| **interp** | 空 | `DISPATCH_node_xxx` (AST edge ごとの関数 pointer) |
| **AOT** (`--aot-compile`) | 全 entry の `SD_<hash>.so` | `SD_<hash>` (子は同じ .so 内の static SD call として inline) |
| **PGO** (`--pg-compile`) | threshold 超えの entry の `SD_<hash>.so` のみ + `profile.txt` | hot entry: `SD_<hash>`、 cold: default の `DISPATCH_node_xxx` |

`make compare` (および `make compare-big`) で各 mode + chibi-scheme + guile
の wall-clock を表形式で出す。

## 2. 値表現

`VALUE` は `int64_t`、 3 種の tag class を持つ — Ruby 風:

```
xxxx_xxx1  fixnum (signed 62-bit, value = (int64_t)v >> 1)
xxxx_xx10  flonum (IEEE-754 double encoded inline; bit-rotation; CRuby's scheme)
xxxx_x000  pointer to heap-allocated `struct sobj` (8-byte aligned)
```

inline flonum は IEEE double のうち上位 3 exponent bit が `0b011` または
`0b100` のもの全て (= 大きさで概ね `[1e-77, 1e+77]`) を round-trip する。
0.0、 NaN、 ±inf、 範囲外は heap `OBJ_DOUBLE` に fallback する。 影響は
[`mandel`](../bench/big/mandel.scm) と [`nbody`](../bench/big/nbody.scm) を
参照: 内側 loop の flonum allocation を消すとそれぞれ 6×/1.7× の speedup
(README の bench table 参照)。

heap object (`struct sobj`) は `int type` tag と union を持つ:

```
type           variant
────────────   ────────────────────────────────────────────
OBJ_PAIR       { VALUE car, cdr }
OBJ_SYMBOL     { char *name }                  (interned)
OBJ_STRING     { char *chars; size_t len }
OBJ_CHAR       { uint32_t cp }
OBJ_VECTOR     { VALUE *items; size_t len }
OBJ_CLOSURE    { Node *body; sframe *env; int nparams, has_rest; bool leaf; ... }
OBJ_PRIM       { scm_prim_fn fn; const char *name; int min/max_argc }
OBJ_DOUBLE     { double dbl }                  (heap path; flonum-encoding miss)
OBJ_BIGNUM     { mpz_t mpz }                   (GMP)
OBJ_RATIONAL   { mpq_t mpq }                   (GMP)
OBJ_COMPLEX    { double re, im }
OBJ_PROMISE    { VALUE thunk, value; bool forced }
OBJ_PORT       { FILE *fp; bool input, closed, owned }
OBJ_CONT       struct scont *cont               (out-of-line — see below)
OBJ_MVALUES    { VALUE *items; size_t len }    (return value of `(values …)`)
OBJ_BOOL,OBJ_NIL,OBJ_UNSPEC,OBJ_EOF — singleton sobj's
```

`sizeof(struct sobj) == 48` byte。 continuation の状態 — `jmp_buf` + 小さい
3 field — は inline ではなく `struct scont` の pointer 越しに置く。 split
しないと union が全 cons cell / vector header を ~208 byte (`jmp_buf` の
size) まで pad してしまう。 [`docs/perf.md`](perf.md) §9 を参照。 `scm_cons`
は更に `offsetof(sobj, pair) + sizeof(pair)` = 24 byte だけ allocate し、
Boehm の最小 bucket に収まる (§10)。

GMP の allocation は `mp_set_memory_functions` で `GC_malloc` 経由に route
してあるので、 bignum と rational も他と同様に conservative GC で回収される。

closure variant は parse 時に立てる `bool leaf` を持ち、 lambda の body が
内側 `lambda` を含まない時に真。 `leaf` は §4 の 2 つの hot-path 最適化を
enable する: 非末尾再帰呼出での `alloca` による stack frame と、 self-tail
call での in-place frame 再利用。

## 3. AST ノード

[`node.def`](../node.def) から ASTroGen + 小さな
[`ascheme_gen.rb`](../ascheme_gen.rb) extension で生成 (後者は `@ref`-stored
cache struct の hash / dump / specialize の仕方を framework に教える)。

### カテゴリ

| Group | Nodes | 用途 |
|---|---|---|
| **Literal** | `node_const_int`, `node_const_int64`, `node_const_double`, `node_const_str`, `node_const_sym`, `node_const_char`, `node_const_bool`, `node_const_nil`, `node_const_unspec`, `node_quote` | self-evaluating な定数と quote 済 scheme value |
| **変数** | `node_lref`, `node_lset`, `node_gref`, `node_gset`, `node_gdef` | `(set! x v)` 等 — `lref/lset` は `(depth, idx)` で lexical frame chain を辿る。 `gref` は名前 lookup + `@ref` inline cache |
| **制御** | `node_if`, `node_seq`, `node_lambda` | 分岐、 sequencing、 closure 生成。 `node_lambda` は parse 時に stamp した `leaf` operand を持つ。 |
| **Call** | `node_call_0`…`node_call_4`, `node_call_n`, `node_callcc` | 固定 arity / 可変長 / `call/cc` (escape のみ)。 tail position は parse 時に `is_tail` flag を bake する。 |
| **特殊化算術** | `node_arith_add/sub/mul/lt/le/gt/ge/eq` | head symbol が `+ − * < <= > >= =` と一致し lex-shadow されていない時、 parse 時に fold |
| **特殊化述語** | `node_pred_null/pair/car/cdr/not` | `null? / pair? / car / cdr / not` を fold |
| **特殊化 vec** | `node_vec_ref`, `node_vec_set` | `vector-ref / vector-set!` を fold |
| **特殊化 list / eq** | `node_cons_op`, `node_eq_op`, `node_eqv_op` | `cons / eq? / eqv?` を fold |

### R5RS 再束縛下での特殊化安全性

R5RS では算術 operator の再束縛が許されている — global
(`(set! + my+)` / `(define + my+)`) でも lexical
(`(let ((+ -)) …)` / `(lambda (+) …)`) でも。 ascheme はそれぞれを適切な
phase で扱う:

**Lexical shadowing — parse 時に検出。** `try_specialize_arith` は
specialize node を出す前に head symbol を `lex_lookup` する。 名前が外側
scope のいずれかに bound していれば `NULL` を返し、 parser は generic な
`node_call_K` (関数位置に `lref`) に fallback する。 *specialize しない*例:

```scheme
(let ((+ -)) (+ 5 3))                  ; emits node_call_2(lref +, …) → 2
((lambda (+) (+ 1 2)) *)               ; same idea — `+` is a parameter → 2
(let ((car cdr)) (car (cons 1 2)))     ; `car` is local → 2
```

**Global 再束縛 — 実行時に検出。** head symbol が lex-bound でない場合、
specialize node は `@ref`-stored cache を持って emit される:

```c
struct arith_cache { int32_t resolved; uint32_t index; };
```

hot path は inline fixnum / flonum fast path に入る前に
`c->globals[cache->index].value == PRIM_<op>_VAL` (= `install_prims` 時に
snapshot した値) を check する。 後続の `(set! + my+)` / `(define + new+)`
で global slot の value が書き換わり、 equality が崩れ、 `arith_dispatch{1,3}`
経由 — 現在 `+` が bind している先への通常の `scm_apply` — に route される。
実行時 case は [`test/13_redefine_arith.scm`](../test/13_redefine_arith.scm)
を参照。

### Tail-call trampoline + frame 再利用

`node_call_K` は compile 時に stamp する `is_tail` field を持つ (`if` /
`begin` / `let` 等を伝播する)。 `scm_apply_tail` は inline header version
(hot path) と out-of-line slow path (`scm_apply_tail_slow` in main.c) に
split している。 速度改善は [`docs/perf.md`](perf.md) §6、 §7、 §12 を参照。

Hot path (`node.h` の `static inline` を介して全 dispatcher と SD 関数に
inline される):

- **末尾位置 + leaf closure + 同じ shape** — 現 frame の slot を in-place
  で上書きして allocate 無しで再入。 「同じ shape」 check (`c->env->parent
  == cl->closure.env && c->env->nslots == total`) により tight tail loop
  は実行全体で 1 つの `sframe` を再利用する。
- **それ以外** — `scm_apply_tail_slow` を call、 `build_frame_for` と heap
  に fallback する。 非 leaf closure では slow path にも入る (`leaf` gate
  は内側 lambda が captured frame を escape している可能性がある時の再利用
  を防ぐ)。

trampoline は `scm_apply` の closure 経路に置く:

- *leaf* closure では frame を C stack に `alloca` し、 `GC_malloc` を
  skip する。 lifetime はこの `scm_apply` call の duration と等しい —
  非末尾再帰では alloca frame が自然に積まれる。
- `for(;;)` loop が `tail_call_pending` を catch し、 新しい (body, env)
  で body を再実行する。 再利用された alloca/heap frame でも、 別 shape
  target 用に slow path が生成した fresh heap frame でも構わない。

つまり trampoline は:

- **末尾位置 + closure target** — `c->next_body / c->next_env /
  c->tail_call_pending` を set し、 dummy 値で即 return する。
- **非末尾 または 非 closure** — `scm_apply` を実行 (frame を build し
  body の dispatcher chain に入る)。

closure 用 `scm_apply` は `EVAL(c, body)` を `for(;;)` で囲み、
`tail_call_pending` が立っている間ずっと再入する。 C frame 1 つで任意の
TCO 深度。 10⁶ / 相互再帰 case は [`test/09_tco.scm`](../test/09_tco.scm)
を参照。

## 4. Evaluator (interp mode)

```c
static inline VALUE
EVAL(CTX *c, NODE *n, VALUE *sp)
{
    return (*n->head.dispatcher)(c, n, sp);
}
```

**3-arg dispatcher** (= commit `04af2521`、 baruby_precise iter 61 と同
パターン): `sp` を関数引数として call chain 全体に register-resident で
持ち回る。 `c->sp` の per-call load / store を避けることで dispatch
overhead を削減。 plain mode で **-18%**、 AOT mode で **-22%** の
speedup (= `docs/perf.md` 参照)。

`n->head.dispatcher` の初期値は ASTroGen が node kind ごとに生成する
`DISPATCH_node_xxx`。 各 `DISPATCH` は node の field を読んで
sample 側 `EVAL_node_xxx` (= `DISPATCH` から強制 inline) を呼ぶ。
signature は `(CTX *c, NODE *n, VALUE *sp, ...)` — `ascheme_gen.rb`
が `common_param_count = 3` と `child_dispatch_args` を override して、
sp を child dispatcher に自動で thread する。

`EVAL_ARG(c, child)` は `(*child_dispatcher)(c, child, sp)` に展開
される (= 親と **同じ** sp を child に渡す)。 NODE_DEF body が N
スロット予約する場合 (= `SP_PUSH(c, sp, N); sp[0]=...; sp[1]=...`)、
child dispatcher には `sp + N` を渡して自フレームの slot を上書き
されないようにする (= body で明示的に書く)。 child の dispatcher は
後に specialized `SD_<hash>` に差し替え可能。 親の code は不変、
link は `dispatcher` field 経由で static call ではない。

body の `sp` は基本 register-resident。 `SP_PUSH` は **GC root scan
用** に `c->sp = sp + N` を sync する (= GC は `[g_sp_scratch, c->sp)`
範囲の VALUE を全て alive として scan)。 `main.c` の alloc helper も
`aro_gc_alloc` 呼出前に `c->sp = sp + N` を書く (= §7.2 参照)。

## 5. AOT (`--aot-compile`) モード

parse 後、 `aot_compile_and_load` が `AOT_ENTRIES` (compile 時に登録された
非 `@noinline` AST node 全て) を walk する:

1. **`astro_cs_compile(entry, NULL)`** を各 entry で実行 → `code_store/c/SD_<Horg>.c`
   を書き出す。 子は同 `.c` file 内の `static inline` helper として emit
   され、 gcc が inline すると親 entry の body に dispatcher が畳み込まれる。
2. **`astro_cs_build(NULL)`** が `make -j` (再生成された Makefile) を
   実行し全 `.c` を `.o` に compile して `code_store/all.so` に link する。
   `CCACHE_DISABLE=1` を set してあるので `--clear-cs` rebuild が
   ccache-warm ではなく honestly cold になる。
3. **`astro_cs_reload()`** が `all.so` を `all.<gen>.so` に rehardlink し
   新 path を `dlopen` する (glibc は pathname で cache するので、 fresh
   inode が再読込を強制する)。
4. **`astro_cs_load(entry, NULL)`** を各 entry で実行 → `dlsym("SD_<hash>")`
   し `entry->head.dispatcher` を SD 関数に patch する。

`--aot-compile` の以降の起動では全 `.c` がすでに disk にあるので、 re-link
と `dlopen` だけが走る。 これが bench の "aot-cached"。

## 6. PGO (`--pg-compile`) モード

`sample/abruby` の `--pg-compile` を踏襲。 1 回の ascheme 起動で:

1. AST に parse + compile。
2. **interp** で実行 (= この run では AOT を適用しない)。
   `scm_apply` の closure branch が entry ごとに `body->head.dispatch_cnt`
   を increment するので、 program 終了時点で body ごとの真の実行回数が得られる。
3. `AOT_ENTRIES` を walk し、 `AOT_PROFILE_THRESHOLD` (= 10) を超えるものに
   filter して `--aot-compile` と同じ compile / build / load sequence を実行
   する — ただし hot subset のみ。
4. `(Horg, count)` tuple を `code_store/profile.txt` に persist する。

次の `--aot-compile` 起動は自動で `profile.txt` を拾い、 同じ threshold
filter を適用する — つまり cold entry は `DISPATCH_node_xxx` のまま、 小さ
めの `all.so` がより速く load され、 hot path は変わらない。 これが bench
の "pg-cached"。

abruby はさらに一歩進んで、 profile 派生定数 (method prologue 等) を生成
C に bake する別 keyed `Hopt` の `PGSD_<Hopt>` variant を emit する。
ascheme の特殊化 node は既に `PRIM_*_VAL` 経由で hot-path 定数を inline
しているので、 parallel hash を持たなくてもその利得の大半は得られている。

## 7. Garbage collection (= precise GC framework)

ascheme_precise は元の `ascheme/` が依存する libgc の代わりに
**ASTro precise GC framework** (`runtime/precise_gc/`) を使う。 移行履歴と
動機は [`migration.md`](./migration.md)、 framework 自体は
[`../../../docs/gc_design.md`](../../../docs/gc_design.md) に記載。

17 個の GC backend (algorithm の詳細、 用語、 allocation 戦略、 write
barrier / remset 設計、 finalizer semantics) は root の
[`../../../docs/gc_runtime.md`](../../../docs/gc_runtime.md) に一度だけ
documented してある。 以下の section は **ascheme_precise 固有の wiring**
(= allocation API の使い方、 root 追跡 hook、 SCAN_EDGES dispatch、 GMP
finalizer 連携) のみを記述する。

### 7.1 Allocation API

全ての scheme allocation は framework の 2 つの call のいずれかを通る:

- `aro_gc_alloc(c, sz)` — scan-safe payload (= sample 定義の SCAN_EDGES
  が内部の VALUE slot を訪問する)。 **encoded VALUE** を返す (= scramble
  backend では bit が XOR mask 済、 非 scramble backend では macro が
  identity に潰れる)。
- `aro_gc_alloc_byte(c, sz)` — byte-only payload (= VALUE slot 無し、
  例えば `BaByteData` 風の raw buffer)。 同じ encoded 戻り値型。

sample は encoded value を slot に直接 store する:

```c
sp[0] = aro_gc_alloc(c, sizeof(struct sobj));   /* encoded */
struct sobj *o = (struct sobj *)ARO_LOAD(c, &sp[0]);  /* decode for init */
SCM_SET_TYPE(o, OBJ_PAIR);
o->pair.car = ...;
```

`ARO_LOAD(c, slot)` が encoded VALUE から raw pointer を取り出す **唯一の**
方法。 `ARO_LOAD` を忘れて (= raw cast で) encoded bit を pointer として
使うと、 `copy_scramble` audit backend で即 SEGV として表面化する。

### 7.2 Root 追跡

ascheme は sample 定義の hook `AROH_VISIT_ROOTS(c, ctx, edge_visit)` で
root set を framework に渡す。 実装 (`main.c` の `aro_scheme_visit_roots`)
は以下を訪問する:

- `c->env` (= 現在の `struct sframe *`)
- `c->next_env` (= tail-call-pending environment)
- 定義済 global 全ての `c->globals[i].value`
- `c->globals[i].name_payload` (= 名前文字列の byte payload)
- `c->loop_args[0..N]` (= scm_apply の scratch buffer)
- `SYMBOL_TABLE[0..LEN]` (= intern 済 symbol)
- `PORT_STDIN` / `PORT_STDOUT` / `PORT_STDERR` (= stdports)
- `QUOTE_NODES[*]` (= compile 時に capture した literal)
- `PRIM_*_VAL` (= 特殊化算術 fast-path sentinel)
- **`g_sp_scratch[0..c->sp - g_sp_scratch)`** (= `SP_PUSH` で予約された
  per-call scratch slot)

framework backend は自身の collect entry からこの hook を call する。
backend は `c->env` や `c->sp` を直接見ない (= iter 76 以降 framework は
CTX-opaque)。

**3-arg dispatcher の sp 同期** (= commit `04af2521`): NODE_DEF body は
`sp` を function parameter として持つ (= register-resident)。 `SP_PUSH(c,
sp, N)` は `c->sp = sp + N` を書くだけで、 これにより GC root scan の
範囲 `[g_sp_scratch, c->sp)` が body の予約 slot を含む。 `main.c` の
alloc helper (= `scm_make_pair`、 `build_frame_for` 等) も `aro_gc_alloc`
呼出前に明示的に `c->sp = sp + N` を書く。 helper 自体は `sp` を引数で
受けるか、 function entry で `VALUE *sp = c->sp;` を declare して GC
safepoint で re-sync する。

### 7.3 オブジェクト走査 (= SCAN_EDGES)

`AROH_SCAN_EDGES(payload, sz, ctx, edge_visit)` は sample 定義で、
`head.flags & SCM_TYPE_MASK` で dispatch する:

- OBJ_PAIR — `pair.car`、 `pair.cdr` を訪問
- OBJ_VECTOR / OBJ_MVALUES — `items` の base + 各 `items[i]` を訪問
- OBJ_SYMBOL / OBJ_STRING — interior char-buffer の base を訪問 (= helper
  `ASCHEME_VISIT_INTERIOR_CHAR_SLOT`)
- OBJ_CLOSURE — `closure.env`、 prim-builtin の name buffer を訪問
- OBJ_PROMISE — `thunk`、 `value` を訪問
- OBJ_CONT — `cont` 自体 (= heap obj) + scont 内に save された `env` /
  `result` / `k_val` / `fn_val` を訪問
- OBJ_FRAME (= sframe) — `parent` + 各 `slots[i]` を訪問

### 7.4 Finalizer (= 外部 resource の cleanup)

ascheme は bignum (`mpz_t`) と rational (`mpq_t`) に GMP を使う。 GMP は
`gmp_alloc` から返した buffer への raw pointer (= `_mp_d`) を保持する。
それら buffer は **libc-malloc** であり (= GC heap の外) 以下が成立する:

- moving GC は relocate しない (= `_mp_d` は valid のまま)
- 非 moving GC は sobj を scan した時に free しない (= `_mp_d` は
  SCAN_EDGES から見えない)

`OBJ_BIGNUM` / `OBJ_RATIONAL` sobj が collect される時、 framework が
`AROH_FINALIZE(payload)` を call し `mpz_clear` / `mpq_clear` に dispatch
する。 これらは `gmp_free` (= `free()`) 経由で libc buffer を release する。

登録は allocation 時に行う:

```c
VALUE scm_make_bignum_z(CTX *c, mpz_srcptr z) {
    VALUE v = aro_gc_alloc(c, sizeof(struct sobj));
    struct sobj *o = (struct sobj *)ARO_LOAD(c, &v);
    SCM_SET_TYPE(o, OBJ_BIGNUM);
    mpz_init_set(o->mpz, z);
    aro_gc_finalize_register(c, o);   /* ← finalize list に登録 */
    return v;
}
```

finalize list は `AroGcCommonState` 内で libc-malloc され、 weak reference
table として機能する (= framework は SCAN_EDGES からは訪問しない、 さも
なくば全部 alive のままになる)。 各 collect 後、 `aro_gc_finalize_walk`
が各 entry を backend の `aro_gc_finalize_check` で check する — live
entry は pointer が更新され (= moving GC 用)、 dead entry は
`AROH_FINALIZE` が呼ばれて drop される。

GMP の memory pressure (= LCG chain が MB スケールの `_mp_d` を生成する)
は `aro_gc_account_external(c, ±bytes)` で framework に報告され、 GC
threshold check に拾われる — さもなくば bignum-heavy code は framework が
気付いて collect を trigger する前に GB 級の libc memory を allocate して
しまう。

### 7.5 Backend 選択

17 個の GC backend が `make GC=<name>` で利用可能 — 全 list、 algorithm
要約、 backend ごとの trade-off は root の
[`../../../docs/gc_runtime.md`](../../../docs/gc_runtime.md) §1 / §2 / §3
を参照。

選択の動機と libgc 対比の bench 数値は [`perf.md`](./perf.md) を参照。
audit 用 knob (`BARUBY_GC_STRESS=1`、 `BARUBY_GC_PURGE=1`) もそこに記述。

## 8. リポジトリ配置

```
sample/ascheme/
├── README.md             user guide
├── docs/runtime.md       this file
├── node.def              AST node definitions (40 kinds)
├── ascheme_gen.rb        ASTroGen extension (handles `@ref` cache structs)
├── context.h             VALUE / sobj / CTX / GMP+GC prototypes
├── node.h                NodeHead, EVAL, extern decl for arith helpers
├── node.c                runtime wiring (alloc, OPTIMIZE, includes generated)
├── main.c                reader, compiler, primitives, drivers (interp / AOT / PGO)
├── Makefile              build, test, bench, bench-big targets
├── test/                 16 self-tests + chibi r5rs-tests adapter
├── bench/small/          quick micro-benchmarks (~1 s interp)
├── bench/big/            substantial workloads (multi-second interp)
├── bench/compare.sh      runs benches across ascheme / chibi / guile
├── code_store/           AOT artefacts (gitignored)
└── .chibi/               chibi-scheme 0.12 cache for `make compare` (gitignored)
```

## 9. 制限

- **`dynamic-wind` 無し** — `call/cc` は escape-only (one-shot, downward)。
  期限切れ continuation の再 invoke は明示的な error を上げる。
- **`syntax-rules` 無し** — user 定義 macro 無し。 compiler は
  `quasiquote` / `unquote` / `unquote-splicing` を認識して `cons` / `list`
  / `append` に展開する。
- **再定義 operator の `eqv?` immutable** — §3 の arith cache trade-off を
  参照。 影響は `+ − * < = …` のみ。
- **source レベル行追跡無し** — `NodeHead.line` は予約済だが未使用。 PGC
  の `(file, line) → Hopt` index は従って Horg のみ。
- **Boehm GC quirk** — pointer-free buffer (string/symbol char data) は
  `GC_malloc_atomic`、 他は全て `GC_malloc` を使い内部参照を scan させる。
