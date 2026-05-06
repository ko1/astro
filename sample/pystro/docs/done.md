# done.md — pystro 実装済み

ASTro 上の Python サブセット。本書は **動く言語機能** を一覧する。
未実装は [todo.md](./todo.md)、ランタイム解説は [runtime.md](./runtime.md)、
ベンチ結果は [perf.md](./perf.md)。

## テストスイート

```
$ make test
... 56 tests OK ...
passed=56  failed=0  total=56
```

interpreter / AOT cached の両方で 56/56 pass。
うち test 45〜55 は CPython の `Lib/test/test_*.py` を pystro 用に adapt した
unittest 形式 (TestCase / assertEqual / assertRaises / main(globals()))。

## 言語機能

### リテラル
- 整数: 62-bit fixnum + GMP `mpz_t` bignum
- 浮動小数: **inline flonum** (CRuby 流 3-bit rotate) + heap fallback
- 文字列 `"..."` / `'...'`、エスケープ + slice の buffer 共有
- エスケープ: `\n / \t / \r / \\ / \' / \" / \0 / \b / \f / \a / \v / \xHH`
- f-string `f"x={expr}"` + format spec `{x:.2f}` `{x:>10}` `{x:05d}` 等、**conversion `!r` / `!s` / `!a`**、**debug `{x=}`**
- `"%d %s" % args` 系 % 演算子フォーマット
- `"{} {}".format(a, b)` (positional / 数値インデックス + format spec)
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
- `def f(p, q=default, *args, kwonly=N, **kwargs):` + 呼び出し側 `f(*list, **dict)` 展開
- 型アノテーション `def f(x: int, y: str = "a") -> int:` / `n: int = 5` (parse して破棄)
- `return [expr]`
- `lambda x: expr`、`lambda x, y=1: ...`
- `class Name(Base, ...):` (**多重継承**、`__init__`、メソッド、`self.x`)
- **`@decorator`** (関数 / クラス、複数段、closure 込み、`@staticmethod` / `@classmethod` / `@property`)
- `try / except [E [as e]] / else / finally / raise [expr]` / `raise` / **`raise X from Y`**
- **`with EXPR as NAME [, EXPR as NAME]*: ...`** (context manager — `__enter__` / `__exit__`、複数も可)
- `yield expr` / `yield from iter` (eager、結果 list)
- `global x` / `nonlocal x`
- `x = e`、`a.b = e`、`a[i] = e`
- **slice 代入**: `a[i:j] = list` (step==1 で伸縮、step!=1 は同じ長さ)
- **多重代入**: `a = b = expr` (RHS は temp で 1 回だけ評価)
- 多重 unpack: `a, b, c = (1, 2, 3)`、`a, b = b, a`、**starred target `a, *rest = ...`** / `*pre, b = ...` / `*all, = ...`
- `assert cond [, msg]` (失敗で AssertionError)
- `del a[i]` (dict / list / set からの要素削除)
- 式文

### スコープ
- トップレベル代入 → global
- 関数内代入 → local (parser の suite pre-scan で判定、ネスト def はスキップ)
- **closure capture**: ネスト def が外側 local を読める (lref_up depth 経由)
- `nonlocal x` で外側 local への代入
- `global x` で local シャドウ抑制

### 組み込み関数
| グループ | 名前 |
|---|---|
| 出力 / 変換 | `print` / `str` / `repr` / `ascii` / `int(s, base)` / `float` / `bool` / `format` |
| 数値 | `abs` / `divmod` / `round` (banker's) / `pow` (3-arg modular) |
| 反復 | `range` / `len` / `enumerate` (start) / `zip` / `reversed` / `sum` / `min` (key, default) / `max` (key, default) / `sorted` (key, reverse) / `all` / `any` / `map` / `filter` / `iter` / `next` |
| 構築 | `list` / `tuple` / `dict` / `set` / `frozenset` / `bytes` / `bytearray` |
| 型 | `type` / `isinstance` (built-in types + tuple of classes) / `issubclass` / `hash` / `id` / `callable` |
| 内省 | `dir` / `globals` / `locals` / `vars` |
| 属性 | `hasattr` / `getattr (default)` / `setattr` / `delattr` |
| 文字 | `chr` / `ord` / `hex` / `bin` |
| 入出力 | `input` / `open` (`with open(...)` 対応) |
| 動的実行 | `eval` / `exec` |
| descriptor | `staticmethod` / `classmethod` / `property` |

### 組み込み例外クラス
`Exception` を頂点として `TypeError / ValueError / NameError / IndexError /
KeyError / ZeroDivisionError / AttributeError / RuntimeError / StopIteration`。
`raise X("msg")` は `e.args = ("msg",)` と `e.message = "msg"` をセット。

### 文字列メソッド
`split / join / upper / lower / strip [chars] / lstrip / rstrip /
startswith / endswith / find / replace / count / encode / format /
zfill / center / ljust / rjust / title / swapcase / casefold /
splitlines / removeprefix / removesuffix / partition / expandtabs /
isdigit / isalpha / isspace / isupper / islower / isalnum`

### リストメソッド
`append / pop / extend / insert / index / reverse / sort / remove /
count / copy / clear`

### dict メソッド
`get / keys / values / items / pop / update / setdefault / popitem /
clear / copy` (挿入順保持; Python 3.7+ 仕様)

### set メソッド
`add / discard / remove / pop / union / intersection / difference`

### dunder methods
`__init__` (constructor)、`__add__ / __sub__ / __mul__ / __radd__`、
`__eq__` / `__lt__` / `__hash__`、`__repr__` / `__str__`、`__len__`、
`__getitem__` / `__setitem__`、`__enter__` / `__exit__` (with)、
`__iter__` / `__next__` (custom iterator)、`__call__` (callable instance)

### 標準ライブラリ (mini)
| モジュール | 中身 |
|---|---|
| `math` | `pi / e / sqrt / sin / cos / tan / log / log2 / log10 / exp / floor / ceil / atan2 / pow / fabs / hypot / factorial / gcd / isclose` |
| `sys` | `argv / path / version / version_info / exit / get/setrecursionlimit` |
| `os` | `getcwd / getenv / environ / listdir / remove / makedirs` + `os.path.{join, basename, dirname, splitext, exists, isdir, isfile, isabs, abspath, split}` |
| `time` | `time / sleep / perf_counter / monotonic` |
| `collections` | `OrderedDict / defaultdict / Counter / deque / namedtuple` |
| `itertools` | `chain / count / repeat / cycle / islice / takewhile / dropwhile / accumulate / product / combinations / permutations` |
| `functools` | `partial / reduce / wraps / cache / lru_cache / cmp_to_key` |
| `operator` | `add/sub/mul/truediv/floordiv/mod/.../itemgetter/attrgetter/methodcaller` |
| `copy` | `copy / deepcopy` (cycle-safe via id memo) |
| `enum` | `_make_enum("Color", {"R": 1, "G": 2, ...})` (factory; metaclass 未対応) |
| `random` | xorshift64 ベース。`seed/random/randint/choice/shuffle/sample/uniform` |
| `string` | `ascii_letters / digits / punctuation / whitespace / printable` 等の定数 |
| `pathlib.Path` | `exists/is_dir/is_file/parent/name/suffix/stem/read_text/write_text/iterdir/__truediv__` |
| `io` | `StringIO / BytesIO` |
| `json` | `dumps / loads` (str/int/float/bool/None/list/dict、escape / unicode `\uXXXX`、`__all__`) |
| `hashlib` | **md5 / sha256** — C 実装 (`__pystro_md5__` / `__pystro_sha256__` builtin) |
| `dataclasses` | `@dataclass` + `_fields`、`make_dataclass / asdict / astuple / fields` |
| `argparse` | `ArgumentParser / add_argument / parse_args` (positional / `--flag` / `--key=val` / `-v` / type / default / required / store_true) |
| `unittest` | mini — `TestCase / assertEqual / assertRaises / assertIn / main(globals())` |
| `typing` | `List / Dict / Optional / Union / Callable / TypeVar / Protocol / Final` 等 (型は no-op) |
| `asyncio` | 同期 stub (`async def` / `await` parse、`run / gather / sleep / Lock / Event / Queue`) |
| `pickle` | text-based serialization (CPython 互換ではない、int/float/str/bool/None/list/tuple/dict/set) |

### import
- `import name`、`import a.b.c [as alias]`(dotted: `a/b/c.py` を読み込む)
- `from m import x [as y], z` / `from a.b import name`
- `from m import *` (`__all__` を尊重、無ければ `_` 始まりを除外)
- モジュール init 中の例外は呼び出し側の `try/except` に伝播 (キャッシュもしない)

### スライス
- `s[i]`、`s[i:j]`、`s[i:j:k]`、負インデックス、step、`s[::-1]` 反転
- str / list / tuple に対応 (slice 結果は元と同型)
- list には slice 代入も対応

### 実行モード
| フラグ | 動作 |
|---|---|
| (なし、引数なし)    | REPL 起動 (readline 対応、`:` / 括弧での複数行入力) |
| (なし、ファイル指定) | interpreter (code_store/all.so があれば dlopen 利用) |
| `-c`               | AOT-bake してから実行 |
| `--aot-compile`    | AOT-bake のみ |
| `--no-compile`     | code_store 不使用 (純 interpreter) |
| `-e <code>`        | コマンドライン文字列を実行 |

### 例外 traceback
`raise` 時にコールスタックの関数名を `__traceback__` 属性として exc に
保存。uncaught exception で `Traceback (most recent call last): in foo
... ClassName: msg` 形式で表示する。

## 性能 (vs. CPython 3.12)

`make bench` の結果。詳細は [`perf.md`](./perf.md)。

| bench (~1s on python3) | python3 | pystro AOT | pystro/python3 |
|---|---:|---:|---:|
| while_loop 10M | 0.91 s | 0.05 s | **18×** |
| for_range 15M | 0.98 s | 0.08 s | **12×** |
| list 7M append+sum | 0.91 s | 0.19 s | **4.8×** |
| fib(35) | 1.20 s | 0.63 s | **1.9×** |
| recursive (tak) | 3.97 s | 2.58 s | **1.5×** |
| string 2M split | 0.58 s | 0.49 s | **1.2×** |
| nqueens | 0.68 s | 0.61 s | **1.1×** |
| mandel | 0.68 s | 0.65 s | **1.05×** |
| dict 3M put+get | 0.76 s | 0.83 s | 0.92× |

**8 / 9 の bench で python3 を上回る。**

### Python 仕様準拠について

pystro の bench 勝率はアーキテクチャ由来 (AST 直接ディスパッチ vs CPython の
bytecode loop、Boehm GC vs CPython の refcount + cycle collector)。
仕様面では以下を **実装済み** で、CPython 互換性を保つコストを
払っている:

- 真の metaclass (`class C(metaclass=M):` で M(name, bases, attrs))
- super proxy as value (`s = super(); s.method()`)
- complex 型 (`1+2j`, real/imag, 算術)
- descriptor protocol (`__get__` / `__set__`)
- `type(5) is int` (`type()` が真の class を返す)
- exception chaining (`raise X from Y`)、`__traceback__`
- multi-context `with`、`raise from`、`f"{x=}"` debug
- generator (`yield`、`yield from`)、generator expression (`(x for x in xs)`)
- `async def` / `await` (synchronous stub)
- 全 builtin: `id / dir / globals / locals / vars / hasattr / getattr / setattr / delattr / callable / open / eval / exec / type(name, bases, attrs)`

### 仕様準拠で残るギャップ

- built-in 型の真の subclass (`class M(list): pass` で append が継承される)
  — base が drop されるため subclass しても built-in 動作はしない
- `type` 自体が `class type(object)` ではなく builtin function オブジェクト
  (`isinstance(int, type)` は本来 True だが pystro では別判定)
- refcount + 即時 `__del__` — pystro は Boehm GC のため `__del__` の即時実行は未保証

---

## R11–R12 (autonomous probe round, test 78–91)

### dict / sequence

- bool/int dict-key collision: `{1: a, True: b}` が 1 entry に collapse (CPython 同一)
- list/dict/set/bytearray の `hash()` が `TypeError: unhashable type: ...` を上げる
- `KeyError(missing)` の str が `'missing'` (repr 形式) になる
- `del L[a:b]`, `del L[::s]` (slice 削除)
- `str.count(s, start, end)` の slice 引数
- `str % {key}` mapping 形式
- `range(0, 5) == range(0, 5, 1)` で True (CPython の sequence equality)
- `zip(strict=True)` で長さ不一致時に ValueError

### generator / 例外

- `yield from` の戻り値捕捉 (`val = yield from gen()` が StopIteration.value を取る)
- `g.close()` で本物の `GeneratorExit` を投げる
- 暗黙の `__context__` 連鎖 (raise inside except)
- `raise X from Y` で `__suppress_context__ = True`
- import 時の EXC_* save/restore を全 46 エラークラスに拡張 (PYSTRO_EXC_LIST マクロ)

### parser

- 暗黙文字列連結 (`"a" "b"`, `b"a" b"b"`)
- match パターンの `*rest` (e.g. `case [a, *mid, z]`)
- attr/subscript target の tuple-unpack (`self.x, self.y = a, b`)

### 数値

- complex の `**` (exp/log polar form)
- `int(huge_bignum)` を slice index として渡したとき clamp (例: `xs[10**100:]`)
- `float.as_integer_ratio` (frexp + 整数化 + GCD reduce)
- `n in [n]` で NaN identity short-circuit (CPython 仕様)

### attribute access / class

- property: `.deleter` / `.getter`
- `obj.__dict__` がインスタンス attrs の live alias を返す (mutate が persist)
- `__getattribute__` / `__setattr__` / `__delattr__` の user-override hook
- abstract method 強制 (`abc.ABC.__new__` が MRO walk → TypeError)
- 既定 `object.__init__` (no-op) — `super().__init__()` が built-in subclass で連鎖切れない
- `super()` が built-in 親 (dict/list/...) の method を primary 経由で resolve
- built-in subclass で primary 自動 setup (user `__init__` 有無で arg forwarding 切替)
- 関数の `__defaults__` / `__kwdefaults__`

### bug fixes

- **stale param_names**: PYSTRO_NAME_TABLE が realloc されると古い function の param_names ポインタが破損していた (importlib 後の `unexpected keyword argument` 多発)。py_make_func で param_names を private GC array に copy。
- NaN == NaN を False に
- user `__iter__` が built-in iter を返したとき正しく unwrap
- bytearray `[i] = b` の代入対応
- bytes/bytearray * int

### stdlib 拡充

- functools.total_ordering, importlib.import_module
- random: randrange / randbytes / choices / gauss
- collections.Counter: subtract / total / elements / update
- collections.deque: maxlen / rotate / extend / extendleft / count / index / remove
- enum: IntEnum / StrEnum / Flag / IntFlag / unique
- itertools.product (repeat=)
- io.StringIO: readline / __iter__ / seek / tell
- file iter (`for line in f:`)
- os.close / unlink / rmdir 等の stub

---

## R14 (continued autonomous probe — test 109–120, 121 tests passing)

### parser

- `with (cm1, cm2 as x):` 3.10+ parenthesised multi-context (prescan respects outer parens)
- 'def f(): a; b; c' inline-suite multi-statement with ';'
- 'yield a, b' yields tuple (a, b)
- match patterns on built-in types: `case int():` / `case str():` / etc.

### dunder + protocols

- list & list / list | list as set ops (dict_keys-style courtesy)
- {:,.2f} / {:_d} grouping for floats
- 'in' identity short-circuit (NaN-safe)
- format zero-pad with sign: '{-42:05d}' = '-0042'
- str.startswith/endswith/rfind/rindex/index accept start+end
- builtin function: __class__ / __qualname__ / __module__ / __doc__
- generic __class__ fallback to type(v)
- KeyError on dict lookup carries repr(key)
- '...' Ellipsis literal
- __hash__ = None enforces TypeError
- __set_name__ descriptor hook (called at class def time)
- cls[...] dispatches to __class_getitem__
- __index__ for sequence indexing + hex/oct/bin coercion
- __round__ / __floor__ / __ceil__ / __pos__ / __divmod__ on instances
- __getitem__ sequence iter protocol (no __iter__ required)
- raise non-exception → TypeError ("must derive from BaseException")

### class machinery

- `class C(B, name=...)` kwargs forwarded to __init_subclass__
- dataclass inheritance walks MRO for inherited _fields
- dataclass __post_init__
- enum methods on members (per-class _Member subclass)
- iter() idempotent: iter(it) returns it for iterators
- vars(cls) returns dict of class methods
- abc.ABC abstract method enforcement

### parameter kinds

- pos-only / kw-only enforcement at call time
- __slots__ enforcement (subclass without slots grants __dict__)
- trailing comma in def, `t = 1,` → (1,)
- (a, b) = ... / [a, b] = ... paren/bracket unpack
- lambda *args / **kw / kw-only / pos-only
- `for i, (a, b) in items:` nested tuple targets

### bug fixes

- list(zip()) infinite loop (n_inner==0 → empty every iter)
- bool/int dict-key collision → 1 entry
- complex `**` (polar form)
- stale param_names after PYSTRO_NAME_TABLE realloc

### stdlib

- functools.cached_property / singledispatch / total_ordering
- string.Template (substitute / safe_substitute) / capwords
- bisect with `key=` (3.10+)
- dataclasses.field(default_factory=...) / is_dataclass / replace / astuple
- collections.UserDict / UserList / UserString as real classes
- Counter.subtract / total / elements / update
- deque maxlen / rotate / extend / extendleft
- enum.IntEnum / StrEnum / Flag / IntFlag / unique
- itertools.product(repeat=)
- random.randrange / randbytes / choices / gauss
- io.StringIO readline / __iter__ / seek / tell
- file iter (for line in f)
- typing.NamedTuple metaclass-based
- weakref / statistics modules (best-effort)
- Fraction(float) accepts float

---

## R13 (continued autonomous probe — test 92–108, 109 tests passing)

### parser / param kinds

- positional-only / keyword-only 強制 (`/` / `*` markers)
- `__slots__` 強制 (subclass が slots 持たないなら `__dict__` 容認)
- `def f(a, b, c,)` trailing comma in def, `t = 1,` で 1-tuple
- `(a, b) = ...` / `[a, b] = ...` paren/bracket unpack
- `lambda *args:` / `lambda **kw:` / `lambda x, *, k=10:`
- inline-suite multi-stmt (`def f(): a; b; return c`)
- `class C(B, name="x"):` の class kwargs を `__init_subclass__` に forward
- `for i, (a, b) in items:` nested tuple target
- `...` Ellipsis literal

### dunder protocols (R13)

- `__set_name__` descriptor hook
- `cls[...]` → `__class_getitem__`
- `__index__` for sequence indexing
- `__round__` / `__floor__` / `__ceil__`
- `__getitem__` のみで sequence iter protocol (kind 13)
- iter() idempotent (iter(iter(xs)) is iter(xs))
- `__hash__ = None` で TypeError

### attribute / format

- builtin function `__class__` (builtin_function_or_method)
- generic `__class__` fallback to type(v)
- KeyError carries repr(key) as message
- str.startswith/endswith with start/end
- list & list / list | list as set ops (dict_keys-style)
- {:,.2f} / {:_d} grouping for floats
- "{p.x}" / "{x[0]}" string.format attr/index trailers
- list += iterable
- nan-identity in `n in [n]`

### stdlib 拡充 (R13)

- collections.UserDict/UserList/UserString as real classes
- string.Template, string.capwords
- bisect with key=
- weakref / statistics modules
- copy.deepcopy recurses through user-class __dict__
- copy honors __copy__ / __deepcopy__ hooks
- dataclasses.field(default_factory=...), is_dataclass, replace, astuple
- itertools.product(repeat=)
- random: randrange/randbytes/choices/gauss
- enum.IntEnum/StrEnum/Flag/IntFlag/unique

### class machinery (R13)

- class with no base → MRO includes implicit object
- super().method() on built-in subclass via primary
- bi_object_new sets up empty primary; user __init__ owns args
- object.__init__ no-op default
- abc.ABC abstract method enforcement

### bug fixes (R13)

- **stale param_names** (NAME_TABLE realloc) → private GC array per func
- complex `**` (polar form)
- bytearray `[i] = b`, bytes/bytearray * int
- file iter (kind 12)
