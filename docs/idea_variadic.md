# Variadic Node のための ASTroGen 拡張

(旧称: VLP — Variable Length Parameters)

## 1. 動機

`call(f, a, b, c)` のように **引数の個数が AST 構築時点で決まる、可変長な子ノード列を持つノード** を、`call1` / `call2` / `call3` ... のように arity ごとに別ノード型として `node.def` に並べたくない。

ASTro の最重要前提は「**関数ポインタを名前として渡せば呼出先でインライン展開される**」という gcc/clang の挙動である。これを満たすには、子ノードの dispatcher が C ソース上で **named identifier** として登場する必要がある (`EVAL_ARG(c, lv)` が `(*lv_dispatcher)(c, lv)` に展開される、という EVAL_ARG マクロの規約)。

可変長な子ノード列を「配列＋個数」として渡すと、`args[i]##_dispatcher` というトークン連結が成立せず、インライン展開の前提が崩れる。素朴に書くと:

```c
for (int i = 0; i < cnt; i++) r += (*args[i]->head.dispatcher)(c, args[i]);
```

これは間接呼び出しのループになり、ASTro の高速化機構が機能しない。

## 2. 設計上の制約

検討する解は **以下の原則をすべて守る** ものでなければならない:

1. `node.def` に書かれた **EVAL body は ASTroGen が一切書き換えない**（C パーサを書かない、という ASTroGen の根本方針)
2. **`EVAL_ARG` マクロは現状のまま** (`(*n##_dispatcher)(c, n)`)
3. `node.def` に **arity 別の宣言を並べない**（単一の `node_call` で済ませる）
4. パーサは arity を意識せず単一の allocator API を呼ぶ
5. ホット部位で gcc が **フルインライン展開できる** 形になっている

## 3. 解 — ASTroGen による per-arity 特化版の自動生成

### 3.1 node.def 側のシンタックス

```c
NODE_DEF
node_sum(CTX *c, NODE *n, NODE *args[])      /* args[] が variadic マーカ */
{
    VALUE r = 0;
    FOR_EACH_NODE_ARG(arg,
        r += EVAL_ARG(c, arg)
    );
    return r;
}
```

- `NODE *args[]` という配列形パラメータを ASTroGen が「variadic 子ノード列」として認識する。
- `FOR_EACH_NODE_ARG(VAR, ...)` は **C プリプロセッサのマクロ**。`EVAL_ARG` と同様、ASTroGen が出力する scaffolding 中で `#define` される規約マクロ。
- body は通常の C コードとしてのみ読まれ、ASTroGen は中身を解析しない。

### 3.2 ASTroGen が生成する EVAL/DISPATCH（K = 0..MAX_INLINE_ARITY + generic）

#### K-特化版

```c
#define FOR_EACH_NODE_ARG(VAR, ...) \
    do { const NODE *VAR = args_0; \
         const node_dispatcher_func_t VAR##_dispatcher = args_0_dispatcher; \
         __VA_ARGS__; } while (0); \
    do { const NODE *VAR = args_1; \
         const node_dispatcher_func_t VAR##_dispatcher = args_1_dispatcher; \
         __VA_ARGS__; } while (0)

static inline __attribute__((always_inline)) VALUE
EVAL_node_sum_2(CTX *c, NODE *n,
                NODE *args_0, node_dispatcher_func_t args_0_dispatcher,
                NODE *args_1, node_dispatcher_func_t args_1_dispatcher)
{
    /* === user body verbatim === */
    VALUE r = 0;
    FOR_EACH_NODE_ARG(arg,
        r += EVAL_ARG(c, arg)
    );
    return r;
    /* === end user body === */
}
#undef FOR_EACH_NODE_ARG
```

K = 3, 4, ..., MAX_INLINE_ARITY についても同様に、`#define` の中身と関数シグネチャだけを K に応じて変えて出力する。**user body は K+1 回そのままテキスト copy するのみ**。

#### Generic fallback

```c
#define FOR_EACH_NODE_ARG(VAR, ...) \
    for (int _i = 0; _i < cnt; _i++) { \
        const NODE *VAR = args[_i]; \
        const node_dispatcher_func_t VAR##_dispatcher = args[_i]->head.dispatcher; \
        __VA_ARGS__; \
    }

static VALUE
EVAL_node_sum_generic(CTX *c, NODE *n)
{
    const int cnt = n->u.sum.cnt;
    NODE *const *args = n->u.sum.args;
    /* === user body verbatim === */
    VALUE r = 0;
    FOR_EACH_NODE_ARG(arg,
        r += EVAL_ARG(c, arg)
    );
    return r;
    /* === end user body === */
}
#undef FOR_EACH_NODE_ARG
```

- generic 版では `FOR_EACH_NODE_ARG` がループに展開される。
- `arg->head.dispatcher` を都度ロードするので **インライン展開はされない**（その代わり実行時の gather 配列も memcpy も無い）。
- `MAX_INLINE_ARITY` を超える稀ケースだけ通る経路。

### 3.3 dispatcher テーブル

```c
static const node_dispatcher_func_t dispatcher_table_sum[] = {
    DISPATCH_node_sum_0,
    DISPATCH_node_sum_1,
    DISPATCH_node_sum_2,
    /* ... */
    DISPATCH_node_sum_MAX,
};
```

### 3.4 allocator

```c
NODE *
node_sum_alloc(int cnt, NODE *const *args)
{
    NODE *_n = /* sizeof(head) + sizeof(int) + cnt*sizeof(NODE*) で確保 */;
    _n->u.sum.cnt = cnt;
    memcpy(_n->u.sum.args, args, sizeof(NODE*) * cnt);   /* 構築時 1 回限り */
    _n->head.dispatcher = (cnt < DISPATCHER_TABLE_SUM_LEN)
        ? dispatcher_table_sum[cnt]
        : DISPATCH_node_sum_generic;
    return _n;
}
```

- `cnt` を見て **alloc 時に一発で正しい K の dispatcher を埋める**。実行時 swap は無い（profile-based swap は別レイヤ、§6 参照）。
- `memcpy` は alloc パス上の 1 回のみ。EVAL/DISPATCH（hot path）には memcpy も gather もない。
- ムーブ契約 (`args` の所有権を allocator が引き取る) も選択肢。Arena ベースのパーサなら memcpy も省ける。

### 3.5 EVAL/DISPATCH の hot path コスト

K-特化版:
```c
static VALUE DISPATCH_node_sum_3(CTX *c, NODE *n) {
    NODE *a0 = n->u.sum.args[0];
    NODE *a1 = n->u.sum.args[1];
    NODE *a2 = n->u.sum.args[2];
    return EVAL_node_sum_3(c, n,
        a0, a0->head.dispatcher,
        a1, a1->head.dispatcher,
        a2, a2->head.dispatcher);
}
```

K 個の `args[i]` 読みと `->head.dispatcher` 読みのみ。**通常の 2-子ノード DISPATCH と同形・同コスト**。memcpy/gather なし。

`always_inline` の連鎖により、`FOR_EACH_NODE_ARG` 展開中の `arg`/`arg_dispatcher` は const-local に named パラメータが入るだけなので、gcc は SCCP/コピー伝播でフルインライン展開する（実証済 — `/tmp/claude/inline_test/test20`）。

## 4. ASTroGen 側の実装規模

`lib/astrogen.rb` への追加:

| 場所 | やること |
|---|---|
| `Operand` クラス階層 | `VariadicNodeOperand` を 1 つ追加 (`NODE *foo[]` を検出) |
| `parse_operands` | 配列形宣言を variadic operand に振り分け |
| EVAL/DISPATCH 出力ループ | variadic を含む node なら K=0..MAX + generic を回して出す（テキスト連結のみ）|
| allocator 出力 | `cnt` 依存サイズの確保 + dispatcher テーブル引き |
| dumper / hash / specializer など | variadic フィールドを `cnt` 込みで再帰扱い |

**C パーサは一切要らない**。すべて Ruby 側のテキスト生成で完結する。

## 5. 原則チェック

| 原則 | 守られる？ |
|---|---|
| EVAL body は ASTroGen が解析・書き換えしない | ✓ — body は K+1 回 opaque text copy するのみ |
| `EVAL_ARG` マクロ未改造 | ✓ — `n##_dispatcher` のまま |
| node.def に arity 別宣言を並べない | ✓ — `node_sum` ひとつで完結 |
| パーサが arity を知らない | ✓ — `node_sum_alloc(cnt, args)` ひとつ |
| gcc でフルインライン展開 | ✓ — K-特化版は名前付き引数規約を満たす |
| C パーサを書かない（ASTroGen 流儀） | ✓ — テキスト連結と `#define`／`#undef` サンドイッチのみ |

## 6. 他の特化軸との関係

ASTro の特化軸は arity だけではない。本提案は **arity 軸の自動化** を提供するだけで、他の軸（型仮定 / プロファイルに基づく dispatcher swap 等）と直交する:

| 軸 | きっかけ | やること |
|---|---|---|
| arity-based 選択（本提案） | alloc 時、`cnt` で決定 | 初期 dispatcher を K 番にする |
| profile-based dispatcher swap | 実行時、ホット検出 | dispatcher を更に特化版に差し替え |

profile 軸との階層化:`DISPATCH_node_sum_3` を更に型仮定で specialized して `DISPATCH_node_sum_3_int_specialized` のように積めるが、**dispatcher 数は両軸の積になる**ので注意 (§7)。

## 7. トレードオフ

### 7.1 dispatcher 数の線形増加

- variadic ノード型 1 つあたり K+1 個の dispatcher/EVAL を出す。
- variadic ノード型が M 種なら **M × (K+1)** で線形増加。combinatorial にはならない。
- 手書きで `call_K` を並べる場合と **オーダーは同じ**。手書きの煩雑さを ASTroGen 側に追いやっただけ。

### 7.2 profile 軸との掛け算

- profile-based 特化を併用すると `M × (K+1) × P` で増える。
- 緩和策:
  - `MAX_INLINE_ARITY` を保守的にする（実測で 95% カバーする値、典型的には 4〜8）
  - profile 軸を **on-demand**（code store による lazy 生成）に倒す
  - `MAX_INLINE_ARITY` をノード型ごとに別設定する (`node_array_lit` は 2 で十分、`node_call` は 8 必要、等)

### 7.3 マクロ規約の制約

- `FOR_EACH_NODE_ARG(VAR, ...)` の `__VA_ARGS__` 部分には **トップレベルのコンマ演算子** を書けない（マクロパラメータ区切りと衝突）。
- 実用上は単一式 (`r += EVAL_ARG(c, arg)`) で十分。複数式が必要なら `({...})` GCC 拡張、または `do { ... } while (0)` で囲む。
- ループ変数名は通常の C 識別子（`arg` 等）。EVAL_ARG の `n##_dispatcher` 規約と整合する形しか書けない。

## 8. 結論

「ノードを arity 別に分けない」「ASTroGen の C パーサ非保有原則を守る」「ホット部位は gcc がフルインライン展開する」を **同時に満たす** 唯一の現実解が、

- node.def 側で `NODE *args[]` + `FOR_EACH_NODE_ARG` マクロ規約
- ASTroGen 側で K-特化版を `#define`/`#undef` サンドイッチで出力（body は opaque text copy）
- alloc 時に `cnt` で dispatcher テーブルを引いて初期 dispatcher を確定
- 大 arity / 冷たい部位は generic 版（インライン展開を諦める代わりに正しく動く）

という構成。

増えるのは **「ASTroGen が裏で出す K+1 個の特化関数」だけ**であり、user-visible なノード型・パーサ API・EVAL body のいずれも単一のまま保たれる。コスト面の主リスクは profile 軸との積による dispatcher 数増加であり、`MAX_INLINE_ARITY` の調整と profile 軸の lazy 化で緩和する。
