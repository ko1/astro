# runtime.md — nuq の実装詳解

nuq は ASTro 上に乗せた **jq 互換ツリーウォーカー**。各 NODE_DEF が
`EMIT { items, count }` を返し、items は CTX 上の flat な VALUE pool
に切られたスライス。per-emit GC alloc ゼロ + SD specializer による
AOT inlining と相性のいい構造を取る。さらに **per-run arena +
Cheney 式 stop-the-world copying GC** で長 run / 巨大入力時の
中間累積を mid-run reclaim する。

ファイル構成:

| ファイル | 内容 |
|---|---|
| `node.def`  | AST ノード定義 (フィルタ言語 + 全 builtin) |
| `node.h`    | NodeHead + EMIT pool helper |
| `context.h` | VALUE / nuq_obj / CTX / 公開 API + `static inline` fast path + GC pin API |
| `node.c`    | アロケータ + ASTroGen 生成ファイルの `#include` |
| `value.c`   | VALUE 構築 / 比較 / 算術 slow path / **arena + copying GC** |
| `json.c`    | JSON parser + pretty-printer |
| `runtime.c` | tree-eval helpers (object_eval / user_call / reduce / sort / module loader …) |
| `filter.c`  | jq lexer + parser + AST fusion + module directives |
| `builtin.c` | builtin の VALUE-level 実装 |
| `main.c`    | CLI driver |

## 1. 値モデル

```
xxxx_xxx1 → 62-bit signed fixnum (左 1 シフト + 1)
xxxx_xxx0 → ヒープオブジェクト (`struct nuq_obj *`、8-byte aligned)
```

`null` / `true` / `false` は **静的 singleton** (グローバル `struct
nuq_obj` のアドレス)。fixnum 範囲外の整数は heap-boxed double。

```c
struct nuq_obj {
    enum nuq_type type;     /* NULL / BOOL / DOUBLE / STRING / ARRAY / OBJECT / FORWARD */
    union {
        bool b;
        double dbl;
        struct { char *bytes; size_t len; } str;
        struct {
            VALUE *items;          /* points at inline_buf when capa <= NUQ_ARR_INLINE */
            size_t len, capa;
            VALUE  inline_buf[4];  /* 4-slot inline storage */
        } arr;
        struct {
            VALUE *keys, *vals;    /* parallel arrays, insertion order */
            size_t len, capa;
            uint32_t *idx;         /* lazy hash index, NULL until len > 16 */
            uint32_t  idx_mask;
        } obj;
        struct nuq_obj *forward;   /* GC: type == NUQ_T_FORWARD のとき */
    };
};
```

- 整数は fixnum、`__builtin_*_overflow` で失敗時のみ heap double に昇格
- 配列は **挿入順 + 4 slot inline buffer**。`nuq_make_array(N)` で
  N ≤ 4 なら `inline_buf` を使い alloc 節約
- オブジェクトは **挿入順 parallel array `keys[]` / `vals[]`** + 16
  keys 超で **lazy hash idx** (open-addressing FNV-1a, load ≤ 0.5)。
  挿入順イテレーションは parallel array 側でそのまま

## 2. EMIT プロトコル

各 NODE_DEF は `EMIT` 構造体を return する (16 byte、`{ items,
count, flags }`)。`items` は **CTX の `pool[]` 上のスライス**、`count`
はその長さ。

```c
NODE_DEF
node_b_length(CTX *c, NODE *n)
{
    return nuq_emit_one(c, nuq_length(c->input));
}
```

呼び出し側の責任で **pool top0 を保存し、必要なら巻き戻す**:

```c
NODE_DEF
node_array(CTX *c, NODE *n, NODE *body)
{
    size_t top0 = c->pool_top;
    EMIT bo = EVAL_ARG(c, body);                 /* SD inline */
    if (UNLIKELY(c->error != NUQ_NULL)) return EMIT_EMPTY;
    VALUE arr = nuq_make_array(bo.count);
    for (uint32_t i = 0; i < bo.count; i++) nuq_array_push(arr, bo.items[i]);
    c->pool_top = top0;                           /* slice 解放 */
    return nuq_emit_one(c, arr);
}
```

エラーは `c->error` (NUQ_NULL = OK) で伝搬、break は `c->break_label`
(0 = 無)。pool 巻き戻しは stack-discipline で sub-expr の emits を
解放する hot pattern。

mid-stream で error が発生した場合、すでに pool に push 済の値は
slice として return する (jq 互換の partial output 規則)。`nuq_run`
が pool スライスを stdout に出した後で `c->error` を stderr へ flush
する。

## 3. value 演算 fast path

`context.h` に `static inline` で fixnum 高速路、slow case は `_slow`
接尾辞で `value.c`:

```c
static inline VALUE
nuq_op_add(VALUE a, VALUE b) {
    if (LIKELY(NUQ_IS_FIX(a) && NUQ_IS_FIX(b))) {
        int64_t la = NUQ_FIX_VAL(a), lb = NUQ_FIX_VAL(b), r;
        if (LIKELY(!__builtin_add_overflow(la, lb, &r))) return nuq_make_int(r);
    }
    return nuq_op_add_slow(a, b);
}
```

inline 対象: `add` / `sub` / `mul` / `neg` / `mod` / `eq` / `cmp` /
`truthy` / `make_int`。`div` は jq 仕様で常に double 演算
(`5/2 == 2.5`) なので fast path 無し。

## 4. CTX

```c
typedef struct CTX_struct {
    VALUE                 input;          /* 現 `.` */

    /* EMIT pool — flat VALUE buffer.  Each NODE_DEF pushes onto pool
     * starting at pool_top, returns a slice; caller rewinds pool_top
     * to top0 to release.  Pre-grown to 4096 entries at startup. */
    VALUE                *pool;
    size_t                pool_top, pool_capa;

    struct nuq_var_slot  *var_stack;      /* `as $x` 束縛 */
    size_t                var_top, var_capa;

    struct nuq_func_def **funcs;          /* `def` 定義のスタック */
    size_t                func_cnt, func_capa;
    size_t                func_skip_start, func_skip_end;

    VALUE                 error;          /* NUQ_NULL = no error */
    uint32_t              break_label;    /* 0 = no break */
    bool                  path_drop_pending;  /* select 経由の drop signal */
} CTX;
```

CTX は **`calloc` で確保** する。`pool` / `var_stack` / `funcs` 等の
内部ポインタは初期 NULL で、後から `realloc` で伸ばす。プロセス終了
まで保持される (CLI ツールなので明示 free はしない)。

## 5. メモリ管理 — Per-run arena + Cheney copying GC

### 5.1 動機

ASTro samples の従来パターン (旧 nuq も) は **Boehm GC 一本**:
- 値ごとに `GC_malloc(sizeof(nuq_obj))`
- per-alloc に lookup / sweep / mark のコスト
- 100MB JSON 処理で 10-15% が libgc 内に消える
- `libgc.so` 依存

per-run arena で改善できる点:
- bump-pointer alloc は 2 命令 (cmp + add)
- run 終了で wholesale reset (GC sweep 不要)
- per-emit alloc が事実上ゼロコストに
- libgc 依存を剥がせる (libm + libc のみで動く)

ただし **run 内で死んだ中間値を回収する仕組み**がないと、
`reduce range(N) as $i ([]; . + [$i])` のような accumulating
mutation で **memory が O(N²) に膨れる** (各反復が新配列を作って
旧配列を捨てるが、arena は run 終わりまで free しない) — これに
対しては Cheney 式 copying GC + 線形性解析 (`linearity.c`) で対処
する (§5.3 / [perf.md](perf.md))。

### 5.2 二層構造

| 領域 | アロケータ | 寿命 | 用途 |
|---|---|---|---|
| **永続領域 (perm)** | `malloc` / `calloc` / `realloc` | プロセス終了まで | AST / 字句リテラル / `--argjson` / `--slurpfile` / module data / intern table / def_table / CTX 自身 / pool バッファ / var_stack / funcs[] |
| **per-run arena** | `nuq_value_alloc` (bump) | run 1 回 + mid-run minor GC | filter eval が作る中間 VALUE (array / object / string / heap-double / 各 buffer) |

`nuq_alloc_perm` フラグで切替: 起動時 (parser / module load /
`--argjson` parse / data import) は perm = true で plain malloc 行き、
`nuq_run` 突入時に false へ。run 終了で `nuq_arena_reset()` +
perm = true。

```c
static inline void *
nuq_value_alloc(size_t sz)
{
    if (UNLIKELY(nuq_alloc_perm)) return malloc(sz);
    return nuq_arena_alloc(sz);
}
```

(実装上は `GC_malloc` という名前のマクロが残っているが、これは
`calloc(1, sz)` への単なるリダイレクト — 旧 Boehm の zero-init 契約
を保つため。コードを段階的に書き換える際の互換シム。)

arena chunk も plain `malloc/free`。永続領域の値 (literal や
module data) は別の global table からも reachable で、明示 free しない
限り解放されない。permanent allocations は process exit でまとめて
OS が回収する (CLI なので問題なし)。

### 5.3 Cheney 式 copying GC

run 内の dead 累積を回収するため、`arena_total > 16 MB` で minor GC
を発火させる。stop-the-world、from-space → to-space で生きてる
オブジェクトをコピー。

#### Forwarding pointer

obj をコピーしたら、旧 obj の slot に forwarding 情報を書き込む:

```c
old->type = NUQ_T_FORWARD;
old->forward = new_obj;
```

union のおかげで `forward` は他のフィールド (arr.items / str.bytes /
…) と同じ offset。続く参照が旧 obj を見たら type を見て new に
飛ばす。

#### Combined alloc layout

`make_array` / `make_object` / `make_string` は **obj ヘッダ +
out-of-line buffer (items / keys / vals / idx / bytes) を 1 回の
arena_alloc で連続割り当てる**。これが Cheney scan の鍵:

```
[ obj header (64B) ][ items[capa] ][ next obj ][ next items ]...
```

scan ポインタは obj 単位で前進し、obj 種類 + capa から自身のフット
プリント (header + buffer) を計算してスキップする。obj とその
buffer がチャンクをまたいで分裂すると scan が壊れる (チャンク先頭
が obj header と認識されない) ので、必ず単一 alloc に収める。

#### Cheney scan ループ

```c
struct nuq_chunk *scan_chunk = gc_to_first;
char *scan_ptr = scan_chunk ? scan_chunk->data : NULL;
while (scan_chunk) {
    for (;;) {
        char *chunk_end = (scan_chunk == gc_to_current)
                          ? gc_to_cur
                          : scan_chunk->data + scan_chunk->used;
        while (scan_ptr < chunk_end) {
            struct nuq_obj *o = (struct nuq_obj *)scan_ptr;
            size_t sz = gc_obj_total_size(o);
            gc_scan_obj(o);              /* 子 VALUE を forward */
            scan_ptr += sz;
        }
        if (scan_chunk == gc_to_current && scan_ptr < gc_to_cur) continue;
        break;
    }
    scan_chunk = scan_chunk->next;
    scan_ptr = scan_chunk ? scan_chunk->data : NULL;
}
```

ポイント:
- `chunk->used` は当該 chunk が「ほかの chunk に追い抜かれた」時に
  記録される使用バイト数。current chunk は `gc_to_cur` で代用
- スキャン中に新規コピーが伸びる (children を forward するため) →
  current chunk なら同じ chunk を再ループで延長
- 新規 chunk が追加されたら次イテレーションで拾う

#### Roots

GC が forward する VALUE のソース:

1. **CTX state**:
   - `c->input` / `c->error`
   - `c->pool[0..pool_top]`
   - `c->var_stack[0..var_top].value`
   - `c->funcs[i]->var_snap[j].value` (call-by-name closure の
     キャプチャされた variable bindings)

2. **Transient pin stack** (`nuq_gc_roots[]`): C 関数のローカルで
   alloc を跨いで生存させたい VALUE をプッシュ。

3. **Transient array pin stack** (`nuq_gc_arrs[]`): NODE_DEF が
   pool slice を別バッファに snapshot したときの VALUE 配列を
   pin。

#### 助手関数の規則 — pin と re-fetch

raw `struct nuq_obj *` のローカルは **alloc を跨いだら必ず stale**
になる (obj が to-space に移ったため)。規則:

1. VALUE 入力は `NUQ_GC_PIN1/2/3` で pin する (alloc 時に GC が
   forward して slot が新位置を指すよう更新する)
2. 各 alloc 後に `NUQ_PTR(value)` で raw ptr を **再取得**
3. 出口で `NUQ_GC_UNPIN(n)` で pop

例 — `nuq_clone`:

```c
case NUQ_T_OBJECT: {
    NUQ_GC_PIN1(v);
    size_t len = o->obj.len;
    bool has_idx = (o->obj.idx != NULL);
    VALUE r = nuq_make_object(len);          /* GC may fire here */
    NUQ_GC_PIN1(r);
    /* Re-fetch after make_object. */
    struct nuq_obj *src = NUQ_PTR(v);
    struct nuq_obj *dst = NUQ_PTR(r);
    for (size_t i = 0; i < len; i++) {
        dst->obj.keys[i] = src->obj.keys[i];
        dst->obj.vals[i] = src->obj.vals[i];
    }
    dst->obj.len = len;
    if (has_idx) obj_idx_rebuild_v(r);
    NUQ_GC_UNPIN(2);
    return r;
}
```

- `v` を pin → `make_object` 中 GC が走っても `v` の slot 経由で
  最新位置へ
- `r` を pin → 後の `obj_idx_rebuild_v` 内 GC でも追跡
- `src` / `dst` は pin しない。**alloc を跨いで使わない** ことが
  契約 (各 alloc 後に再取得すれば良い)

#### NODE_DEF の snapshot pin

`node_add` 等は cartesian のため lhs 出力を別バッファに snapshot
する:

```c
EMIT l = EVAL_ARG(c, lhs);
uint32_t lc = l.count;
VALUE local_small[16];
VALUE *local = (lc <= 16) ? local_small : (VALUE *)malloc(lc * sizeof(VALUE));
memcpy(local, l.items, lc * sizeof(VALUE));
NUQ_GC_PIN_ARR(local, lc);
c->pool_top = top0;
EMIT rv = EVAL_ARG(c, rhs);     /* ここで GC が走り得る */
...
NUQ_GC_UNPIN_ARR();
```

local は stack か `malloc` の独立バッファで、Cheney scan は触れない。
PIN_ARR で配列単位の root を登録し、GC walk が中身の VALUE を
forward する。

#### Big alloc と GC trigger

`sz > NUQ_ARENA_CHUNK / 2` (= 512 KB) は大物専用 chunk へ。**GC
トリガはこの分岐の前で判定** する — 大物が dedicated chunk path
でしきい値チェックを bypass すると、`reduce` で要素 60k 超のとき
N=200k で OOM になる事態が発生したため。

#### Spare chunk のリサイクル

GC 完了で from-space chunk は `arena_spare` に移し、再利用に備える。
ただし **growing-acc workload** (毎反復で前回より大きな array が
要る) では、spare に残った chunk が次回サイズ要求に届かず、累積
する一方になる。`NUQ_SPARE_KEEP = 3` で枝刈りして上限固定。

### 5.4 評価

| bench | nuq before GC | nuq after GC | 改善 |
|---|---:|---:|---:|
| Q3 reduce N=50k mem | 11313 MB | 22 MB | **500×** |
| Q3 reduce N=50k time | 13.0 s | 1.0 s | 13× |
| Q1 6-stage map mem | 1438 MB | 268 MB | 5× |
| Q1 6-stage map time | 1.6 s | 1.5 s | 同等 |
| big.tree_leaf_sum | 2.14× vs jq | 2.70× vs jq | +26% |
| big.group_city | 1.55× | 1.75× | +13% |
| micro.group-by 100k | 18.3× | 20.6× | +13% |
| real.transform | 1.46× | 1.79× | +23% |

通常 bench に regression 無し。GC は `arena_total > 16 MB` でのみ
発火するので、典型的な map / select / sort / group_by chain では
そもそも走らない。

残課題: jq の `acc + [x]` を refcount で in-place mutation に最適化
する系の高速化は未実装。Q3 N=50k で nuq 1.0 s vs jq 0.01 s の
100× 差はこれが原因。memory は完全解決。

### 5.5 Debug build (stale pointer を即 segfault にする)

`make gctest` で `-DNUQ_GC_DEBUG_MPROTECT=1 -DNUQ_GC_DEBUG_STRESS=1`
の二段構成で build する `nuq-gcdebug` を回せる:

- **`NUQ_GC_DEBUG_MPROTECT`**: chunk を `malloc` ではなく
  page-aligned `mmap` で確保。Cheney 終了時に from-space chunk を
  spare に戻す代わりに `mprotect(PROT_NONE) + madvise(MADV_DONTNEED)`
  で物理ページを解放しつつ仮想アドレスを reserve したまま deny に。
  ピン抜けで stale arena ptr を deref した瞬間に SEGV になる。
  通常 build は recycled chunk が後続 alloc に上書きされて corrupt
  が観測まで持ち越されるが、この build なら deref 場所が直接
  特定できる。
- **`NUQ_GC_DEBUG_STRESS`**: `nuq_arena_alloc` が常に slow path に
  落ちる + slow path の threshold check を skip → 毎 alloc で
  Cheney GC を強制発火。production の 16 MB threshold まで届かない
  latent な pin 漏れを表面化。

production build と stress+mprotect build の両方で `make test`
370/370 PASS を維持。pin 抜けが新たに混入したら 1 cell 落ちる
ところで止めれば deref 行が gdb で即見える。重い (毎 alloc で full
GC) ので CI と debug 専用、production には混ぜない。

実装は `value.c` (mprotect は `arena_take_chunk` / GC 終了 / 
`nuq_arena_reset` の 3 箇所、`#if NUQ_GC_DEBUG_MPROTECT` で分岐) と
`context.h` (`nuq_arena_alloc` の inline で `NUQ_GC_DEBUG_STRESS` 時
slow path 直行)。

### 5.6 Cheney scan ループの subtle bug (修正済み、参考)

旧実装は inner walker を:

```c
for (;;) {
    char *chunk_end = (scan_chunk == gc_to_current) ? gc_to_cur
                                                    : ... + used;
    while (scan_ptr < chunk_end) { gc_scan_obj(o); scan_ptr += sz; }
    if (scan_chunk == gc_to_current && scan_ptr < gc_to_cur) continue;
    break;
}
```

と書いていた。`chunk_end` を `for(;;)` 頭で 1 度だけ捕捉、while で
消費、足りなければ `continue` で再評価、という意図。問題は scan
中に `gc_to_alloc` が新規 chunk を allocate して transition すると
発生する:

1. `scan_chunk` は元 chunk のまま、`gc_to_current` は次 chunk へ。
2. 元 chunk の `used` は transition で確定 (gc_to_alloc 内で seal)。
3. 元 chunk にはまだ `chunk_end` (= 旧 gc_to_cur) と `used` の間に
   未 scan obj が残る — transition 直前の最後のひと押しで積まれた
   ぶん。
4. ところが `if (scan_chunk == gc_to_current && ...)` が false なので
   `break` してしまい、scan は次 chunk に進む。間の obj は
   forward されない。

forward されない obj の keys/vals は from-space を指したまま。GC
終了で from-space chunk は spare に戻るが、後続の `arena_take_chunk`
で再利用されると別データで上書きされ、stale ptr deref が segv に
化ける。10K-scale `transform` / `keys_aggregate` で intermittent
segfault が出ていた根本原因。

修正は inner を素朴な single-step に戻す:

```c
for (;;) {
    char *chunk_end = (scan_chunk == gc_to_current) ? gc_to_cur
                                                    : ... + used;
    if (scan_ptr >= chunk_end) break;
    /* one obj */
}
```

毎反復で chunk_end を再評価するので transition 後も chunk の最終
fill を正しく踏める。debug build (§5.5) と組み合わせると、こうした
silent corruption は SEGV まで一気に縮退するので発見が早い。

## 6. 主要ノードの意味論

`./nuq --dump-ast` で実 AST を確認できる。

### `.users[] | .name`
```
node_pipe
├── lhs: pipe(field("users"), iter)
└── rhs: field("name")
```

### `[.users[] | select(.age > 30)] | length`
```
node_pipe
├── lhs: array
│         └── body: pipe(pipe(field("users"), iter),
│                        b_select(body=gt(field("age"), int(30))))
└── rhs: b_length
```

SD specializer は AST 全体を 1 つの SD 関数に折り畳む (lhs の
array_ctor → 内側 pipe → users access → iter ループ → select の
cartesian → length まで全部 inline)。

AST fusion の `[X] | length` ルールが先に発火して `emit_count(body)`
1 ノードになる場合もある — fusion は parser 内で意味保存のまま。

### `map(.name)` (= `[.[] | .name]`)
```c
NODE_DEF
node_b_map(CTX *c, NODE *n, NODE *body)
{
    VALUE result = nuq_make_array(o->arr.len);
    NUQ_GC_PIN1(result);
    for (size_t i = 0; i < o->arr.len; i++) {
        c->input = NUQ_PTR(input)->arr.items[i];   /* re-fetch input */
        size_t top0 = c->pool_top;
        EMIT bo = EVAL_ARG(c, body);    /* body SD inline; GC 可 */
        for (uint32_t j = 0; j < bo.count; j++) nuq_array_push(result, bo.items[j]);
        c->pool_top = top0;
    }
    NUQ_GC_UNPIN(1);
    return nuq_emit_one(c, result);
}
```

### `def f(g; h): body;` の call
ユーザ定義 `def` は side-table へ lower、call サイトは
`node_call(name_id, arity, args)`。`runtime.c` の `nuq_user_call` が
- value-arg (`$`-prefix) は eager 評価、cartesian 展開
- filter-arg (no-prefix) は **call-by-name closure** (§ 7)

## 7. Call-by-name closure

`def f(x): ...` の `x` (no-prefix) は call site で **値ではなく式
AST と caller scope を保存** する。f の body から `x` を参照する
たびに caller scope で式を再評価。

実装: `struct nuq_func_def` に `var_snap` (var stack snapshot) と
`var_snap_cnt`:

```c
struct nuq_func_def {
    uint32_t   name_id;
    int        arity;
    uint32_t  *param_ids;
    bool      *param_is_value;
    struct Node *body;
    size_t     scope_top;
    struct nuq_var_slot *var_snap;
    size_t     var_snap_cnt;
};
```

f が呼ばれるとき、各 filter-arg `g` について 0-arity の `pfd` を作り
`body = arg AST`、`scope_top = c->func_cnt - 1` (f を skip した
caller scope)、`var_snap = clone(c->var_stack)` で現在の var stack を
スナップ。

`x` が呼ばれた時:
- 既存 var stack を退避
- snap を fresh 配列にクローンして c->var_stack に swap
- body を eval (snap clone の上で `as $y` 等で push しても破壊しない)
- swap を戻す

func スコープも `func_skip_start` / `func_skip_end` で
`pfd->scope_top` までに制限し、f 自身や f が定義した内側 def を
見えなくする。

## 8. SD specialization と AST fusion

### SD specialization

ASTro の SD specializer は `EVAL_ARG(c, child)` を見つけると child
の dispatcher を constant-fold して、child の body を親 SD に
inline する。nuq では:

- **runtime helper を経由しない**: 多くの NODE 本体は node.def 直接
  展開で `EVAL_ARG` を使う
- **builtin が個別 NODE**: `length` / `map` / `select` / `range` 等
  60+ の builtin は parser が直接対応 NODE を生成
- **再帰 def 本体を独立 entry に登録**: `nuq_user_call` 内の
  `EVAL(c, fd->body)` は runtime resolved dispatcher なので
  top-level filter SD からは inline できない。`def_tab` を walk
  して各 body を別 entry として `astro_cs_compile` に渡す

### AST fusion (parse-time peephole)

`filter.c` の `nuq_make_pipe(lhs, rhs)` で意味保存の rewrite:

- `map(F) | map(G)` → `map(F | G)` (中間配列消去)
- `select(F) | select(G)` → `select(F and G)` (短絡保存)
- `[body] | length` → `node_emit_count(body)` (専用ノード)
- `[body] | add` → `node_emit_fold_add(body)` (`add` の type-dispatch
  kernel `nuq_add_fold_items` を共有)
- 右辺エッジ fusion: 左結合 chain `f | g | h` を 1 段ずつ折り畳む

意味保存は jq 公式テスト + ローカル差分テストで常時チェック。

## 9. Path-mode walk

`walk_path(c, n, v, fn, ud)` は AST `n` を path として辿り、leaf に
`fn` を適用しつつ container を rebuild。代入 `=` `|=` / `del` /
`setpath` の実装基盤。

サポートする path 構成要素:
- `.` (identity) — leaf
- `.foo` (field) — descend、auto-vivify object
- `.[expr]` (index) — int → array、string → object、auto-vivify
- `.[]` (iter) — for each child
- `.[a:b]` (slice)
- `pipe(a, b)` — recurse a with `nested_apply(b, fn, ud)`
- `select(cond)` — cond truthy なら fn 適用
- `as $x | body` — bind して body へ
- `..` (recurse) — bottom-up rebuild、各 sub-tree に fn 適用
- `getpath([keys...])` — path 配列で descend

`..` 経由の代入: `(.. | select(P) | .b) |= F` で全マッチを
bottom-up に更新。

## 10. Lazy stream eval

`limit(N; gen)` / `first(gen)` / `last(gen)` / `nth(N; gen)` /
`any(gen; cond)` / `all(gen; cond)` / `isempty(gen)` は
`nuq_stream_eval(c, body, cb, ud)` ヘルパが gen を遅延展開:

- `body` が `node_comma(lhs, rhs)` なら `stream_eval(lhs)` → 続けて
  `stream_eval(rhs)`
- `body` が `node_pipe(lhs, rhs)` なら `stream_eval(lhs,
  pipe_inner_cb)` で各 lhs emit に対し `c->input` を設定して
  `stream_eval(rhs)`
- それ以外は普通の `EVAL`、各 emit について `cb(c, v, ud)` を呼ぶ
- `cb` が false を return すると stream を打切る

これで `limit(1; 1, error)` が `1` で停止、`error` を評価しない。

## 11. Module loader

`filter.c` 末尾の section に実装:

- `struct nuq_module`: cache キーは canonical abs path、各 module に
  ユニークな `ns_id` を割り当て
- `loaded_defs[]`: 全 module の def を flat に集めた配列 — 名前は
  `<ns_id>::<original>` で qualify
- `parse_directives`: top-level / module file head の
  `module {meta};` / `import "X" as foo;` / `include "X";` を
  recursive に処理
- `prescan_local_defs`: module body を full parse する前に lex-scan
  で def 名 + arity を集める。これにより body 内の bare 名 call を
  `<my_ns>::<name>` に rewrite できる
- 探索: `{search: "..."}` import meta → `-L` パス → CWD の順で
  `<dir>/<rel>.jq` と `<dir>/<rel>/<rel>.jq` を試す
- データ import (`as $var`) は `nuq_user_arg_add_value` 経由で
  `$var` と `$var::var` 両方に bind
- `include` は後勝ち shadow — alias / include 配列を末尾から逆順で検索
- 循環 import は cache に pre-register することで安全に終結
- `modulemeta` builtin は別経路で軽量 lex-scan のみ (defs を実際に
  load しない)

## 12. JSON I/O

`json.c` に手書き再帰下降パーサ + pretty-printer。

- パーサは `(src, len, *endp, *errmsg) → VALUE`
- 深さ 10001 以上で `"Exceeds depth limit for parsing"` を返す
- jq 互換の `Infinity` / `-Infinity` / `NaN` / `nan` リテラル accept
- エラーメッセージは jq 互換の `at line L, column C (while parsing
  '<src>')` 形式
- pretty-printer は jq 互換の数値整形:
  - 整数値の double を整数表記
  - 通常は最短 round-trip (`%.15g` から `%.17g` まで増やして strtod
    一致確認)
  - 整数値で `>2^53` の大きい double は `%.17g` のマンティッサ +
    末尾ゼロパッディングで fixed-point 化
  - 深さ 10001 以上で `"<skipped: too deep>"` プレースホルダ
- 出力は `*_unlocked` stdio + bulk fwrite で per-byte ロックを回避

## 13. ASTro / Code Store

`INIT()` で `astro_cs_init("code_store", ".", 0)`。`main.c` は parser
出力 AST に対して `astro_cs_compile` → `astro_cs_build` →
`astro_cs_reload` → `astro_cs_load` で SD を生成 dlopen → dispatcher
に patch (`--no-compile` で skip)。

`astro_cs_build` の `make` が ccache 経由で落ちる環境では
`CCACHE_DISABLE=1` を設定。

再帰 def の AST を hot loop に載せるため、`nuq_compile_all_def_bodies`
が `def_tab` を walk して全 def body を独立 entry として登録、
`nuq_load_all_def_bodies` が dlopen 後に dispatcher を patch する。

## 14. 設計上の妥協

- emit は **CTX 上の flat VALUE pool** からのスライス。pros: per-emit
  GC alloc ゼロ、SD inlining 容易。cons: pool 巻き戻し忘れがバグる
- pipe は **lhs を一旦配列に集めて iterate**。streaming にはなって
  いない (Cheney GC で memory は bound するが、time はそのまま)
- object は **挿入順 parallel array + lazy hash idx**。jq 互換のため
  keys は順序保持
- 値表現は IEEE-754 double + 62-bit fixnum。decnum 未対応
- `acc + [x]` の in-place mutation は未実装。jq の refcount-based
  最適化に相当 — escape analysis で同等を実現できる余地あり
  ([`todo.md`](./todo.md) 参照)
