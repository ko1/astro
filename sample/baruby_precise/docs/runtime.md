# baruby_precise ランタイム解説

このドキュメントは **baruby_precise の中身がどう動いているか** を、
これから ASTro / GC まわりを読みに行く人 (= 学部生レベル想定) に
向けて解説する。 GC 周りの「16 backend を順番に読みたい」 場合は
[gc_runtime.md](gc_runtime.md) のほうがやさしい。

- 言語仕様 → [spec.md](spec.md)
- 残タスク → [todo.md](todo.md)
- ベンチ結果 → [perf.md](perf.md)
- iter 履歴 → [done.md](done.md)

## 0. baruby_precise とは何か

`sample/baruby` (Boehm libgc を使う simple Ruby サブセット) を fork
して、 **precise な moving GC** に置き換えた実装。 つまり:

- **conservative scan**: スタック / レジスタを「VALUE っぽいビット
  パターン」 を全部辿る。 libgc 方式。 false retention あり。
- **precise scan**: 「VALUE が確実に置いてある場所」 だけを辿る。
  baruby_precise 方式。 false retention 無し、 オブジェクトを動かせる
  (= moving GC が成立する)。

precise rooting と moving GC をやるには「実行中の VALUE が今どこに
あるか」 を runtime が正確に把握している必要があり、 dispatcher /
helper の作法が conservative 版とはガラッと変わる。 §6 以降がその話。

build-time の `GC=<name>` で **16 種類の GC algorithm** を切替可能。
algorithm の違いは [gc_runtime.md](gc_runtime.md) を参照。

## 1. 全体パイプライン

ソース 1 行が結果になるまでの流れ:

```
   foo.ba.rb
       │
       ▼
   Prism (libprism.so)           ← Ruby と同じ AST を構築
       │   pm_node_t *
       ▼
   transduce (baruby_parse.c)    ← PM AST → baruby NODE 木
       │   PM_* → ALLOC_node_* + bake_X
       │   (parse 中に sp_offset / callee_fp_offset を焼き込む)
       ▼
   NODE 木 (head + operands)
       │
       ▼
   callsite_resolve              ← 前方参照の sp_body / locals_cnt 解決
       │
       ▼
   OPTIMIZE                      ← code_store/all.so から SD があれば bind
       │                          (plain mode では no-op)
       ▼
   EVAL(c, ast, sp)              ← AST を辿って実行
       │   返り値は RESULT (VALUE + state bits)
       ▼
   print result + GC stats
```

各 NODE は `head` (= 種別 + 共通フラグ) と `operand` 構造体 (= 種別
ごとの中身) を持つ。 dispatcher (`DISPATCH_node_<kind>`) と eval body
(`EVAL_node_<kind>`) は **`node.def` から ASTroGen が自動生成**する。

## 2. 値表現 — タグ付き VALUE

C 上の `VALUE` は `intptr_t`。 LSB 1 ビットで type tag:

```c
typedef intptr_t VALUE;

// LSB == 1                       fixnum (signed int63、 算術右シフトで sign-extend)
// raw == 0                       false シングルトン
// raw == 2                       true シングルトン
// raw == 4                       nil シングルトン
// LSB == 0, raw not in {0,2,4}   ヒープオブジェクト (BaArray / BaString)

#define INT2VAL(i)  ((VALUE)(((uintptr_t)(intptr_t)(i) << 1) | 1u))
#define VAL2INT(v)  (((intptr_t)(v)) >> 1)
#define VAL_FALSE   ((VALUE)0)
#define VAL_TRUE    ((VALUE)2)
#define VAL_NIL     ((VALUE)4)
```

押さえどころ:

- **比較は untag 不要** — `a < b` は両辺が同じだけ左シフトされている
  ので signed 比較が正しい順序を返す。
- **加減算は untag → op → tag** が必要 (= shift で tag bit を巻き
  込まないため)。
- **`if cond`**: C の `if` は raw 0 を false にするので `nil = 4` が
  truthy になってしまう。 `IS_TRUTHY(v)` (= `v != 0 && v != 4`) で
  両方を falsy 判定する。
- **`==` / `!=`** はまず raw 等価 (= fixnum / singleton / pointer
  identity を一発カバー) → 違ったら型を見て String byte 比較 / Array
  再帰で `baruby_value_eq`。

## 3. ヒープオブジェクト

可変長のものは 2 つだけ — Array と String。 どちらも「固定サイズの
header」 + 「別 alloc の可変長 payload」 の二層構造で、 moving GC が
**payload だけを動かせる** 形にしてある。

```c
typedef struct ObjectHeader {
    uint32_t type;   // OBJ_ARRAY (1) | OBJ_STRING (2)
    uint32_t flags;  // SSO 等
} ObjectHeader;

typedef struct BaArray {
    ObjectHeader hdr;
    uint32_t len, capa;
    VALUE *items;    // capa 個の VALUE を別 alloc
} BaArray;

typedef struct BaString {
    ObjectHeader hdr;
    uint32_t len, capa;
    char *bytes;     // NUL 終端 (printf 互換)
    char small[8];   // SSO: capa <= 7 なら bytes は small を指す
} BaString;
```

iter 53 で **SSO (small-string optimization)** 導入: 7 byte 以下の
文字列は header 内の `small[8]` に in-place で収めて 2 つ目の alloc を
省略する (`hdr.flags & OBJ_FLAG_SSO`)。 トークン頻出 workload で 5-17%
速くなる。

## 4. AST ノードと dispatcher

ASTroGen は `node.def` を読んで以下を自動生成する:

- `struct node_<kind>_struct` (= operand 構造体)
- `ALLOC_node_<kind>(...)` (= ヒープ確保 + 初期化)
- `DISPATCH_node_<kind>(c, n, sp)` (= 共通呼出 entry、 @child を eval
  してから body に値で渡す)
- `EVAL_node_<kind>(c, n, sp, ...)` (= ユーザが node.def に書いた本体)
- `HASH_*` / `DUMP_*` / `SPECIALIZE_*` 等の構造ヘルパ

baruby_gen.rb は ASTroGen 本体を継承して 2 点だけ override:

```ruby
def common_param_count = 3                            # (c, n, sp) の 3 引数
def child_dispatch_args(slot, field) = "c, #{field}, sp"  # fp 引数は無い
```

つまり baruby_precise 専用の方言は **3-arg dispatcher** だけで、
それ以外は ASTroGen の標準機能を使っている。

### dispatcher の 3 引数 = (c, n, sp)

`CTX *c` は process 全体の状態 (heap、 GC、 関数表)。 `NODE *n` は
これから eval するノード。 `VALUE *sp` は **scratch top 兼 frame
base** で、 これがこのランタイムの肝。

```
共有 VALUE stack (c->env から 8 GiB 確保):

    +-- c->env --+--- toplevel locals ---+--- f1 locals ---+--- f2 ...
    |             |                       |                 |
    fp (toplevel) fp(f1)                  fp(f2)            sp (現在)

         ←----- precise GC が flat scan する範囲 ----→
```

- `c->env` は仮想 stack の最下端 (= GC mark の起点)。
- `c->sp` は現在のスクラッチ上端。 alloc 直前に caller が更新する。
- フレームポインタ (旧 `c->fp`) は **無い**。 ローカル変数アクセス
  (`lget` / `lset`) は parse 時に sp 相対 offset へ事前変換される
  (§5 参照)。

## 5. ローカル変数アクセス: sp_offset bake

これは baruby_precise の特徴的な仕組み。 仕組みを理解すると残り
全部読みやすくなる。

### 問題

普通のインタプリタなら「現在のフレームの fp + index」 でローカルを
参照する:

```c
fp[index]    // (i) fp + index 経由
```

しかし precise rooting + 共有 stack の都合で **fp フィールドを
廃止**したい (= GC のスキャンは sp だけ見れば済む、 register 1 個減る、
SD chain での引き回しが消える)。

そこで:

```
fp[index] = (sp - locals_cnt)[index] = sp[index - locals_cnt]
```

と書き換える。 つまり `lget(index)` は実行時に `sp[X]` を読めば
よく、 X は parse 時に計算できる:

```
X = index - locals_cnt - chain
```

`chain` は「現在のノードまでに parent dispatcher たちが進めた sp
の量の合計」。

### parse-time bake (iter 72)

iter 72 でこの計算を **parse 中** に完結させた。 主な仕掛け:

1. `tc->chain_sum` を transduce 中に持ち回り。 push_frame で 0
   リセット。
2. `WITH_CHILD_CHAIN(kind, BODY)` macro: BODY を評価する間だけ
   chain を `kind.slot_count` ぶん bump (= GCC statement-expr で
   save/bump/restore)。
3. `bake_lget(tc, index)` 等のヘルパが ALLOC 時に
   `sp_offset = index - tc->chain_sum` を operand に焼く (= まだ
   locals_cnt は引いてない partial 値)。 同時にこの NODE を
   frame の `bake_list` に append。
4. `pop_frame` でその frame が `bake_list` に積んだ全 NODE を回って
   `sp_offset -= max_cnt` (= 最終確定した locals_cnt を引く) + HASH
   キャッシュを invalidate。

これで runtime の lget は `sp[sp_offset]` の **1 命令だけ** で読める。

### iter 72 以前: walker 経由

iter 61〜71 では parse 終了後に AST を再走査する walker
(`walk_bake_sp_offset`) を呼んで sp_offset を埋めていた。 iter 72 で
walker (= 170 行) は廃止。 詳細は [done.md (72)](done.md)。

## 6. メソッド呼び出しの parse-time desugar

baruby は OO を持たない (spec.md 参照)。 `recv.method(args)` 形式は
parse 時に専用ノードに変換する:

```c
// baruby_parse.c の PM_CALL_NODE 分岐
if (recv != NULL) {
    if (name == "[]")       return ALLOC_node_call_aget (recv, idx);
    if (name == "[]=")      return ALLOC_node_call_aset (recv, idx, val);
    if (name == "size")     return ALLOC_node_call_size (recv);
    if (name == "push")     return ALLOC_node_call_push (recv, val);
    if (name == "to_s")     return ALLOC_node_call_to_s (recv);
    if (name == "to_i")     return ALLOC_node_call_to_i (recv);
    // ...
}
```

各 `node_call_*` は eval 時に recv の型タグで分岐:

```c
NODE_DEF
node_call_size(CTX *c, NODE *n, VALUE *sp, VALUE r@child) {
    if (IS_ARY(r)) return RESULT_OK(INT2VAL(VAL2ARY(r)->len));
    if (IS_STR(r)) return RESULT_OK(INT2VAL(VAL2STR(r)->len));
    fprintf(stderr, "no size for non-array/string\n");
    return RESULT_OK(INT2VAL(0));
}
```

`@child` annotation は ASTroGen への合図で「この operand は parser が
NODE * として渡すが、 dispatcher が pre-eval して body には VALUE
として渡してね」。 framework が dispatch 時に sp を `slot_count`
だけ進めて @child を sp[-N..-1] にスピルし、 body に値で渡す。

文字列補間 `"a#{expr}b"` も parse 時に
`node_add(node_add(str_lit("a"), node_call_to_s(expr)), str_lit("b"))`
に展開される。

## 7. precise GC との連携

詳細は [gc_runtime.md](gc_runtime.md)。 ここでは「コードを書く側に
影響する部分」 だけ。

### Alloc API (iter 62 で framework abstraction)

```c
void *aro_gc_alloc      (CTX *c, AroGcKind kind, size_t payload_size);
void *aro_gc_alloc_byte (CTX *c, size_t payload_size);
void *aro_gc_realloc_payload(CTX *c, void *p, size_t new_size);
void  aro_gc_collect    (CTX *c);
void  aro_gc_init       (CTX *c);
void  aro_gc_fini       (CTX *c);  // clean shutdown
```

`aro_gc_alloc` は VALUE / pointer を含む payload 用 (= zero-init で
GC が未初期化 ptr を辿らないようにする)。 `aro_gc_alloc_byte` は char
配列専用 (= GC は中身を読まないので memset 省略)。

cooperative GC — `aro_gc_alloc` 経由でしか collect は起きない。

### **絶対ルール (A)**: 跨 GC 値は sp[] スロットへ

GC が走ると in-place forward で sp[] の中身は更新されるが、 C の
ローカル変数は更新されない。 alloc / helper を挟んで保持する VALUE は
**必ず sp[] に置く**。

```c
// NG: alloc 後 l が stale → SEGV する可能性
VALUE l = UNWRAP(EVAL_ARG(c, lhs));
VALUE r = UNWRAP(EVAL_ARG(c, rhs));
c->sp = sp;
baruby_str_concat(c, &l, &r);  // ← l が古いアドレスを指してる

// OK
sp[0] = UNWRAP(BARUBY_EVAL_ARG(c, lhs, sp + 2));
sp[1] = UNWRAP(BARUBY_EVAL_ARG(c, rhs, sp + 2));
c->sp = sp;
baruby_str_concat(c, &sp[0], &sp[1]);
```

### **絶対ルール (B)**: helper は `VALUE *` で受ける

内部で alloc する helper は VALUE を値で受け取らず、 caller の
sp[] slot への `VALUE *` (= sp[i] のアドレス) で受ける。 alloc 後に
`*ref` を再 deref すれば post-GC アドレスが取れる。

```c
// NG: 内部 alloc 後 av が stale
VALUE baruby_str_concat(CTX *c, VALUE av, VALUE bv) {
    ...
    aro_gc_alloc(c, OBJ_STRING, ...);  // ここで GC が走るかも
    VAL2STR(av);                         // av は古い → SEGV
}

// OK
VALUE baruby_str_concat(CTX *c, VALUE *av, VALUE *bv) {
    ...
    aro_gc_alloc(c, OBJ_STRING, ...);
    VAL2STR(*av);                        // 再 deref して post-GC を取る
}
```

### caller の `c->sp` 更新規約

alloc を呼ぶ前に caller は `c->sp = sp;` で最新の scratch top を
GC に通知する。 これにより GC は `c->env..c->sp` の範囲だけスキャン
すればよい (= flat scan)。

```c
NODE_DEF
node_ary_new(CTX *c, NODE *n, VALUE *sp, NODE *capa_node) {
    sp[0] = UNWRAP(BARUBY_EVAL_ARG(c, capa_node, sp + 1));
    c->sp = sp;                          // ← alloc 前に更新
    return RESULT_OK(baruby_ary_new(c, (uint32_t)VAL2INT(sp[0])));
}
```

helper 側 (`baruby_ary_new` 等 12 関数) は sp 引数を取らない —
caller が更新した `c->sp` を読む契約に統一済 (iter 63)。

### Write barrier

世代別 GC のため `aro_gc_wb(c, holder, slot, v)` で old→young 参照を
remembered set に push する:

```c
aro_gc_wb(c, a, (VALUE *)&a->items, (VALUE)items);
```

non-gen backend (`none` / `mark` / `copy` / ...) では no-op (= 単に
`*slot = v`)。 gen backend (`mark_gen` / `copy_gen` / ...) でだけ
remset へ push する。

### BARUBY_EVAL_ARG macro

framework の `EVAL_ARG(c, n)` は parent の sp をそのまま child に
渡す。 parent が sp[0..N-1] を root に使っているなら sp を進めて
渡したい:

```c
#define BARUBY_EVAL_ARG(c, n_node, new_sp) \
    ((*n_node##_dispatcher)(c, n_node, new_sp))
```

`@child` 化された operand なら framework が自動でこの規約に従う。
手書き helper で eval する必要があるときだけ自前で
`BARUBY_EVAL_ARG(c, child, sp + N)` を呼ぶ。

## 8. 文字列 / 配列リテラル

### `node_str_lit(bytes, len)` は eval 毎に fresh alloc

intern pool は **持たない**。 ベンチマーク観点で「同じリテラルが
ループ内で大量に新しい BaString を生む」 = collector に仕事を
させる、 という意図的な選択。

実用上は parse 時に `BaString *` を確保してキャッシュする intern
pool が欲しい (todo.md 参照)。

### 配列リテラル

`[a, b, c, d]` は size に応じた直接 alloc node:

- size 1〜4: `node_ary_lit_1` 〜 `node_ary_lit_4` で 1-shot alloc
- size 5+: `ary_new` + `ary_push` チェイン

iter 36 で size <= 4 を 1-shot 化したことで plain で -9〜-12%
(fib_pair / gc_combined / list_alloc / interp_calc) の改善が出た。

## 9. GC backend 切替

```sh
make GC=copy           # default: Cheney semispace
make GC=mark_gen       # mark+sweep 世代別
make GC=immix_gen      # Immix 世代別
# ... 計 16 種
```

16 backend の algorithm 比較は [gc_runtime.md](gc_runtime.md)、
ベンチ結果は [perf.md](perf.md) を参照。

backend を変えるときは:

```sh
make clean_bench && make GC=<name>
```

backend stamp ベース ↔ `make GC=foo` を切替えても再 link が走るので、
通常は単に `make GC=foo` でも OK。

## 10. ビルド / 実行

```sh
make                                # ./baruby_precise (= GC=copy)
make run                            # = ./baruby_precise --plain test.ba.rb
make test                           # T_*.ba.rb を実行 + oracle と diff
make test_all                       # 16 backend × test
make test_stress                    # BARUBY_GC_STRESS=1 で test
make test_all_stress                # 全 backend × stress test
make bench                          # bench/run.rb で全 bench
```

### Stress mode (`BARUBY_GC_STRESS=1`)

stress mode を有効にすると:

- **毎 alloc で GC を起動** — mark 漏れがあれば即発覚
- 古い from-space を恒久 retire (`mprotect(PROT_NONE)` + madvise) で、
  stale pointer を deref した瞬間 SIGSEGV
- 新しい to-space は毎 GC で `mmap` 取り直し

moving GC 特有のバグ (rooting 漏れ / 内部 stale local) が即座に
表面化する。 開発中の事実上の必須モード。

### AOT mode (`-c`)

`code_store/all.so` に SD (specialized dispatcher) を bake しておく。
2 回目以降の run はそれを dlopen して dispatcher を bind。
plain mode の 1.6〜6× 速くなる workload もある (sieve / hash_chain /
list_sort 等)。

```sh
CCACHE_DISABLE=1 ./baruby_precise -c bench/sieve.ba.rb
```

CCACHE_DISABLE=1 は sandbox 環境での ccache 書込み権限問題回避用
([feedback_ccache_disable](memory))。

### PG mode (`-p`)

PG mode (Profile-Guided) は実行中の callcache の最終 body を吸い
上げて `node_pg_call_<N>.sp_body` operand に焼き込む。 forward ref が
解決された後の "本物" の body が PGSD に bake される。

## 11. 既知の不整合 / 制約

- **callee frame zero-init コスト**: `node_call_<N>` で毎 call 時に
  `for (i < locals_cnt) sp[i] = 0` が走る。 全 local が即書きされる
  ことを parser が保証できれば skip 可能 (todo.md)。
- **REGION_BYTES は 64 GiB virtual**。 mmap lazy なので未使用は無料
  だが、 64 GiB を超える heap は OOM abort (= 大きな常駐データに
  非対応)。
- **`gc_copy` の stress mode**: 全 minor で from-space を恒久 retire
  するので約 65k 回 GC で `/proc/sys/vm/max_map_count` を使い果たして
  abort する。 stress で長 bench 回したいなら max_map_count を上げる。
- **`mark_bump_gen`** は tenured 側 compactor を持たない (= long-live
  old が溜まると tenured を使い切る design limit、 `mark_compact_gen`
  を使えば回避可)。

最新の todo は [todo.md](todo.md)。
