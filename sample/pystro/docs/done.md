# done.md — pystro 実装済み

ASTro 上の Python サブセット。本書は **動く言語機能** を一覧する。
未実装は [todo.md](./todo.md)、ランタイム解説は [runtime.md](./runtime.md)、
ベンチ結果は [perf.md](./perf.md)。

## テストスイート

```
$ make test
... 21 tests OK ...
passed=21  failed=0  total=21
```

interpreter / AOT cached の両方で 21/21 pass。

## 言語機能

### リテラル
- 整数: 62-bit fixnum + GMP `mpz_t` bignum
- 浮動小数: **inline flonum** (CRuby 流 3-bit rotate) + heap fallback
- 文字列 `"..."` / `'...'`、エスケープ + slice の buffer 共有
- f-string `f"x={expr}"` + format spec `{x:.2f}` `{x:>10}` `{x:05d}` 等
- リスト `[1, 2, 3]`、内包表記 `[x*x for x in range(10) if x % 2 == 0]`
- タプル `(1, 2, 3)`、空 `()`、単要素 `(x,)`
- 辞書 `{"k": v}`、内包表記 `{k: v for ...}`
- **集合 `{1, 2, 3}`**、内包表記 `{x*x for x in xs}`、`set()` 組み込み
- `True` / `False` / `None`
- `0xFF` / `0b101` / `0o17`、`1_000_000`

### 演算子
- 算術 `+ - * / // % **` (整数 fixnum fast path、float inline、mixed → py_make_float)
- 単項 `+x` / `-x` / `~x`
- 比較 `< <= > >= == !=`、**比較連鎖** (`a < b < c`)、`is` / `is not`
- 論理 `and` / `or` / `not` (short-circuit)
- ビット演算 `& | ^ ~ << >>`
- メンバーシップ `in` / `not in` (list / tuple / dict / set / str / range)
- 拡張代入 `+= -= *= /= //= %= **= &= |= ^= <<= >>=`
- 条件式 `x if cond else y`
- **walrus `(x := expr)`**

### 文
- `pass` / `break` / `continue`
- `if / elif / else`
- `while ... else: ...` (else は break しないとき実行)
- `for x in iter: ... else: ...` 同上
- `def f(p, q=default, *args, kwonly=N, **kwargs):`
- `return [expr]`
- `lambda x: expr`、`lambda x, y=1: ...`
- `class Name(Base):` (継承、`__init__`、メソッド、`self.x`)
- **`@decorator`** (関数 / クラス、複数段、closure 込み)
- `try / except [E [as e]] / else / finally / raise [expr]` / `raise`
- **`with EXPR as NAME: ...`** (context manager — `__enter__` / `__exit__`)
- `yield expr` / `yield from iter` (eager、結果 list)
- `global x` / `nonlocal x`
- `x = e`、`a.b = e`、`a[i] = e`
- **slice 代入**: `a[i:j] = list` (step==1 で伸縮、step!=1 は同じ長さ)
- **多重代入**: `a = b = expr` (RHS は temp で 1 回だけ評価)
- 多重 unpack: `a, b, c = (1, 2, 3)`、`a, b = b, a`
- 式文

### スコープ
- トップレベル代入 → global
- 関数内代入 → local (parser の suite pre-scan で判定、ネスト def はスキップ)
- **closure capture**: ネスト def が外側 local を読める (lref_up depth 経由)
- `nonlocal x` で外側 local への代入
- `global x` で local シャドウ抑制

### 組み込み関数 (37 個)
| グループ | 名前 |
|---|---|
| 出力 / 変換 | `print` / `str` / `repr` / `int` / `float` / `bool` / `format` |
| 数値 | `abs` / `divmod` / `round` / `pow` (3-arg modular) |
| 反復 | `range` / `len` / `enumerate` / `zip` / `reversed` / `sum` / `min` / `max` / `sorted` / `all` / `any` / `map` / `filter` / `iter` / `next` |
| 構築 | `list` / `tuple` / `dict` / `set` |
| 型 | `type` / `isinstance` / `hash` |
| 文字 | `chr` / `ord` / `hex` / `bin` |
| 入出力 | `input` |
| descriptor | `staticmethod` / `classmethod` / `property` |

### 組み込み例外クラス
`Exception` を頂点として `TypeError / ValueError / NameError / IndexError /
KeyError / ZeroDivisionError / AttributeError / RuntimeError / StopIteration`。
`raise X("msg")` は `e.args = ("msg",)` と `e.message = "msg"` をセット。

### 文字列メソッド
`split / join / upper / lower / strip / startswith / endswith / find /
replace / count`

### リストメソッド
`append / pop / extend / insert / index / reverse / sort`

### dict メソッド
`get / keys / values / items / pop`

### set メソッド
`add / discard / remove / pop / union / intersection / difference`

### dunder methods
`__init__` (constructor)、`__add__ / __sub__ / __mul__ / __radd__`、
`__eq__` / `__lt__`、`__repr__` / `__str__`、`__len__`、`__getitem__` /
`__setitem__`、`__enter__` / `__exit__` (with)

### スライス
- `s[i]`、`s[i:j]`、`s[i:j:k]`、負インデックス、step、`s[::-1]` 反転
- str / list / tuple に対応 (slice 結果は元と同型)
- list には slice 代入も対応

### 実行モード
| フラグ | 動作 |
|---|---|
| (なし)             | interpreter (code_store/all.so があれば dlopen 利用) |
| `-c`               | AOT-bake してから実行 |
| `--aot-compile`    | AOT-bake のみ |
| `--no-compile`     | code_store 不使用 (純 interpreter) |
| `-e <code>`        | コマンドライン文字列を実行 |

## 性能 (vs. CPython 3.12)

`make bench` の結果。詳細は [`perf.md`](./perf.md)。

| bench (~1s on python3) | python3 | pystro AOT | pystro/python3 |
|---|---:|---:|---:|
| while_loop 10M | 0.95 s | 0.05 s | **18×** |
| for_range 15M | 1.01 s | 0.09 s | **11×** |
| list 7M append+sum | 0.88 s | 0.19 s | **4.7×** |
| fib(35) | 1.18 s | 0.62 s | **1.9×** |
| recursive (tak) | 4.06 s | 2.49 s | **1.6×** |
| mandel | 0.70 s | 0.63 s | **1.1×** |
| nqueens | 0.69 s | 0.62 s | **1.1×** |
| string 2M split | 0.60 s | 0.50 s | **1.2×** |
| dict 3M put+get | 0.77 s | 0.82 s | 0.94× |

8 / 9 の bench で python3 を上回る。
