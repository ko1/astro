# pystro 言語仕様

`pystro` は **Python 3 のサブセット**インタプリタ。CPython 3.12 と同じ
意味論を目指し、9 ベンチ中 8 ベンチで CPython を上回る範囲をカバーする。
完全な Python 仕様は [Python Language Reference](https://docs.python.org/3/reference/)
を参照。本書は pystro で動く範囲を端的に示す。

## 値の種類

| 型 | 例 | 備考 |
|---|---|---|
| `int` | `42` `-3` `2**100` | GMP による任意精度 |
| `float` | `3.14` `1e10` | IEEE-754 double。inline-flonum 最適化あり |
| `bool` | `True` `False` | `int` のサブクラス (慣習通り) |
| `str` | `"hello"` `'world'` | immutable |
| `bytes` | `b"data"` | mutable not — `bytearray` が mutable |
| `list` | `[1, 2, 3]` | mutable |
| `tuple` | `(1, 2, 3)` `(x,)` | immutable |
| `dict` | `{"a": 1, "b": 2}` | 挿入順保持 |
| `set` | `{1, 2, 3}` `set()` | mutable |
| `frozenset` | `frozenset({1, 2})` | immutable |
| `range` | `range(10)` | 遅延整数列 |
| `function` / `class` | (定義したもの) | 第一級 |
| `None` | `None` | 値なし |

**真偽判定**: `False` `None` `0` `0.0` `""` `[]` `()` `{}` `set()` が偽。
それ以外は真。

## リテラル

```python
42  -3  0xff  0b1010  1_000_000     # 整数 (アンダースコア区切り可)
3.14  1e10  0.5  6.022e23           # 浮動小数
"hello"  'world'  """multi
line"""                              # 文字列
b"bytes"  rb"raw"                    # バイト列・raw
f"x={x:.2f}"                          # f-string (フォーマット指定子つき)
True  False  None
[1, 2, 3]
(1, 2, 3)   ()   (1,)                 # tuple (1 要素はカンマ必須)
{1, 2, 3}                             # set
{}   {"a": 1}                         # dict (空 dict)
{x for x in range(5)}                 # set 内包
[x**2 for x in range(5)]              # list 内包
{k: v for k, v in items}              # dict 内包
```

## 変数

```python
x = 1
y, z = 2, 3              # 多重代入
a, *rest = [1, 2, 3, 4]  # 残りを rest にまとめる (= [2,3,4])
a = b = 0                # 連鎖代入
```

スコープ: 関数本体内が 1 スコープ (ブロックスコープなし)。`global` /
`nonlocal` 宣言で外側のスコープを書き換え可能:

```python
counter = 0
def inc():
    global counter
    counter += 1

def make_counter():
    n = 0
    def step():
        nonlocal n
        n += 1
        return n
    return step
```

## 演算子

| カテゴリ | 演算子 |
|---|---|
| 算術 | `+ - * / // % ** ` (`/` は実数除算、`//` は整数除算、`**` は冪) |
| 比較 | `< > <= >= == !=` (連鎖可: `1 < x < 10`) |
| 同一性 | `is is not` |
| 包含 | `in not in` |
| 論理 | `and or not` (短絡; 値を返す) |
| ビット | `& | ^ ~ << >>` |
| 代入 | `= += -= *= /= //= %= **= &= |= ^= <<= >>=` |
| walrus | `:=` (式の中で代入: `if (n := len(s)) > 10:`) |
| 添字 | `a[i]` `a[i:j]` `a[i:j:k]` |
| 三項 | `a if cond else b` |

## 制御構文

```python
if cond:
    ...
elif other:
    ...
else:
    ...

while cond:
    ...
else:                  # break しなかった場合のみ実行
    ...

for x in iterable:
    ...
else:
    ...

break
continue

match value:           # 構造的パターンマッチ
    case 0:
        ...
    case [x, y]:       # リストの 2 要素分解
        ...
    case {"k": v}:     # dict の "k" を v に
        ...
    case _:            # ワイルドカード
        ...
```

`while/for ... else` の `else` は **break で抜けなかった場合のみ** 実行される
(Python 独特の構文)。

## 関数

```python
def add(a, b):
    return a + b

def greet(name="world"):                      # デフォルト引数
    return f"hi {name}"

def variadic(*args, **kwargs):                 # 可変長 + キーワード可変長
    return len(args), kwargs

def kwonly(a, *, b=10):                        # * 以降はキーワード専用
    return a + b

def annotated(x: int, y: int = 0) -> int:      # 型注釈 (実行時無視)
    return x + y

# 呼出側
add(1, 2)
greet(name="alice")
greet("bob")
variadic(1, 2, 3, x=10, y=20)
add(*[1, 2])               # spread
greet(**{"name": "ann"})    # dict spread

# 無名関数 (制限あり: 単一式のみ)
sqr = lambda x: x * x
```

`return` 無しの関数は `None` を返す。

### Decorator

```python
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
    return fib(n-1) + fib(n-2)
```

`@dec` は `f = dec(f)` と等価。引数を取る decorator も同じ仕組みで動く。

## クラス

```python
class Point:
    def __init__(self, x, y):
        self.x = x
        self.y = y

    def __add__(self, other):
        return Point(self.x + other.x, self.y + other.y)

    def __repr__(self):
        return f"Point({self.x}, {self.y})"

    def __eq__(self, other):
        return self.x == other.x and self.y == other.y

p = Point(1, 2)
q = Point(10, 20)
print(p + q)         # Point(11, 22)
```

### 継承 + super

```python
class Animal:
    def __init__(self, name):
        self.name = name
    def greet(self):
        return f"hi {self.name}"

class Dog(Animal):
    def __init__(self, name, breed):
        super().__init__(name)
        self.breed = breed
    def greet(self):
        return super().greet() + ", woof!"
```

多重継承は C3 線形化 (Python 標準) で解決される。

### dunder メソッド

特別なメソッド名で演算子・組込関数の挙動を実装:

| dunder | 用途 |
|---|---|
| `__init__` | コンストラクタ |
| `__repr__` `__str__` | 文字列化 |
| `__eq__` `__ne__` `__lt__` `__le__` `__gt__` `__ge__` | 比較 |
| `__add__` `__sub__` `__mul__` `__truediv__` `__floordiv__` `__mod__` `__pow__` | 算術 |
| `__getitem__` `__setitem__` `__delitem__` | 添字 |
| `__len__` | `len(x)` |
| `__iter__` `__next__` | イテレーション |
| `__contains__` | `in` |
| `__call__` | `obj(args)` |
| `__hash__` | hash() |
| `__enter__` `__exit__` | `with` 文 |

### staticmethod / classmethod / property

```python
class C:
    @staticmethod
    def greeting(): return "hi"            # self も cls も無し

    @classmethod
    def create(cls): return cls()           # cls 渡し

    @property
    def x(self): return self._x

    @x.setter
    def x(self, v): self._x = v
```

## 例外

```python
try:
    risky()
except ValueError as e:
    print(f"value error: {e}")
except (TypeError, RuntimeError) as e:
    print("other:", e)
except Exception:
    print("anything")
else:
    print("no exception")     # 例外が出なかったときだけ
finally:
    cleanup()                   # 必ず

raise ValueError("bad input")
raise                            # 直前の except 内で再発生
```

## with 文 (コンテキストマネージャ)

```python
with open("file.txt") as f:
    data = f.read()
# ここに来る時点で f.close() 済み

with lock:                       # __enter__ / __exit__ を持つ任意のオブジェクト
    critical_section()

with a() as x, b() as y:         # 複数同時
    ...
```

## ジェネレータ

`yield` を含む関数は generator function になり、呼び出すと iterator を返す:

```python
def primes(n):
    sieve = [True] * (n + 1)
    for i in range(2, n + 1):
        if sieve[i]:
            yield i                       # 1 個ずつ返す
            for j in range(i*i, n+1, i):
                sieve[j] = False

for p in primes(30):
    print(p)
```

`yield from` でデリゲート、`gen.send(v)` `gen.throw(e)` `gen.close()` も対応。

## 内包表記

```python
[x**2 for x in range(10) if x % 2 == 0]      # list 内包
{x: x**2 for x in range(5)}                   # dict 内包
{x for x in items}                             # set 内包
(x*2 for x in items)                           # generator 式
```

## f-string

```python
name = "alice"
age = 30
f"name={name}, age={age}"
f"{age:.2f}"                  # フォーマット指定子: 小数点以下 2 桁
f"{age:>10}"                  # 右寄せ幅 10
f"{name!r}"                   # repr() 経由 ('alice')
f"{name=}"                    # name='alice' (Python 3.8+)
```

## スライス・添字

```python
a = [0, 1, 2, 3, 4]
a[1:3]                # [1, 2]
a[:2]                 # [0, 1]
a[2:]                 # [2, 3, 4]
a[::2]                # [0, 2, 4]
a[::-1]               # [4, 3, 2, 1, 0] (逆順)
a[1:4] = [10, 20]     # スライス代入で範囲置換
del a[1:3]            # スライス削除
```

文字列・タプル・bytes も同様にスライス可。

## 組み込み関数 (主なもの)

`print input len range enumerate zip map filter sorted reversed
sum min max abs round divmod pow
type isinstance issubclass id hash repr str int float bool list tuple dict set
ord chr bin oct hex
all any iter next callable getattr setattr hasattr delattr
open` (簡易ファイル I/O)

## 標準ライブラリ (実装済みの主なもの)

`sys` — `argv` `exit` `stdin/stdout/stderr` `path`

`math` — `pi e sqrt floor ceil sin cos tan log log2 log10 exp pow`

`os` — `getenv getcwd path.join path.exists`

`json` — `loads dumps`

`functools` — `reduce` `lru_cache`

`itertools` — `chain count cycle repeat product`

`collections` — `Counter defaultdict deque namedtuple`

`hashlib` — `md5 sha256` (簡易)

`random` — `random randint choice shuffle`

`re` — 正規表現 (簡易)

## 例

```python
# クラス + dunder 全部入り
class Vec:
    def __init__(self, x, y): self.x, self.y = x, y
    def __add__(self, o): return Vec(self.x + o.x, self.y + o.y)
    def __mul__(self, k): return Vec(self.x * k, self.y * k)
    def __eq__(self, o): return (self.x, self.y) == (o.x, o.y)
    def __repr__(self): return f"Vec({self.x}, {self.y})"

v = Vec(1, 2) + Vec(3, 4)
print(v * 2)              # Vec(8, 12)

# generator + 内包
def primes(n):
    return [p for p in range(2, n+1) if all(p % d for d in range(2, int(p**0.5)+1))]
print(primes(30))

# decorator + closure
def timed(f):
    import time
    def w(*a, **k):
        t = time.time()
        r = f(*a, **k)
        print(f"{f.__name__}: {time.time()-t:.3f}s")
        return r
    return w

@timed
def slow():
    return sum(i*i for i in range(10_000))

slow()
```

## 持たない / 制限

- `async` / `await` の真の非同期セマンティクス (実装あれば限定的)
- `metaclass` の本格利用 (シンプルなものは可)
- 型アノテーションの実行時利用 (`__annotations__`)
- `__slots__` による属性制限
- C extensions / `ctypes`
- スレッド / `multiprocessing`
- 完全な `re` モジュール (簡易のみ)

詳細: [`done.md`](done.md) / [`todo.md`](todo.md)。
