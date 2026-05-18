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

`BARUBY_GC_STATS=1` で末尾に出力する:

```
__GC_STATS__ backend=<name> alloc_bytes=<累計> heap_bytes=<live> \
             gc_count=<総数> minor=<minor 数> major=<major 数> \
             gc_seconds=<累計 GC 時間> gc_pct=<GC 比率> \
             max_pause_ms=<最大 1 回 pause>
```

- `heap_bytes` は live 量 (各 backend が独自に再計算)
- `gc_seconds` は `CLOCK_MONOTONIC` で 1 回の collect の wall time を
  累積。 minor→major の re-entrant ケースは depth guard で最外側だけ計測
- `max_pause_ms` は単発 collect の最大 pause time。 throughput では
  差が見えない latency 重視ワークロードの選択基準として、 (17) で追加。
  例えば binary_trees で mark_gen=288 ms vs mark_gen_inc=54 ms (5.4×
  短縮) のように顕在化

### 5.10 全 13 GC backend カタログ

`make GC=<name>` で 13 種類の backend を切替えてビルド可能。 切替えは
build time only (`-DBARUBY_GC=<n>`)。 default は `copy`。 共通インタフェース
(`gc.h`) は `aro_gc_init / alloc / alloc_byte / realloc_payload /
collect / wb / wb_bulk` の 7 関数で、 各 backend がそれぞれ実装する。

ベンチ性能比較は [perf.md](perf.md) §2 を参照。

#### 1. `none` — no GC, malloc + leak (baseline)

- **Layout**: libc malloc を直叩き、 leak 専用 (`free` を呼ばない)。
- **Allocation**: malloc(GCHeader + payload)。 オブジェクト毎に分散。
- **GC trigger**: 無し。
- **Write barrier**: 不要 (no GC)。 `baruby_gc_wb` は `*slot = v` に inline 化。
- **特徴**: ロookup heavy 系で per-object malloc の fragmentation が
  cache miss を生む floor が見える。 binary_trees が 0.62 s。
- **用途**: GC を完全に抜いた状態でも sp[] rooting / WB ABI / alloc API
  間接化の overhead がどれくらいかを測る baseline。

#### 2. `mark` — non-moving mark&sweep (per-object malloc list)

- **Layout**: 全 live が doubly-linked list (sentinel `head_node`) に
  繋がる。 各オブジェクトは独立 malloc 領域。
- **Allocation**: malloc + リスト head に linked。
- **GC trigger**: `bytes_since_gc > gc_threshold` で `gc_collect_internal`。
  threshold は (10) 以降 adaptive (`max(MIN=4 MiB, 2 × live_post_sweep)`) —
  fixed 4 MiB だと binary_trees で 50 回 sweep していたのを 4 回に削減。
- **Phases**: ① mark roots (`c->env..sp_top` を flat scan) ② gray queue で
  outgoing refs を辿る ③ sweep (linked list 走査、 unmarked は `free`)。
- **Write barrier**: 不要 (non-moving、 non-gen)。 `gc.h` で no-op inline。
- **特徴**: simplest mark&sweep。 binary_trees で 0.96 s (adaptive 後)、
  hash_chain で 2.20 s — per-object malloc の cache 局所性のなさが
  顕在化する典型 backend。
- **教育的位置**: 「pure mark&sweep だとどこまで遅いか」 の baseline。

#### 3. `mark_gen` — mark&sweep + 2 世代 (linked list 全体)

- **Layout**: young / old の 2 つの linked list (`young_head` / `old_head`)。
  各オブジェクト独立 malloc、 prev/next で繋がる。
- **Allocation**: 新 alloc は young list に malloc + link。
- **GC trigger**: `young_bytes > young_threshold (4 MiB)` で minor、
  `old_alloc_since_major > old_major_threshold` で major。 (10) 以降
  major threshold は adaptive。
- **Minor**: roots + 明示 remset (dirty old) から trace、 mark された
  young を old list に promote (unlink → relink)、 unmarked は free。
- **Major**: 全 list を full mark+sweep。
- **Write barrier**: 必要。 `baruby_gc_wb` が old object に書込み時
  `dirty=true` + remset push。
- **特徴**: linked-list throughout、 nursery も per-obj malloc なので
  alloc が遅い。 binary_trees 1.28 s、 string_concat 1.47 s。

#### 4. `mark_gen_inc` — mark_gen + 増分的マーキング

- **Layout**: mark_gen と同じ (young / old linked list)。
- **Allocation**: 同上、 加えて `inc_marking` 状態で増分 mark 進行。
- **GC trigger**: minor は同じ、 major は「inc_start (root mark) →
  alloc ごとに gray を少量処理 → 空になったら inc_finish_sweep」 に
  分割。 現状 `INC_WORK_PER_ALLOC = SIZE_MAX` (= 実質 STW で全部処理)
  だが、 mark phase と sweep phase が別々の time_begin/end で計測される
  ことで **max_pause が 5.4× 短くなる** (binary_trees: mark_gen 288 ms
  → mark_gen_inc 54 ms)。
- **Write barrier**: SATB (snapshot-at-beginning) — `inc_marking` 中、
  上書きされる OLD 値を gray queue に push。 これで mark 漏れを防ぐ。
- **特徴**: 真の incremental には VALUE stack WB が必要 (現状未実装)。
  infrastructure (gray queue + SATB barrier + INC_WORK_PER_ALLOC tuning) は
  揃っているので、 stack WB を足せばすぐ低 pause 化できる。

#### 5. `copy` — semispace Cheney (default)

- **Layout**: `mmap` 512 MiB の region を 2 つ。 active と to-space。
  オブジェクトは contiguous bump alloc。
- **Allocation**: `active_top` bump。 region 満タンで GC fire。
- **GC trigger**: alloc 失敗時のみ。 1 cycle で全 live が to-space に
  Cheney コピー、 active 切替。
- **Phases**: ① root forward (`c->env..sp_top` を walk、 各 IS_PTR を
  to-space にコピー) ② Cheney scan loop (to-space を進め、 outgoing refs
  を順次 forward) ③ active swap。
- **Write barrier**: 不要 (non-gen)。 `gc.h` で no-op inline。
- **Stress mode**: 旧 from-space を `mprotect(PROT_NONE) +
  madvise(DONTNEED)`、 to-space を毎回 fresh mmap。 stale pointer が
  即 SIGSEGV になる開発用モード。
- **特徴**: 単純さの極み + bump alloc の速さ。 binary_trees 0.52 s
  (`bump` と並んで最速)、 大規模 live でも Cheney が O(live) で済む。
- **API歴**: §5.5-5.7 で詳述している backend。 baruby_precise の出発点。

#### 6. `copy_gen` — bump nursery + semispace tenured (2 region)

- **Layout**: nursery 1 region (16 MiB)、 tenured 2 region (各 512 MiB)。
  両方 bump alloc。 minor は nursery 整理、 major は tenured Cheney swap。
- **Allocation**: nursery_top bump。 NURSERY_BYTES/2 を超える alloc は
  pretenured (tenured 直接)。
- **GC trigger**: nursery 満タンで minor、 `old_alloc_since_major >
  threshold` で major。
- **Minor**: roots + remset から nursery 内 live を tenured に
  Cheney 形式で copy → forward。 Cheney scan loop で chain。
- **Major**: tenured semispace を Cheney swap、 nursery も巻き込んで
  全体 reorganize。
- **Write barrier**: 必要。 `baruby_gc_wb` が tenured object に書込み時
  dirty + remset push。 explicit remset で minor scan O(|dirty|)。
- **特徴**: short-lived workload が nursery 完結して大勝。
  string_concat 0.54 s、 fib_pair 0.88 s、 cons_list 0.77 s。
- **realloc_payload bug 履歴**: (8) で「memcpy-buf-before-alloc → stale
  ptr」 を発掘して修正、 (14) で sp_top[0] rooting に統一。

#### 7. `copy_gen_inc` — copy_gen + 増分マーキング infra

- **Layout**: copy_gen と同じ。 加えて SATB barrier。
- **Allocation**: 同じ。
- **GC trigger / phases**: 構造は mark_gen_inc と同型 (inc_start →
  drain → inc_finish_sweep)。 現状 `INC_WORK_PER_ALLOC = SIZE_MAX`。
- **Write barrier**: SATB + dirty-tracking。 inc_marking 中は上書きされる
  OLD 値を mark。
- **特徴**: copy_gen と ABI 同一、 perf も 3-10% 程度の差。
  list_alloc / string_concat / substr_churn で勝つことが多い。

#### 8. `mark_compact` — single region + Lisp-2 sliding compactor

- **Layout**: `mmap` 1 GiB の単一 region に bump alloc。 linked list 不要
  (region 走査で対応)。
- **Allocation**: region_top bump。
- **GC trigger**: alloc 失敗時のみ。 4 段の Lisp-2 sliding compaction:
- **Phases**: ① mark (gray queue で trace) ② compute forwarding addresses
  (region を 1 pass、 marked obj に fwd 設定) ③ update pointers (root
  + 各 obj の ref を fwd で書換え) ④ slide (memmove で各 obj を fwd 位置
  へ実体移動)。
- **Write barrier**: 不要 (non-gen)。
- **特徴**: copy より single region で済む (1 GiB virtual)、 大型 live で
  Cheney の 2× メモリを避けられる。 binary_trees 0.58 s。 hash_chain の
  realloc-payload latent race の対象外 (slide はあるが nursery_base
  overwrite はない)。

#### 9. `mark_compact_gen` — nursery (copy) + tenured (mark + compact)

- **Layout**: nursery 1 region (bump)、 tenured 1 region (1 GiB bump、
  major で slide compact)。
- **Allocation**: nursery_top bump。 大型は pretenured。
- **GC trigger**: nursery 満タンで minor (Cheney 風に tenured へ promote)、
  `old_alloc_since_major > threshold` で major (Lisp-2 slide compact)。
- **Write barrier**: 必要 (dirty + remset)。
- **特徴**: tenured が 1 region (vs copy_gen の 2) で virtual space 節約。
  major で compact することで領域再利用 + cache locality 良好。
  interp_calc / nqueens / substr_churn で勝つ。 binary_trees 0.79 s
  (compaction が live 密集に貢献)。

#### 10. `bump` — bump alloc only, no GC (allocation floor)

- **Layout**: `mmap` 4 GiB の単一 region。 bump alloc のみ。
- **Allocation**: region_top bump。 region 満杯で abort (OOM)。
- **GC trigger**: 無し (leak)。
- **Write barrier**: 不要。 no-op inline。
- **特徴**: 「rooting + WB + dispatch」 の最小コスト baseline。
  `none` より strictly 速い (malloc 内の bin 管理がない)。
  binary_trees で 0.52 s (`copy` と並ぶ)、 hash_chain で 1.11 s (最速)。
- **用途**: alloc strategy の差を取り除いた pure mutator コスト測定。

#### 11. `mark_bump_gen` — bump nursery + bump tenured (region) + no compact

- **Layout**: nursery 1 region (16 MiB bump)、 tenured 1 region
  (1 GiB bump)。 (13)→(15)→(16) で 3 段階に進化:
  - v1: bump nursery + per-object malloc linked-list tenured
  - v2: bump nursery + bump tenured + linked-list (sweep が pointer chasing)
  - v3 (現状): bump nursery + bump tenured、 線形リスト廃止、 sweep は
    region 走査 (`p += sizeof(GCHeader) + ALIGN8(h->size)`)
- **Allocation**: 両方 bump、 pretenure 判定あり。
- **GC trigger**: nursery 満タンで minor (Cheney 形式で tenured へ
  promote)、 threshold 超過で major。
- **Phases**: minor は scan_buf を queue にした Cheney FIFO。 major は
  mark + region 走査 sweep (compact なし、 dead slot は領域内で leak)。
- **Write barrier**: 必要 (dirty + remset)。
- **特徴**: 「compaction しない bump tenured」 を mark_compact_gen との
  対比で見せるための backend。 binary_trees で 0.92 s vs mark_compact_gen
  0.79 s — 差分 ~15% が compaction の cache locality + 領域再利用効果。
  long-running では 1 GiB が累積消費されて OOM するが短時間 bench では
  問題なし。
- **GCHeader 24 B**: 線形リスト撤廃で prev/next 削除、 fwd + kind/size +
  3 bits + padding で 24 B (mark_gen の 40 B より 16 B 小さい)。

#### 12. `immix` — block (32 KiB) / line (128 B) mark-region (no evac, v1)

- **Layout**: 512 MiB の単一 arena を 32 KiB BLOCK × 16384 個に分割、
  各 block を 128 B LINE × 256 個に分割。 per-block の `line_marks[256]`
  bitmap (byte-wide for speed) で「このサイクルで marked line か」 を保持。
- **Allocation**: "hole" (= 連続した unmarked line の run) 内で bump alloc。
  `cur_ptr / cur_end` で現在の hole を track。 hole 枯渇時に
  `find_hole(n_lines)` を呼んで次の hole を探す (block_cursor / line_cursor
  で前回の続きから resume)。 block 切れたら次の block へ。 arena 全部
  枯渇したら GC trigger → 再 retry → それでも駄目なら abort (OOM)。
- **GC trigger**: `bytes_since_gc > gc_threshold` (adaptive、 mark と同じ
  式)。
- **Phases**:
  1. 全 line_marks クリア (`memset(0)` × 全 block、 16384 × 256 B = 4 MiB)
  2. mark from roots — オブジェクトを marked にすると同時に
     `mark_lines_for(h)` で span する全 line を mark
  3. sweep: block 単位で line_marks を集計、 state を FREE / RECYCLABLE /
     USED に分類。 in-arena オブジェクトの header bit はクリアしない
     (mark epoch counter で代用)。 large objects (>16 KiB) は別 mmap、
     unmarked なら munmap で返却。
  4. `cur_epoch` を tick (1..255 で wrap)。 次サイクル開始時、
     `h->mark_epoch != cur_epoch` で「未マーク」 と扱われる。
- **Mark epoch**: GCHeader に `uint8_t mark_epoch` を持たせ、
  `mark_value` で `h->mark_epoch = cur_epoch` を set、 既に `== cur_epoch`
  なら skip。 sweep 後に cur_epoch を +1 すると以前の mark は自動的に
  invalidate される。 これにより heap 全 walk による mark bit クリアが
  不要 (Immix の従来実装が悩む問題を回避)。
- **Write barrier**: 不要 (世代分離なし)。
- **特徴**: 「hole-based bump alloc + non-moving + region-aware sweep」
  の組合せ。 binary_trees で 0.68 s (`copy` 0.53 / `bump` 0.49 と比べると
  block metadata の overhead が見える)、 string_concat で 0.70 s
  (`mark_bump_gen` 0.51 より遅いが `mark` 0.68 と互角)。 mid-pack。
- **v1 制限**: evacuation を持たない。 long-running で hole の
  fragmentation (= "1 byte の live object がその line 全体を予約する"
  Immix 特有の無駄) が積み重なって有効容量が削れる。 v2 で
  opportunistic evacuation を入れる予定。
- **学術的意義**: 別の paradigm。 既存の mark/copy/compact/bump 系
  どれとも違う「line-granularity な sweep」 を ASTro 上で動かして、
  precise rooting 環境でどう振る舞うかを示せる。

#### 13. `immix_gen` — bump nursery + Immix tenured (mark-region, no evac v1)

- **Layout**: nursery 16 MiB bump region + tenured 512 MiB Immix arena
  (block 32 KiB / line 128 B、 `immix` と同じレイアウト)
- **Allocation**: nursery_bump で nursery に置く。 pretenure threshold は
  `MEDIUM_MAX` (16 KiB) — これより大きい alloc は最初から large 経由で
  promote 可能を保証 (immix の単一 block hole に収まる範囲を nursery と
  tenured で揃える)。
- **GC trigger**: nursery 満タンで minor、 `bytes_since_major > threshold`
  で major (minor 後にチェック)。
- **Minor**: nursery 生存者を `hole_alloc_header` で tenured hole に
  Cheney-copy promote。 forwarding は `oldh->kind = KIND_FREE` + payload
  先頭 8 byte に新 ptr を書き込む (payload は dead-from-source なので OK)。
  remset も処理 (dirty old → forward young refs)。
- **Major**: nursery を leading minor で fold、 その後 line_marks 全クリア
  → mark from roots → mark_lines_for で span line を set → sweep で block
  state を分類 + large objects の munmap。 immix と同じ流れ。
- **Write barrier**: 必要 (gen 系)。 H_OLD / H_DIRTY bit を `flags` に保持。
- **特徴**: short-lived 支配の workload で immix non-gen を上回る:
  - gc_combined 1.11 → **1.01** (-9%)
  - cons_list 0.96 → 0.88 (-8%)
  - hash_chain 1.49 → 1.38 (-7%)
  - list_alloc 1.03 → 0.96 (-7%)
  - string_concat 0.70 → 0.67 (-4%)
  - fib_pair 1.10 → 1.02 (-7%)
- **regression あり**: binary_trees 0.68 → 1.15 — long-lived tree なので
  Cheney copy の余分な作業が出る (古典的な「世代別 GC が苦手な workload」
  = 全 alloc が長寿命)。 immix non-gen を使うのが正解。
- **v1 制限**: tenured で evacuation なし (immix と同じ)。 nursery
  promotion 路では既存 hole にしか書けないので、 hole が枯渇したら
  major を強制 trigger する path に依存。

### 5.11 設計空間の俯瞰

13 backend を「nursery 戦略 × tenured 戦略 × compaction」 の 3 軸で
眺めた表:

| Backend | Nursery | Tenured | Compact? | Gen? |
|---|---|---|---|---|
| `none` | — | malloc list (leak) | — | no |
| `bump` | — | bump (leak) | — | no |
| `mark` | — | malloc list | no | no |
| `copy` | — | semispace (2 region) | Cheney | no |
| `mark_compact` | — | bump (1 region) | Lisp-2 slide | no |
| `mark_gen` | malloc list | malloc list | no | yes |
| `mark_gen_inc` | malloc list | malloc list | no | yes + inc mark |
| `copy_gen` | bump | semispace (2 region) | Cheney | yes |
| `copy_gen_inc` | bump | semispace | Cheney | yes + inc mark |
| `mark_compact_gen` | bump | bump (1 region) | Lisp-2 slide | yes |
| `mark_bump_gen` | bump | bump (1 region) | no (累積) | yes |
| `immix` | — | hole-bump (block/line) | no (v1) | no |
| `immix_gen` | bump | hole-bump (block/line) | no (v1) | yes (nursery→tenured copy) |

「nursery が bump か」「tenured が bump か」「major で compact するか」 が
直交軸として並び、 backend を選ぶことで各軸の影響を孤立して測れる。

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
