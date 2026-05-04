# pystro — Python subset on ASTro

ASTro フレームワーク上に乗せた **Python サブセット** インタプリタ。
クラス + 多 dunder、例外 (try/except/else/finally)、bignum (GMP) +
inline flonum、リスト/タプル/辞書/集合 + 内包表記、`for/in/break/
continue/else`、`with`、closure + decorator、generator (yield、eager)、
キーワード引数 + `*args/**kwargs`、staticmethod/classmethod/property、
walrus `:=`、多重代入、slice 代入、f-string + format spec、まで実装し、
`./pystro -c` で AOT 特化バイナリ (SD 群の `dlopen`) も使える。

実装の詳細は [`docs/runtime.md`](./docs/runtime.md)、
動く範囲は [`docs/done.md`](./docs/done.md)、
未実装と性能 todo は [`docs/todo.md`](./docs/todo.md)、
ベンチと最適化の歴史は [`docs/perf.md`](./docs/perf.md)。
ASTro 本体は [`../../docs/idea.md`](../../docs/idea.md)。

## 試す

```sh
make            # pystro バイナリ
make test       # test/*.py 21 件 (interp / AOT 両方が pass)
make bench      # bench/*.py を python3 / interp / AOT cached の 3 列で比較

./pystro test/03_func.py            # ファイル実行
./pystro -e 'print(1 + 2)'          # one-liner
./pystro -c test/03_func.py         # AOT-bake してから実行
./pystro --aot-compile foo.py       # AOT-bake だけして exit
./pystro --no-compile test/...      # code_store 一切無視
./pystro --dump-ast test/...        # AST を dump
```

サンプル:

```python
# 内包表記、closure、decorator、generator、dunder 全部入り
def memoize(f):
    cache = {}
    def wrapped(*args):
        if args not in cache:
            cache[args] = f(*args)
        return cache[args]
    return wrapped

@memoize
def fib(n):
    if n < 2: return n
    return fib(n - 1) + fib(n - 2)

print([fib(n) for n in range(20)])

class Vec:
    def __init__(self, x, y):
        self.x, self.y = x, y
    def __add__(self, other):
        return Vec(self.x + other.x, self.y + other.y)
    def __repr__(self):
        return f"Vec({self.x}, {self.y})"

print(Vec(1, 2) + Vec(10, 20))     # Vec(11, 22)

def primes(n):
    sieve = [True] * (n + 1)
    sieve[0] = sieve[1] = False
    for i in range(2, n + 1):
        if sieve[i]:
            yield i
            for j in range(i*i, n + 1, i):
                sieve[j] = False

print([p for p in primes(50)])     # [2, 3, 5, 7, ...]
```

## ベンチマーク

`make bench` で `bench/*.py` (各 ~1 秒スケール on python3) を計測。
詳細は [`docs/perf.md`](./docs/perf.md)。

| bench (~1s on python3) | python3 | pystro AOT | **倍率** |
|---|---:|---:|---:|
| `while_loop` (10M, augassign) | 0.95 s | 0.05 s | **18× 速い** |
| `for_range` (15M sum) | 1.01 s | 0.09 s | **11× 速い** |
| `list_bench` (7M append+sum) | 0.88 s | 0.19 s | **4.7× 速い** |
| `fib(35)` (再帰) | 1.18 s | 0.62 s | **1.9× 速い** |
| `recursive` (tak) | 4.06 s | 2.49 s | **1.6× 速い** |
| `string_bench` (2M split) | 0.60 s | 0.50 s | **1.2× 速い** |
| `mandel` (float-heavy) | 0.70 s | 0.63 s | **1.1× 速い** |
| `nqueens` (recursion + list) | 0.69 s | 0.62 s | **1.1× 速い** |
| `dict_bench` (3M put+get) | 0.77 s | 0.82 s | 0.94× |

9 ベンチ中 **8 つで python3 を上回る**。CPython の C 実装 dict は強敵。
最大は `while_loop` の **18×**。

主な最適化 — 詳しくは [`docs/perf.md`](./docs/perf.md):

- §1 `gref_cache @ref`: global lookup の strcmp 線形走査を排除
- §2 `globals_serial` を構造変化のみで bump (値更新では bump しない)
- §3 `node_for_global` 内蔵キャッシュ (for-loop の iter target 代入)
- §4 `method_cache @ref`: bound-method allocation を消去
- §5 `py_apply` を `node.h` に static inline (PLT hop 排除)
- §6 leaf func の alloca フレーム (parser が `has_nested_def` で判定)
- §7 dict identity-equal fast path
- §8 string slice の buffer 共有 + 小サイズ pyobj alloc
- §9 inline flonum (CRuby 流 3-bit rotate) + 算術ノードに float-float fast path
- §10 node_eq / py_eq の fixnum fast path (GMP 経路を回避)

> AOT bake で `astro_cs_build: make failed (exit 512)` のようなエラーが
> 出る環境では `CCACHE_DISABLE=1` を環境変数で渡すと回避できる
> (ccache の cache ディレクトリへの書込が sandbox 等で塞がれてるケース)。

## ファイル構成

```
sample/pystro/
├── README.md         この文書
├── docs/
│   ├── runtime.md    実装詳細
│   ├── done.md       実装済み機能 (詳細リスト)
│   ├── todo.md       未実装機能 + 性能 todo
│   └── perf.md       ベンチ + 最適化の歴史
├── node.def          AST ノード定義 (~80 種)
├── pystro_gen.rb     ASTroGen 拡張 (`@ref` operands を扱う)
├── context.h         VALUE / pyobj / CTX / 公開 API
├── node.h            NodeHead + EVAL + py_apply (inline) マクロ
├── node.c            ランタイム配線
├── runtime.c         heap / globals / apply / display / numeric / dict /
│                     list / set / iter / attr / method / try / 37 builtins
├── lexer.c           インデント追跡 + f-string + 全演算子トークナイザ
├── parser.c          recursive-descent parser (内包表記 / kwargs / closure /
│                     decorator / walrus / slice 代入 / 多重代入 込み)
├── main.c            driver
├── Makefile          build / test / bench / clean
├── test/             *.py + .expected (21 件)
├── bench/            *.py (~1 秒スケール、python3 比較用、9 件)
└── code_store/       AOT 生成物 (gitignore)
```

## 制限 (詳細は docs/todo.md)

- **モジュール / import**: parser はスキップ済み (no-op stub)
- **真の generator**: 現状 eager。無限 generator は不可
- **多重継承の MRO**: 現状は base 1 段のみ
- **`*args` / `**dict`** の呼び出し側 unpacking
- **bytes / bytearray / frozenset / match 文**
- **`def f(x: int) -> int:`** の type hint
- **比較連鎖の中央オペランド単一評価保証**

GC は Boehm-Demers-Weiser、bignum は GMP。
