# baruby_precise ランタイム構造

言語仕様は [spec.md](spec.md)、未対応項目は [todo.md](todo.md)、
ベンチは [perf.md](perf.md) を参照。

baruby_precise は **`sample/baruby` (libgc conservative) を copy して
precise GC に置き換えた testbed**。 `make GC=<name>` で **10 種類** の GC
アルゴリズム (none / mark / mark_gen / mark_gen_inc / copy / copy_gen /
copy_gen_inc / mark_compact / mark_compact_gen / bump) から選択できる。
設計の背景は
[`docs/gc_design.md`](../../../docs/gc_design.md) を参照。 ASTroGen
自体には手を入れず、 BODY をベタ書きで sp[] spill するスタイル。

### Write barrier と remembered set

`baruby_gc_wb(holder, slot, v)` / `baruby_gc_wb_bulk(holder, dst, src, n)`
が共通 interface。 非世代別 backend (`none` / `mark` / `copy`) は no-op
(単に `*slot = v`)。 世代別 backend (`mark_gen` / `mark_gen_inc` /
`copy_gen` / `copy_gen_inc`) は holder が old なら remset へ push し、
minor GC は remset 走査だけで old → young pointer を捕捉する。 O(|old|)
の lazy scan を O(|dirty|) に置換することで binary_trees / interp_calc
が 30〜50% 改善。

主な追加・変更点 (vs baruby):

1. **共通引数を 4 つに拡張**: `(CTX *c, NODE *n, VALUE *fp, VALUE *sp)`
   — fp は function frame base (既存)、 sp は scratch top (新)
2. **precise semi-space (Cheney) GC** (`gc.c` / `gc.h`) — libgc の代わり
3. **sp[] root spill** — NODE_DEF body が VALUE root を `sp[i]` に書き、
   `BARUBY_EVAL_ARG(c, n, sp + N)` で child に sp を進めて渡す
4. **callee frame を共有 stack 上に** — 旧 `VALUE F[locals_cnt]` の
   C-stack VLA を廃止、 `sp[0..locals_cnt-1]` に配置 (GC が flat scan)

## 1. パイプライン

```
   foo.ba.rb
       │
       ▼
   Prism (libprism.so)         libprism は naruby/prism を symlink で共有
       │   pm_node_t* (CRuby と同じ Ruby AST)
       ▼
   transduce  (baruby_parse.c)
       │   PM_* → ALLOC_node_*
       ▼
   NODE 木  (head + operand 構造体)
       │
       ▼
   OPTIMIZE() — code_store/all.so から SD/PGSD があれば bind
       │
       ▼
   EVAL(c, ast, fp, sp)  →  RESULT (= VALUE + state bit)
```

`baruby_gen.rb` は astrogen を継承して `common_param_count=4` だけ
override。 ASTroGen 自体は無修正。

## 2. 値表現 (LSB tag)

```c
typedef intptr_t VALUE;

// LSB == 1                       → fixnum (signed int63, 算術右シフトで sign-extend)
// raw == 0                       → false singleton
// raw == 2                       → true singleton
// raw == 4                       → nil singleton (false と区別)
// LSB == 0, v not in {0, 2, 4}   → heap object pointer
#define INT2VAL(i)    ((VALUE)(((uintptr_t)(intptr_t)(i) << 1) | 1u))
#define VAL2INT(v)    (((intptr_t)(v)) >> 1)
#define VAL_FALSE     ((VALUE)0)
#define VAL_TRUE      ((VALUE)2)
#define VAL_NIL       ((VALUE)4)
#define IS_INT(v)     ((v) & 1)
#define IS_FALSY(v)   ((v) == VAL_FALSE || (v) == VAL_NIL)
#define IS_TRUTHY(v)  (!IS_FALSY(v))
#define IS_PTR(v)     ((v) != 0 && (v) != 2 && (v) != 4 && ((v) & 1) == 0)
```

設計上の含意:

- **比較は untag 不要**: `(a_tagged < b_tagged)` は signed のまま
  正しい順序になる (両辺が同じ量だけ左シフトされているため)。
- **加減算は untag → op → tag** が必要。`(a + b - 1)` で tag を保つ
  トリックは現状未採用 (clarity 優先、`-O3` で gcc が shift pair を
  畳んでくれる場面が多い)。
- **`if cond`**: `nil = raw 4` は C 上で truthy になってしまうので
  プレーンな `if (...)` ではダメ。**`IS_TRUTHY(v)` 経由**で `VAL_FALSE`
  と `VAL_NIL` 両方を falsy 判定する。`node_if` / `node_while` 双方
  この方針。
- **`&&` / `||` 注意**: `INT2VAL(0) = 1` なので `node_num(0)` を
  「false 相当」として使えない。専用の `node_true` / `node_false` /
  `node_nil` ノードが各シングルトンを返す。
- **`p` / `to_s` の表示**: `VAL_NIL` → "nil"、`VAL_FALSE` → "false"、
  `VAL_TRUE` → "true"、それ以外は IS_INT / IS_ARY / IS_STR で分岐。
  `true` と Integer 1 は raw 値が違うので別々に表示される。
- **`==` / `!=`**: `l == r` の raw 等価で fixnum / シングルトン /
  ポインタ identity を一発カバー → 違ったら `IS_INT` を見て fast-fail
  → 残りで `baruby_value_eq` (String byte 比較 / Array 再帰)。
- **順序比較**: `node_lt` / `node_le` / `node_gt` / `node_ge` も
  type branch。Int+Int は tag のまま signed compare、Str+Str は
  `baruby_str_cmp` (memcmp + 長さ tiebreak)。

## 3. ヒープ型

```c
typedef struct ObjectHeader {
    uint32_t type;       // OBJ_ARRAY (= 1) | OBJ_STRING (= 2)
    uint32_t flags;      // 予約
} ObjectHeader;

typedef struct BaArray {
    ObjectHeader hdr;
    uint32_t len, capa;
    VALUE *items;        // capa 個の VALUE を別 alloc
} BaArray;

typedef struct BaString {
    ObjectHeader hdr;
    uint32_t len, capa;  // len は NUL を含まないバイト長
    char *bytes;         // NUL 終端 (printf 互換性のため)
} BaString;
```

二層オブジェクト (固定サイズ header + 別 alloc な可変長 payload) は
`docs/gc_design.md` §3 の「value.def の標準形」に揃えてある。将来
moving GC に切り替えるとき、payload だけを動かして header を pin する
道も残せる。

## 4. メソッド呼び出しの parse-time desugar

OO 機能を入れない方針 (spec.md 参照) のもと、`recv.method(args)` 形式は
**parse 時にメソッド名固定の専用ノードに変換**される。`baruby_parse.c`
の `PM_CALL_NODE` 分岐で:

```c
if (lhs != NULL) {
    if (ceq(name, "[]"))   return ALLOC_node_call_aget (lhs, idx);          // 1-arg
    if (ceq(name, "[]"))   return ALLOC_node_call_aget2(lhs, idx, count);   // 2-arg slice
    if (ceq(name, "[]="))  return ALLOC_node_call_aset (lhs, idx, val);
    if (ceq(name, "size") || ceq(name, "length"))
                           return ALLOC_node_call_size (lhs);
    if (ceq(name, "push")) return ALLOC_node_call_push (lhs, val);
    if (ceq(name, "pop"))  return ALLOC_node_call_pop  (lhs);
    if (ceq(name, "to_s")) return ALLOC_node_call_to_s (lhs);
    if (ceq(name, "to_i")) return ALLOC_node_call_to_i (lhs);
}
```

文字列 interpolation `"a#{expr}b"` は parse 時に
`node_add(node_add(node_str_lit("a"), node_call_to_s(expr)),
node_str_lit("b"))` に desugar される (`node_add` が
str+str 経路で concat する既存の挙動を流用)。

各 `node_call_*` は eval 時に recv の型タグ (`IS_ARY` / `IS_STR`) で
runtime branch する。例:

```c
NODE_DEF
node_call_size(... NODE *recv) {
    VALUE r = UNWRAP(EVAL_ARG(c, recv));
    if (IS_ARY(r)) return RESULT_OK(INT2VAL(VAL2ARY(r)->len));
    if (IS_STR(r)) return RESULT_OK(INT2VAL(VAL2STR(r)->len));
    fprintf(stderr, "no size for non-array/string\n");
    return RESULT_OK(INT2VAL(0));
}
```

ASTro の specialization で profile に応じて `_ary` / `_str` variant に
分岐する余地はあるが、現状は generic 1 本のみ。

`node_add` も同じ流儀で **int+int / str+str / ary+ary** を runtime
branch する (LIKELY で int+int のホットパスを優先)。`node_eq` /
`node_neq` も同型で、raw 等価チェックの後に `baruby_value_eq`
(`node.c`) で再帰的な値比較に降りる。

## 5. Precise semi-space (Cheney) GC

libgc を捨て、 `gc.c` / `gc.h` に precise な copying GC を実装
(~310 行)。

### 5.1 共有 VALUE stack

`main.c::create_context` で `c->env` に 100,000 slot 分の VALUE 配列を
`calloc` で確保 (zero-init)。 全 live root はこの上に居る:

- `c->env` = stack 最下端 (mark phase の起点)
- `c->fp`  = 現在の function frame base
- `c->sp`  = 現在の scratch top (= scan range の上端)

各 NODE_DEF 共通引数の `fp` / `sp` は `c->fp` / `c->sp` の register
コピー (毎 dispatch で渡される)。 `c->sp` は alloc API が内部で
update する (§5.3)。

### 5.2 半空間レイアウト

```c
typedef struct GCHeader {
    uint32_t kind;     // KIND_OBJ_ARRAY / OBJ_STRING / PAYLOAD_VAL / PAYLOAD_BYTE
    uint32_t size;     // payload bytes
    void    *fwd;      // forwarding pointer (NULL = not yet copied)
} GCHeader;
```

各 region は **`mmap` 1 回で 512 MiB 確保** (`PROT_READ|PROT_WRITE`)。
仮想空間だけ予約され、 実メモリは touch したページ分だけ消費される。
allocation は active region の `active_top` を bump するだけ。 GC が
走ると Cheney 風に live を **to-space** にコピーして active を切替える。

通常モード: `space0` / `space1` の 2 region を `mmap` で確保しておき、
GC ごとに交互に切替える (classic semispace)。

### 5.3 Stress mode (`BARUBY_GC_STRESS=1`)

stress mode を有効にすると:

- **毎 alloc で GC を起動** — 「mark 漏れ」が起きていればその場で発覚
- **古い from-space を恒久 retire** — GC 後に `mprotect(PROT_NONE)` +
  `madvise(MADV_DONTNEED)` で物理ページを解放しつつ仮想アドレスは予約
  維持。 過去 GC 由来の stale pointer を deref した瞬間 SIGSEGV
- **新しい to-space は毎 GC で `mmap` 取り直し** — 仮想アドレスは
  使い捨て (= 同一アドレスに再 alloc される偶然を排除)

これで「root rooting 漏れ」「helper 内の stale C local」 等の moving
GC 特有のバグが即座に表面化する。 開発中の事実上の必須モード。

### 5.4 Alloc API

```c
// Zero-init payload. OBJ_ARRAY / OBJ_STRING / PAYLOAD_VAL 用。
void *baruby_gc_alloc(BarubyGCKind kind, size_t payload_size, VALUE *sp_top);
// 生バイト用 (PAYLOAD_BYTE)。 memset しない — caller が即座に埋める。
void *baruby_gc_alloc_byte(size_t payload_size, VALUE *sp_top);
// 既存 payload の realloc。 kind は元の header から継承。
void *baruby_gc_realloc_payload(void *p, size_t new_size, VALUE *sp_top);
```

全 alloc が `sp_top` 引数を取る。 内部で必要なら `gc_collect_internal(sp_top)`
を呼ぶ。 cooperative — GC は alloc 経由でしか起きない。

`baruby_gc_alloc` は VALUE / pointer slot を含む payload なので zero-init
する (GC が未初期化 ptr を辿らないように)。 `baruby_gc_alloc_byte` は
char[] 専用で、 GC は中身を pointer として読まないので memset を省略。
String alloc 系のホットパスで memset コスト (3〜4% / total) を削減。

呼び出し側 (NODE_DEF body や C helper) は自分が把握している scratch top
を `sp_top` に渡す。 例:

```c
NODE_DEF
node_ary_new(CTX *c, NODE *n, VALUE *fp, VALUE *sp, /*...*/)
{
    return RESULT_OK(baruby_ary_new(0, sp));   // sp = 自分の top
}
```

### 5.5 Cheney copy collector

1. **PRE-MARK 不変条件チェック** (stress + ASTRO_DEBUG のみ): `c->env..sp_top`
   の各 `IS_PTR(v)` slot は現在の from-space 内を指していなければならない
2. **Root forward**: `c->env..sp_top` を flat scan、 `IS_PTR(v)` を
   `forward_value` で to-space にコピー (in-place で書き換え)
3. **Scan-loop**: to-space を `scan` ポインタで前進、 各オブジェクトの
   outgoing ref を `forward_payload` で順次転送
4. **Active 切替え** + (stress なら) 旧 region を恒久 retire

per-NODE_DEF の frame chain は **無い**。 GC scan は `c->env..c->sp`
の 1 続きの flat array を見るだけ。

### 5.6 BARUBY_EVAL_ARG macro

framework の `EVAL_ARG(c, n)` は parent の sp をそのまま child に渡す
ので、 parent が sp[0..N-1] を root に使う場合は **`BARUBY_EVAL_ARG(c,
n, sp + N)`** で sp を進めて child に渡す:

```c
NODE_DEF
node_call_1(... NODE *a0)
{
    /* sp[0..locals_cnt-1] が callee の locals 領域 (root) */
    for (uint32_t i = 0; i < locals_cnt; i++) sp[i] = 0;
    sp[0] = UNWRAP(BARUBY_EVAL_ARG(c, a0, sp + locals_cnt));
    return RESULT_OK(EVAL(c, cc->body, sp, sp + locals_cnt).value);
}
```

`#define BARUBY_EVAL_ARG(c, n_node, new_sp) \
    ((*n_node##_dispatcher)(c, n_node, fp, new_sp))` (node.h)

### 5.7 Moving GC で必須の二大パターン

semi-space に切り替えた結果、 mark&sweep 時代には潜伏していたバグが
顕在化した。 NODE_DEF / C helper を書くときの **絶対ルール**:

**(A) sp[] spill** — heap VALUE を子ノード eval を跨いで保持する場合、
C local ではなく sp[] slot に置く。 子の eval で GC が走ると in-place
forward で sp[] は更新されるが、 C local は更新されない。

```c
// NG: rhs 評価で GC が走ると l が stale → SEGV
VALUE l = UNWRAP(EVAL_ARG(c, lhs));
VALUE r = UNWRAP(EVAL_ARG(c, rhs));
baruby_str_concat(l, r, sp);

// OK
sp[0] = UNWRAP(BARUBY_EVAL_ARG(c, lhs, sp + 2));
sp[1] = UNWRAP(BARUBY_EVAL_ARG(c, rhs, sp + 2));
baruby_str_concat(&sp[0], &sp[1], sp + 2);
```

**(B) helper は VALUE* で受ける** — 内部で alloc する helper は VALUE を
値で受け取らず、 caller の sp[] slot への pointer で受ける。 alloc 後に
`*ref` を再 deref して post-GC アドレスを取り直す。

```c
// NG: 内部 alloc 後 av が stale → VAL2STR(av) で SEGV
VALUE baruby_str_concat(VALUE av, VALUE bv, VALUE *sp_top) {
    const BaString *a = VAL2STR(av);   // pre-GC ptr
    ... baruby_gc_alloc(...) ...        // ここで GC 起こりうる
    a = VAL2STR(av);                    // av は C local のまま stale
}

// OK
VALUE baruby_str_concat(VALUE *av_ref, VALUE *bv_ref, VALUE *sp_top) {
    uint32_t a_len = VAL2STR(*av_ref)->len;   // size だけ pre-alloc で読む
    ... baruby_gc_alloc(...) ...               // GC で *av_ref は in-place forward
    const BaString *a = VAL2STR(*av_ref);     // post-GC ptr
}
```

この pattern を `baruby_ary_push` / `_plus` / `_repeat`、 `baruby_str_concat`
/ `_repeat` / `_slice` / `_append` 等に適用。 stress mode で検証済み。

`baruby_str_new(const char *bytes, ...)` だけは ref pattern を採らず
**source bytes が呼び出し中ずっと valid**を caller に要求 (rodata /
C スタック / GC-rooted)。 これは hot path で literal string が圧倒的に
多いので、 ref pattern の strict 適用より直接 memcpy が速い。 heap
interior が source の場合は `baruby_str_slice(VALUE *src_ref, offset,
len, sp_top)` を使う。

### 5.8 ASTRO_ASSERT / ASTRO_DEBUG

framework 共通の assertion macro。 `runtime/astro_debug.h`:

```c
#if ASTRO_DEBUG
#  define ASTRO_ASSERT(expr) assert(expr)
#else
#  define ASTRO_ASSERT(expr) ((void)0)
#endif
```

baruby_precise では `ASTRO_DEBUG=1` がデフォルト (`context.h` で設定)。
`make ASTRO_DEBUG=0` でビルドすれば assertion / stress mode の verbose
check が compile time に消える。

GC 内部の不変条件 (alloc 時の kind validity、 `process_object` の type
タグ整合、 stress mode の PRE-MARK 範囲 check 等) はすべて
`ASTRO_ASSERT` 経由。

### 5.9 統計出力

`BARUBY_GC_STATS=1` で `__GC_STATS__ alloc_bytes=... heap_bytes=... gc_count=...`
を末尾に出力する。 `alloc_bytes` は累計、 `heap_bytes` は live 量
(Cheney の scan loop で再計算)、 `gc_count` は collection 回数。

## 6. 文字列リテラルの fresh alloc

`node_str_lit(bytes, len)` は **eval 毎に新しい `BaString` を確保する**。
intern pool は持っていない。これは GC testbed としての feature
(同じリテラルが loop 内で大量にゴミを生む = collector が忙しくなる)。

ベンチ用途を超えて常用するなら parse-time に `BaString *` を生成して
キャッシュする intern pool が欲しい (todo.md)。

## 7. 配列リテラルのチェイン展開

`[a, b, c]` は parse 時に下記の AST に落ちる:

```
ary_push(
  ary_push(
    ary_push(ary_new(), a_expr),
    b_expr),
  c_expr)
```

`ary_new` は eval されるたびに新しい空配列を確保する。各 `ary_push`
は引数 lhs を eval して同じ配列をそのまま返すので、AST が線形チェインで
展開されてもアロケーションは leaf の `ary_new` 1 回だけ (= リテラル評価
1 回ぶん)。

## 8. ベンチ・実行モード

`make` で `./baruby_precise` ができる:

| target | 効果 |
|---|---|
| `make` | 通常ビルド |
| `make run` | `./baruby_precise --plain test.ba.rb` |
| `make bench` | `bench/run.rb` で全 bench を実行 |
| `make clean` | 生成物 + code_store を消す |

AOT mode: `CCACHE_DISABLE=1 ./baruby_precise -c bench/list_alloc.ba.rb`
で動作確認済 (perf.md §2 参照)。 CCACHE_DISABLE は sandbox 環境での
ccache 書込み権限問題回避用。

## 9. 既知の不整合 / 制約

- **toplevel sp が 64 で hardcode** (`main.c::create_context`)。
  本来は parser が toplevel locals_cnt を返してくれば計算可能。
  大きな toplevel フレームでは scratch 不足の恐れ
- **callee frame zero-init コスト**: `node_call_<N>` で
  `for (i < locals_cnt) sp[i] = 0` を毎 call で実行。 function call
  頻度に比例。 zero-init が要らない (= 全 local が即書きされる) ことを
  parser が保証できれば skip 可能
- **REGION_BYTES が 512 MiB 固定**: live set がこれを超えるプログラムは
  OOM で abort。 mmap は lazy なので未使用ページは物理メモリを食わないが、
  仮想空間は確実に消費する
