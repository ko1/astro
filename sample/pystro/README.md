# pystro — Python subset on ASTro

ASTro フレームワーク上に乗せた **Python サブセット** インタプリタ。
クラス、例外、bignum (GMP)、リスト・タプル・dict、`for/in`、f-string、
lambda、デフォルト引数、tuple unpack まで実装し、`./pystro -c` で
AOT 特化バイナリ (SD 群の `dlopen`) も使える。

実装の詳細は [`docs/runtime.md`](./docs/runtime.md)、
動く範囲は [`docs/done.md`](./docs/done.md)、
未実装と性能 todo は [`docs/todo.md`](./docs/todo.md)、
ベンチと最適化の歴史は [`docs/perf.md`](./docs/perf.md)。
ASTro 本体は [`../../docs/idea.md`](../../docs/idea.md)。

## 試す

```sh
make            # pystro バイナリ
make test       # test/*.py 11 件 (interp / AOT 両方が pass)
make bench      # bench/*.py を python3 / interp / AOT cached の 3 列で best-of-3 比較

./pystro test/03_func.py            # ファイル実行 (interpreter)
./pystro -e 'print(1 + 2)'          # one-liner
./pystro -c test/03_func.py         # AOT-bake してから実行
./pystro --aot-compile foo.py       # AOT-bake だけして exit
./pystro --no-compile test/...      # code_store 一切無視
./pystro --dump-ast test/...        # AST を dump
```

サンプル:

```python
def fact(n):
    r = 1
    for i in range(2, n + 1):
        r *= i
    return r

print(fact(50))   # GMP bignum: 30414093201713378043612608166064768844377641568960512000000000000

class Point:
    def __init__(self, x, y):
        self.x, self.y = x, y
    def manhattan(self):
        return abs(self.x) + abs(self.y)

p = Point(3, -4)
print(f"|p| = {p.manhattan()}")    # |p| = 7

def safe_div(a, b):
    try:
        return a / b
    except ZeroDivisionError as e:
        return None

print(safe_div(10, 0))             # None
```

## ベンチマーク

`make bench` で `bench/*.py` (各 ~1 秒スケール on python3) を計測。
詳細は [`docs/perf.md`](./docs/perf.md)。

| bench (~1s on python3) | python3 | pystro interp | pystro AOT cached | **倍率** |
|---|---:|---:|---:|---:|
| `fib(35)` (再帰) | 1.21 s | 0.64 s | 0.65 s | **1.86× 速い** |
| `while_loop` (10M, augassign) | 0.92 s | 0.18 s | 0.05 s | **18× 速い** |
| `for_range` (15M sum) | 1.00 s | 0.15 s | 0.08 s | **12.5× 速い** |
| `list_bench` (7M append+sum) | 0.93 s | 0.22 s | 0.19 s | **4.9× 速い** |
| `string_bench` (2M split) | 0.59 s | 0.51 s | 0.48 s | **1.23× 速い** |
| `dict_bench` (3M put+get) | 0.78 s | 0.87 s | 0.75 s | **1.04× 速い** |

すべて pystro の勝ち。最大は `while_loop` で **18×**、最小は `dict_bench`
で 1.04× (CPython の dict は強敵)。

主な高速化の施策 — 詳しくは [`docs/perf.md`](./docs/perf.md):

- §1 `gref_cache @ref`: global lookup の strcmp 線形走査を排除
- §2 `globals_serial` を構造変化のみで bump (値更新では bump しない)
- §3 `node_for_global` 内蔵キャッシュ (for-loop の iter target 代入)
- §4 `method_cache @ref`: bound-method allocation を消去
- §5 `py_apply` を `node.h` に static inline (PLT hop 排除)
- §6 leaf func の alloca フレーム (parser が `has_nested_def` で判定)
- §7 dict identity-equal fast path
- §8 string slice の buffer 共有 + 小サイズ pyobj alloc

> AOT bake で `astro_cs_build: make failed (exit 512)` のようなエラーが
> 出る環境では `CCACHE_DISABLE=1` を環境変数で渡すと回避できる
> (ccache の cache ディレクトリへの書込が sandbox 等で塞がれてるケース)。

## ファイル構成

```
sample/pystro/
├── README.md         この文書
├── docs/
│   ├── runtime.md    実装詳細 (VALUE / CTX / フレーム / iter / try)
│   ├── done.md       実装済み機能
│   ├── todo.md       未実装機能 + 性能 todo
│   └── perf.md       ベンチ + 最適化の歴史
├── node.def          AST ノード定義 (~60 種)
├── pystro_gen.rb     ASTroGen 拡張 (`@ref` operands を扱う)
├── context.h         VALUE / pyobj / CTX / 公開 API
├── node.h            NodeHead + EVAL + py_apply (inline) マクロ
├── node.c            ランタイム配線 (allocate / OPTIMIZE / 生成ファイル取り込み)
├── runtime.c         heap / globals / apply_slow / display / numeric / dict /
│                     list / iter / attr / method / try / builtins
├── lexer.c           インデント追跡 + f-string + 全演算子トークナイザ
├── parser.c          recursive-descent parser、scope pre-scan、leaf 判定、
│                     f-string パース、tuple unpack
├── main.c            driver
├── Makefile          build / test / bench / clean
├── test/             *.py + .expected (11 件)
├── bench/            *.py (~1 秒スケール、python3 比較用)
└── code_store/       AOT 生成物 (gitignore)
```

## ノード一覧 (抜粋)

| グループ | ノード |
|---|---|
| 定数  | `const_int / const_int64 / const_bignum / const_float / const_str / const_none / const_true / const_false` |
| コンテナ literal | `make_list / make_tuple / make_dict` |
| 変数  | `lref / lset / gref(@ref) / gset(@ref)` |
| 単項  | `neg / not / bit_inv` |
| 算術  | `add / sub / mul / truediv / floordiv / mod / pow` (fixnum fast path inline) |
| 比較  | `lt / le / gt / ge / eq / ne / is / is_not / in / not_in` |
| 論理  | `and / or` (short-circuit) |
| ビット | `bit_and / bit_or / bit_xor / lshift / rshift` |
| 添字 / 属性 | `subscript_get / subscript_set / slice / attr_get / attr_set` |
| 制御  | `if / while / for_local / for_global(@ref) / seq / nop / return / break / continue / try / raise / raise_bare` |
| 関数  | `def / lambda / class / call_0..3 / call_n / method_0..2(@ref) / method_n(@ref) / unpack_assign` |

`@ref` 印は内部 inline cache が付くノード。

## 制限 (詳細は docs/todo.md)

- ネスト def の closure capture (parser 側のみ未対応)
- `*args` / `**kwargs` / キーワード引数
- 内包表記 / generator / `yield`
- `with` / `else` 節 (for/try)
- 装飾子 `@dec`
- 多重継承 / dunder メソッド連動 / `super()`
- `import` (parser はスキップ、no-op)
- f-string format spec (`{x:.2f}`)
- 比較連鎖の中央オペランド単一評価保証
