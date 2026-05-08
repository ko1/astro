# done.md — pystro 実装済み

ASTro 上の Python サブセット。本書は **動く言語機能** を一覧する。
未実装は [todo.md](./todo.md)、ランタイム解説は [runtime.md](./runtime.md)、
ベンチ結果は [perf.md](./perf.md)。

## テストスイート

```
$ make test
... 213 tests OK ...
passed=213  failed=0  total=213
```

interpreter / AOT cached の両方で **213/213 pass**。
test 45〜213 は CPython の `Lib/test/test_*.py` を pystro 用に adapt した
unittest 形式 (TestCase / assertEqual / assertRaises / main(globals()))。

### CPython 公式テストの直接実行 (R18)

`cpython/` を clone して `PYTHONPATH=cpytest_stubs:cpython/Lib` で
`Lib/test/test_*.py` を pystro で実行している。 現状 (2026-05-07):

| 区分 | 数 |
|---|---|
| total | 394 |
| **fully pass** (`failed=0`) | **28** |
| mixed (test logic ran, ≥1 fail) | 322 |
| crash / timeout | 18 |
| parse error | 22 |
| import error | 4 |

詳細は [todo.md](./todo.md) の R18 節を参照。 CPython 互換性のために
入れた shim 群:
- stdlib stubs: `_socket`, `_operator`, `_io`, `_imp`, `_locale`,
  `_thread`, `_weakref`, `_contextvars`, `_tracemalloc`, `_symtable`,
  `_lsprof`, `_multibytecodec`, `_opcode`, `atexit`, `pyexpat`,
  `faulthandler`, `email/{header,message,utils,charset}`, `argparse`
  (formatter/Action), `numbers`。
- test.support shim package (cpytest_stubs/test/support/) — skip
  decorator + helper 群を no-op で。
- runtime: `bi_import` cached re-attach, module の `__file__` /
  `__dict__`, `exec(bytes)` 受理, `in` の iter protocol fallback,
  単項 `+` で non-numeric は TypeError, exec/eval/compile が
  parse 失敗時 SyntaxError, ABCMeta `_abc_registry` 経由 virtual
  subclass, **chained call/attr/subscript で raise 伝播**,
  **PYSTRO_BI_KWC leak 修正** (configparser 用)。
- parser: `del(target)` paren / `del d[1, 2]`, `super(C)` 1-arg,
  `with cm as (a.x, a.y):`, multi-element subscript with slice,
  `tuple[*Ts]`, top-level `*a, *b`, nested `[*rest]`, `@=` matmul,
  `class C(*Ts):`, `for st.lineno, line in ...`, chain assign / unpack
  group buffer 256。

これにより `213 internal + 28 CPython official` = 多数のテストが
pass。

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

`make bench` の結果。 詳細は [`perf.md`](./perf.md)、 比較分析は
[`vs_cpython.md`](./vs_cpython.md)。

### micro (10 本、 best-of-5)

| bench (~1s on python3) | python3 | pystro AOT | pystro/python3 |
|---|---:|---:|---:|
| while_loop 10M | 0.97 s | 0.06 s | **16.2×** |
| for_range 15M (C range) | 0.96 s | 0.07 s | **13.7×** |
| for_range_pyrange (Py iter) | 2.28 s | 0.41 s | **5.6×** |
| list 7M append+sum | 0.95 s | 0.19 s | **5.0×** |
| fib(35) | 1.14 s | 0.40 s | **2.9×** |
| recursive (tak) | 4.19 s | 1.56 s | **2.7×** |
| mandel (float-heavy) | 0.72 s | 0.27 s | **2.7×** |
| nqueens | 0.72 s | 0.36 s | **2.0×** |
| string 2M split | 0.61 s | 0.54 s | **1.13×** |
| dict 3M put+get | 0.74 s | 0.94 s | 0.79× (1.27× 遅い) |

**10 / 10 micro 中 9 で python3 を上回る** (`dict_bench` のみ負け)。

### macro (4 本、 pyperformance 由来)

| bench | python3 | pystro AOT | pystro/python3 |
|---|---:|---:|---:|
| richards (OS sched sim)         | 1.07 s | **0.49 s** | **2.18× FASTER** |
| crypto_pyaes (pure-Py AES-CTR)  | 0.54 s | **0.37 s** | **1.46× FASTER** |
| deltablue (constraint solver)   | 0.17 s | **0.14 s** | **1.21× FASTER** |
| raytrace                        | 0.89 s | **0.84 s** | **1.06× FASTER** |

**4 / 4 macro 全勝** (2026-05-08)。 命令数も全 4 で python3 を下回る
(やる仕事の絶対量が少ない)。

### 主要な最適化 (時系列、 直近順)

詳細は [`perf.md`](./perf.md)。 ここでは要点のみ。

#### Phase 8 (2026-05-08): macro 全勝

- `node_subscript_get` に fixnum index fast path
- `node_floordiv` / `node_mod` に fixnum fast path
- **`node_iadd` 新設** — `lst += [...]` を in-place extend (pyaes O(N²)→O(N))
- attr_cache に instance-receiver class-attr 用 monomorphic slot
  (`inst_ca_*`)
- `pyclass.fast_new` flag — default `__new__` 経路を skip
- `py_method_resolve` に classmethod IC + `node_method_N` に
  `t == PY_T_CLASS` branch
- **parser LHS の `dot+call` fuse** — `self.x().y = z` の中間 `self.x()`
  を `node_method_0` に fuse (deltablue を 0.57 s → 0.14 s に押し下げ)

#### Phase 7 (前 session): module IC + attr_set fast path

- `node_method_*` の fast path に `t == PY_T_MODULE` branch
- attr_set fast path で新 instance の attrs alloc を inline 化
- `py_alloc` を per-type sizing (instance alloc 312B → 32B)

#### Phase 4〜6: IC の積み上げ + bytes/bit ops

- user-class method monomorphic / 4-way IC
- dunder 24 種を pre-intern + struct pyclass の slot に格納
- `attrs_id` を class 共有の `shape_version` に置換
- attr_cache も 4-way polymorphic 化
- `binop_cache` (node_add/sub/mul の per-call-site IC)
- bit op の fixnum fast path、 `lm_pop` の memmove 化

最初の baseline (richards 6.83 s / deltablue 2.27 s / raytrace 6.19 s /
pyaes 2.58 s) → 現在 (0.49 / 0.14 / 0.84 / 0.37):
**richards 13.9× / deltablue 16.2× / raytrace 7.4× / pyaes 7.0× faster**。

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

---

## R16 (2026-05-06)

138 → 208 unit tests passing (added 70 new test files).

### big semantic / scope fixes (later in R16)

- **Comprehension scope (CPython 3)**: `[i for i in xs]; print(i)` now
  raises NameError.  Loop targets get synthetic comp-private locals
  via parser-side name remap.  Lambda / def inside the comp re-enters
  fresh scope so `lambda x, i=i: ...` per-iteration capture works.
- **yield-from close/throw protocol**: `outer.close()` / `outer.throw()`
  while suspended in `yield from inner` propagates to inner — inner's
  finally / except runs.  Implemented via `pygen.yf_inner` tracking.
- **Metaclass `__call__` override** (singleton pattern): `cls(...)`
  walks `__metaclass__.__call__` first.  type.__call__(cls, ...) added
  as a builtin so override can delegate to default construction.
- **Class-attr lookup falls through to metaclass**: `cls._instances`
  reaches `SM._instances` defined on the metaclass.
- **Lambda nested-closure heap frames**: parse_lambda detects nested
  lambda / def in the body and forces a heap-allocated frame so the
  inner closure doesn't read freed alloca'd memory after the outer
  returns.  Fixes Y-combinator factorial.
- **async / await as SyntaxError**: pystro has no coroutine model;
  silently accepting `async def` / `await` was misleading.  Both now
  parse-error.
- **Genexp returns iterator**: `(x for x in xs)` is now wrapped in
  iter() so `next(genexp)` works (previously returned a list).  Still
  eagerly materialised — true laziness needs a synthesised gen fn.
- **OrderedDict order-aware __eq__**: `OrderedDict == OrderedDict`
  compares insertion order; `OrderedDict == dict` remains
  order-insensitive.
- **int/float/complex .real/.imag/etc. as @property** (was: methods).
  type_method gained an is_property flag invoked at access.
- **descriptor __get__ at class-attr access**: `Cls.x` invokes
  `desc.__get__(None, Cls)` if x is a non-data descriptor.
- **super().__init_subclass__()**: object now provides a no-op default
  so the chain terminates cleanly.
- **TYPE_method/function/generator/module/NoneType** identity preserved
  across module imports → `isinstance(x, types.MethodType)` works.
- **List += iterable** accepts str / range / dict / set / gen / etc.
- **Bytes hash by content**: `hash(b"abc") == hash(b"abc")`; bytes-as-
  dict-key works.
- **bytes + bytearray respects LHS**: result type follows left operand.
- **bytes.hex(sep[, bytes_per_sep])** (CPython 3.8+).
- **Nested classes attached to outer**: `Outer.Inner` resolves.
- **Exception __cause__ / __context__ / __suppress_context__** always
  exist (None / False) on every exception instance.
- **gen.close() propagates non-GE/StopIteration exceptions** to caller.
- **match-stmt `pat as NAME`** pattern (sequence / class / or / guard).
- **Positional class match patterns via `__match_args__`**.

### parser

- nested unpack: `(a, b), c = ...` and `[(a, b), c] = ...`
- `for (a, b) in pairs:` (parens around for-target tuple — single-paren detector)
- adjacent f-string concatenation: `f"a{1}" f"b{2}"` and mixed f-string / plain str
- `int("+42")` / `int("-0x1f", 16)` — sign before prefix
- `expandtabs(0)` no longer SIGFPEs (tabsize=0 → drop tabs)

### runtime

- **gen body try-stack reset** — generators run on a separate ucontext
  stack, so longjmping into the caller's stale jbufs caused random
  segfaults whenever a `with` body inside `contextlib.contextmanager`
  raised.  Now the gen body has its own (initially empty) try-stack;
  exceptions escaping it propagate via state=RAISE / swapcontext.
- bound-method __eq__ + __hash__ by (self, func) identity (so `m.f == m.f`
  is True and `{m.f, m.f}` collapses)
- Exception str/repr now uses .args (multi-arg shows tuple repr)
- gen.throw 3-arg form (type, value, tb) materialises type → instance
- super().method() works in @classmethod / @staticmethod (unwrap CLASSMETHOD
  / STATICMETHOD descriptors after MRO walk)
- format default-align numerics — `f"{42:5}"` right-aligns, `{42:05}` zero-pads
- exec(code, globals[, locals]) accepts the dict args (ignored — single
  global namespace)
- bytearray slice assignment supports general resize / step / iterables
- built-in subclass instances mirror primary repr (MyInt(42) → "42")
- str.join no longer caps at 256 items
- struct.pack/unpack "f" / "d" use proper IEEE-754 via new
  __pystro_float_to_bits__ / __pystro_bits_to_float__ builtins

### stdlib

- os.path.normpath, os.mkdir
- math.dist, math.hypot(*coords), fsum / fmod / cbrt / expm1 / log1p / remainder
- datetime: date+timedelta / date-date arithmetic, time.__str__, timedelta * /
  // / abs / hash; date hashable
- contextlib.contextmanager passes the actual exception instance and
  re-raises if the gen didn't catch
- functools.wraps copies __name__/__doc__/__module__/__qualname__ via
  setattr-honouring func attr override
- functools.lru_cache returns CacheInfo with hits/misses/currsize and
  cache_clear; supports both @lru_cache and @lru_cache(maxsize=N)
- collections.Counter __repr__ sorts by descending count (CPython-style)
- json.dumps default= callable, json.loads object_hook=
- pickle: bytes (hex-encoded) and best-effort user-class instances via
  __dict__ + sys.modules class lookup
- re.py: capturing groups (), backrefs in sub, char-class escapes \\d/\\w,
  lazy quantifiers, IGNORECASE on ranges, subn returning (str, count)
- sys.exit raises SystemExit (catchable); sys.exc_info() via __pystro_current_exc__
- sys.setrecursionlimit / getrecursionlimit; CTX gains recursion_limit
  field (default 1000); call dispatcher raises RecursionError instead
  of segfaulting on infinite recursion

### later in R16

- **kwarg shadowing fix** (parser bug): `f(type=int)` previously made
  `int` a local of the caller because the prescan didn't track paren
  depth; now kwarg names inside calls don't get registered as locals
- argparse: choices=, nargs='+'/'*', const=, metavar=
- file methods: seek/tell/readable/writable/seekable/truncate
- file IO regression test (157)
- pathlib: name/parent/suffix/stem/parts/parents @property; with_suffix,
  with_name, unlink(missing_ok=)
- io.BytesIO seek/tell, normalises chunks via bytes()
- enum class supports len/iter/'in' via metaclass __len__/__iter__/__contains__
  on the metaclass; py_seq_len/py_iter_init/py_contains walk
  __metaclass__ for class objects
- slice is now a class (TYPE_slice = builtin class with bi_slice ctor),
  isinstance(slice(1, 5), slice) is True; TYPE_slice saved/restored
  across module imports
- dict subclass __missing__ dispatch
- NotImplemented from __op__ falls through to reflected op (was: returned
  the NotImplemented sentinel)
- math.trunc/floor/ceil dispatch __trunc__/__floor__/__ceil__ dunders
- 0 ** negative_int raises ZeroDivisionError (was: returned inf)
- int() accepts +/- before 0x/0o/0b prefixes
- chr() / ord() handle full Unicode via UTF-8 encoding (was: ASCII-only)
- \\u / \\U escape sequences in string literals → UTF-8 bytes
- import os.path now binds the top-level name (os) and uses parent-attr
  lookup when no module file matches the dotted path
- reversed(bytes), bytes substring/byte 'in' membership
- bytes ordered comparison (memcmp + length tiebreak)
- bytearray gets list-like mutators: insert/pop/remove/reverse/clear
- set.update / set.difference_update accept multiple iterables
- bool(range) reflects emptiness
- list .index/.count use identity short-circuit (so [nan].count(nan) is 1)
- nested classes: Outer.Inner now installed as attr of Outer
- built-in subclass cmp/eq via primary (MyInt(3) == 3 etc.)
- float.as_integer_ratio handles 0.0 (was: OverflowError) and raises
  cleanly on NaN/Infinity
- exception __traceback__ attr always exists (None for top-level raises)
- positional class match patterns via __match_args__: case P(0, 0)
  resolves field names from P.__match_args__ at match time
- with-stmt __exit__ raise chains __context__ (matches CPython
  exception chaining)
- dataclass(frozen|eq|order) kwargs honoured (FrozenInstanceError on
  assign-after-init; eq=False uses identity; order=True synthesises
  __lt__/__le__/__gt__/__ge__ from field tuple)
- statistics.harmonic_mean / geometric_mean / multimode added
- ChainMap mutators (__setitem__/pop/clear); namedtuple._make
- Counter(dict) / Counter(**kwargs) initialise via values
- str.split(None) → whitespace-runs split (was: TypeError)
- str.format_map uses py_list_get so user mapping subclasses work
- json.dumps decodes UTF-8 → codepoints, escapes \\uXXXX (with
  surrogate pairs for U+10000+); ensure_ascii=False emits raw UTF-8
- complex(str) parses 1+2j / -j / pure / paren forms

### R17〜R18 (CPython テスト互換性向上)

R17 で UTF-8 codepoint str / `@` matmul / async sync / eval-exec ns
dict / PEP 604 union / collections.abc + typing.Generic / etc を入れ
**213 internal tests passing**。 R18 では CPython の `Lib/test/test_*.py`
を直接動かす方向で:

#### parser
- `del(target)` paren / `del d[1, 2]` tuple-key subscript
- `super(C)` 1-arg form (パースのみ; `super()` と等価扱い)
- `with cm as (a.x, a.y):` 任意 trailer 付き unpack target
- multi-element subscript with slice (`obj[:42, ..., :24:, 24, 100]`)
  read / write / del
- `tuple[*Ts]` PEP 646 starred subscript element
- top-level `*a, *b` starred tuple (paren なし)
- nested unpack with `[*rest]`、 post-star は negative index
- `@=` matmul augassign
- `class C(*Ts):` PEP 646 starred class base
- `for st.lineno, line in ...` (for-target に attr/subscript)
- chain assign / unpack group buffer 8 → 256
- list literal cap 256 → 2048
- exec/eval/compile parse 失敗が SyntaxError を raise (longjmp 経由)

#### runtime
- chained call/attr/subscript で raise が伝播
  (`raiser().attr` が AttributeError ではなく元の例外を投げる)
- `__getitem__` iter protocol が IndexError を local try frame で catch
- `in` 演算子の generic iter protocol fallback
- 単項 `+` で non-numeric は TypeError
- exec(g, l) が新名を locals 側に書き戻し (timeit 用)
- `__class_getitem__` の @classmethod を unwrap
- iter / `<` の TypeError 表記が CPython 互換
  (`'X' object is not iterable`,
   `'<' not supported between instances of 'X' and 'Y'`)
- ABCMeta `_abc_registry` 経由 virtual subclass
  (`isinstance(5, numbers.Integral)` が動く)
- bi_import cached re-attach (dotted module の parent 属性貼り直し)
- module の `__file__` / `__dict__` 公開
- exec(bytes) / eval(bytes) / compile(bytes) 受理
- nested class call の PYSTRO_BI_KWC leak fix
  (`RawConfigParser(defaults={})` が動く)

#### 標準ライブラリ shim
- stdlib stubs: `_socket`, `_operator`, `_io`, `_imp`, `_locale`,
  `_thread`, `_weakref`, `_contextvars`, `_tracemalloc`, `_symtable`,
  `_lsprof`, `_multibytecodec`, `_opcode`, `atexit`, `pyexpat`,
  `faulthandler`, `email/{header,message,utils,charset}`, `argparse`
  拡張 (formatter / Action / Namespace / FileType etc.), `numbers`
- `cpytest_stubs/test/support/` shim package (skip decorator + helper
  群 を no-op)
- `types.MethodType(fn, inst)` constructible (metaclass で内蔵
  bound-method type も認識)
- `unittest.mock` subnamespace (Mock / patch / sentinel / mock_open)
- `collections.namedtuple` field を non-data descriptor 化
  (`Cls.field.__doc__ = ...` を許す)
- `dict.fromkeys` instance method 経由でも呼べる
- `bytes.decode` / `str.encode` が errors 引数受理
- `math.fmax / fmin / isqrt / prod`, `math.isclose` に self leak
  workaround
- `operator.{concat, iconcat, call, indexOf, countOf}`
- `tempfile.mktemp`, `NamedTemporaryFile` に __iter__ / tell / flush
- `os.stat_result` / `times_result` skeleton
- `csv.{register,unregister,get,list}_dialect`,
  `QUOTE_{MINIMAL,ALL,NONNUMERIC,NONE}`
- `re` top-level alternation `|` (paren 内も group 内も)

CPython 公式テストの sweep 結果:

```
total=394 pass=28 mixed=322 crash/timeout=18 parse_err=22 import_err=4
```

`pass=28` は本物のアサーションを含むテストが全部通っているもの (空の
スケルトンも数件含む)。 `mixed=322` は test logic が走って一部失敗、
`pass=N failed=M` を出している。 残課題は [todo.md](./todo.md) 参照。
