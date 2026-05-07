# todo.md — pystro 残作業

実装済みは [done.md](./done.md)、ベンチは [perf.md](./perf.md)。

---

## 残課題 (現在)

R11–R17 で深掘り (test 78–212 追加, **213 unit tests passing**)。 [done.md](./done.md) に詳細。

### R18 (2026-05-06) — CPython テスト互換ラン

`cpython/Lib/test/test_*.py` を `PYTHONPATH=cpytest_stubs:cpython/Lib`
で 1 ファイルずつ実行する sweep を回している。現状:

| 区分 | 数 |
|---|---|
| total | 394 |
| **fully pass** (`failed=0`) | **28** |
| mixed (test logic ran, ≥1 fail) | 321 |
| crash / timeout | 19 |
| parse error | 22 |
| import error | 4 |

CPython compat の作業として追加した薄い shim 群:

- **stdlib stubs**: `_socket`, `_operator`, `_io`, `_imp`, `_locale`,
  `_thread`, `_weakref`, `_contextvars`, `_tracemalloc`, `_symtable`,
  `_lsprof`, `_multibytecodec`, `_opcode`, `atexit`, `pyexpat`,
  `faulthandler`, `email/{header,message,utils,charset}`, `argparse`
  (formatter/Action 系)。
- **test.support shim**: `cpytest_stubs/test/support/{__init__,os_helper,
  import_helper,warnings_helper,threading_helper,socket_helper,
  script_helper}.py` + `multibytecodec_support.py`。skip/decorator
  系を no-op にして、実際のテスト本体は走らせる。
- **runtime fixes**:
  - `bi_import` cached re-attach — `from a.b import c` 連鎖で `a.b`
    が先にロードされて `a` が後で来たケースの parent 属性貼り直し。
  - module の `__file__` / `__dict__` 公開。
  - `exec(bytes)` を decode して受理 (`compile(b"...")` pass-through 経由)。
  - `in` 演算子の generic iterable fallback (generator / dict view 等)。
- **parser fixes**:
  - `del(target)` paren form, `del d[1, 2]` tuple key
  - `super(C)` 1-arg form
  - `with cm as (a.x, a.y):` 任意 target tuple
  - `obj[:42, ..., :24:, 24, 100]` multi-element subscript with slice
    elements (read & write)
  - `tuple[*Ts]` PEP 646 starred subscript
  - top-level `*a, *b` starred tuple (paren なし)
  - `[*rest]` を含む nested unpack (`[[a], *b] = xs`,
    `(*_, last), x, y = ...`)
  - `@=` augmented matmul
  - unpack target buffer 16 → 64
- **collections.namedtuple** field を non-data descriptor として公開
  (`Cls.field.__doc__ = ...` を許す, dis.py が要求)。
- **numbers module** + ABCMeta `_abc_registry` 経由の virtual subclass
  (`isinstance(5, numbers.Integral)`, `issubclass(complex, numbers.Complex)`
  が動く, own-only lookup なので `complex < Integral` は False)。
- **unittest.mock** subnamespace (Mock / MagicMock / patch / sentinel /
  call / mock_open) を unittest.py に同梱。
- **exec/eval/compile parse error → SyntaxError**: parse_error が
  `parse_error_jmp` (per-call jmp_buf) があれば longjmp、runtime 側で
  SyntaxError として raise する。compile() も実際に parse して
  SyntaxError を上げる (CPython 互換)。
- **types.MethodType** を constructible class 化 (`types.MethodType(fn, inst)`
  で bound method を作れる)。metaclass `_MethodTypeMeta` で
  isinstance() が pystro 内蔵 bound-method type と新 wrapper の両方を
  認識する。
- **for target attr/subscript**: `for st.lineno, line in enumerate(...):`
  / `for d[k], y in pairs:` を許容 (build_for_target_assigns が NAME
  trailer 後に parse_assignable_target に流す)。
- **del multi-element subscript with slice**: `del obj[:42, ..., 24]`
- **comparison TypeError 表記** が CPython 互換に
  (`'<' not supported between instances of 'X' and 'Y'`)。
- **`in` TypeError 表記** が CPython 互換 (`argument of type 'X' is
  not a container or iterable`)。
- **__getitem__ iteration が IndexError を catch**: kind 13 ハンドラに
  local jmp_buf を立てて、 user の `__getitem__` 内で raise された
  IndexError / StopIteration を try frame で受けて iteration 終了。
- **dict.fromkeys を instance method 経由でも呼べる**: dict_methods に
  bridge エントリ追加。
- **bytes.decode / str.encode が errors 引数受理** (1-2 → 1-3)。
- **list literal cap 256 → 2048**, **chain assign / unpack 8 → 256**
  groups。
- **PEP 646 `class C(*Ts):` starred class base**.
- **math.fmax / fmin / isqrt / prod**, **operator.{concat, iconcat,
  call, indexOf, countOf}**, **tempfile.mktemp**.
- **re top-level alternation `|`**: 既存 re.py は `|` をリテラル
  扱いだった。 `_find_top_alt` で top-level の `|` を探して左→右の
  順で alternative を試す。 `(cat|dog|fish)` や html.unescape の
  `&(#[0-9]+;?|...)` パターンが動作。

**まだ blocking している parse error 28 種** (1 ファイルずつ別案件):

- `test_pprint.py:1` — `<INDENT>` (大きな triple-quoted string か?)
- nested generators / async genexp (`(x async for x in ...)`)
- nonlocal `__class__` / nonlocal で binding 探索失敗
- 高度な PEP 695/646 syntax (`class I(*Ts):`)
- `(1).__class__ = X` (literal の attr に assign)
- 3+ chain unpack assign + 1+ tuple targets

**次の優先順位**: fully-pass 数を増やすこと自体より、mixed の 1
file あたり assertion pass / fail バランスを上げること。共通の
失敗カテゴリは `<class 'TypeError'> not raised` (63), `incomparable
operand types` (21), `object is not callable / iterable` (39),
`maximum recursion depth exceeded` (12), `'object' object has no
attribute '__next__'` (12) など。

### R17 (2026-05-06) で追加した CPython 互換項目

| 項目 | 内容 |
|---|---|
| UTF-8 codepoint str | len/index/slice/iter/find/strip/center/ljust/rjust/zfill/reversed が codepoint で動作 |
| `@` matmul operator | node_matmul + py_matmul (`__matmul__`/`__rmatmul__`); decorator 構文と両立 |
| async/await sync | `async def` / `await` / `async for` / `async with` を **sync として通す** (R16 の SyntaxError から方針転換) |
| eval/exec ns dict | `eval(code, globals)`, `exec(code, ns)` が ns dict を inject + 後の writeback まで対応 |
| PEP 604 unions | `int \| str` を tuple-of-classes として返す (isinstance/issubclass 対応) |
| paren imports | `from m import (a, b, c,)` の複数行形式 |
| collections.abc | Iterable / Mapping / Sequence 等を metaclass `__instancecheck__` 経由で動作 |
| typing.Generic etc | Generic / Literal / Annotated / Mapping / Sequence / TypedDict |
| metaclass `__instancecheck__` | `isinstance(x, cls)` が cls の metaclass の `__instancecheck__` を dispatch |
| Warning hierarchy | Warning / Deprecation / User / Future / Runtime / Syntax / Import / Bytes / Resource Warning |
| memoryview slice | 部分 view + `bytes(view)` chain |
| MappingProxyType | proper read-only dict wrapper |
| bytes.maketrans | + bytes.translate w/ delete arg |
| sys.version_info | (3, 12, 0) + named .major/.minor/.micro fields |
| os.sep / .linesep / etc | path constants |
| os.path 拡張 | normcase / split / abspath / realpath / commonprefix / commonpath / relpath |
| class annotations 値持ち | `x: int` で `__annotations__["x"] == int` (前は None placeholder) |
| 関数 `__code__` | co_varnames / co_argcount / co_kwonlyargcount / co_name 等 (inspect.signature 用) |
| Unicode identifier | `α = 5`, `日本 = "Japan"` lexer 対応 (UTF-8 byte ≥ 0x80 を ident byte 扱い) |
| generator 例外伝播 | gen body の uncaught exception が main()の err_jmp に飛ばず、swap-back 経由で caller へ正しく伝播 |
| bi_next state-only StopIter | `next(iter)` が longjmp ではなく state=RAISE 設定 — user `__next__` が次の `next()` を呼ぶパターンが動く |
| __bases__ implicit object | `class C: pass` の `C.__bases__` が `(object,)` を返す |
| typing.NamedTuple defaults | `y: int = 0` が namedtuple の右側 default として正しく扱われる |
| IntEnum 算術 | `IF.X + 1` 等 — `_EnumMember` に算術 dunder を追加 |
| concurrent.futures | Future / ThreadPoolExecutor 等の sync stub |
| fnmatch / glob / shlex | shell-style ファイル/文字列マッチ + lexer |
| pprint | pformat / pp / PrettyPrinter |
| urllib.parse | quote / urlencode / urlparse / urljoin / parse_qs |
| cmath | complex math (phase/polar/rect/sqrt/exp/log/sin/cos/tan) |
| itertools.batched | 3.12+ |
| random.Random class | + getstate/setstate + 各種 variate |
| inspect.signature | `__code__` から param 名を読んで CPython 風に str() |
| dataclasses.InitVar | + KW_ONLY / MISSING / FrozenInstanceError exposed |
| json.JSONEncoder etc | subclass 可能な JSONEncoder / JSONDecoder / JSONDecodeError |
| 関数 `__annotations__` | `def f(x: int) -> bool` 等を `{"x": int, "return": bool}` に格納 |
| PEP 585 `list[int]` etc | built-in container generic alias — annotation 用に subscript 可能 |
| cls.`__class__` | class 自体の class は metaclass (default `type`) |
| typing.get_origin/get_args | 簡易実装 |
| Lazy genexp (S-18) | synthetic gen function に desugar、all/any short-circuit OK |
| PEP 654 `except*` (S-23) | BaseExceptionGroup / ExceptionGroup + split-and-handle |
| PEP 695 type alias (S-24) | `type X = int` を plain assignment に desugar |

### 残存する仕様上の差分 (低優先)

#### S-15. async/await ✅ (R17)
- `async def` / `async for` / `async with` / `await` は **sync として通す**
  (R17 で改修)。R16 では SyntaxError だったが、CPython compat 重視で
  parse OK にして同期実行する方針へ変更。`asyncio.run(async f())` 等が
  ちゃんと動く。real coroutine ではないので yield ベースの並行は不可。

#### S-16. Wide Unicode support ✅ (R17)
- ✅ (R16) `\u` / `\U` lexer エスケープ → UTF-8 として byte string に展開
- ✅ (R16) `chr()` / `ord()` が UTF-8 multi-byte char に対応
- ✅ (R17) `len(s)` / `s[i]` / `s[a:b]` / `s[::-1]` / `for ch in s` /
  `find` / `rfind` / `index` / `rindex` / `count` / `startswith` /
  `endswith` / `strip` / `lstrip` / `rstrip` / `center` / `ljust` /
  `rjust` / `zfill` / `reversed(s)` が codepoint 単位で動作
- 残: `①`.isnumeric() 等の Unicode property 判定 (full Unicode tables 必要)
- 残: upper/lower/title/swapcase は ASCII のみ (full case mapping data 必要)

#### S-17. metaclass による class iteration ✅ (R16)
- `len(EnumClass)` / `for m in EnumClass:` / `member in EnumClass`
  動作。py_seq_len / py_iter_init / py_contains が `__metaclass__` 経由
  で `__len__` / `__iter__` / `__contains__` を呼ぶ。

#### S-18. genexp lazy ✅ (R17)
- `(x for x in xs)` が真の generator (synthetic gen function に desugar)
- `all`/`any` の short-circuit が effective、無限 source も OOM しない。
- closure 経由の var capture も動作。outer iter は eager、inner は lazy。

#### S-22. function annotations ✅ (R17)
- `def f(x: int) -> bool: ...` の `f.__annotations__` が
  `{"x": int, "return": bool}` を返す (R17 で修正)。
- typing.get_type_hints / inspect.signature ともに function に効く。

#### S-23. except* (PEP 654 — exception groups) ✅ (R17)
- BaseExceptionGroup / ExceptionGroup builtin classes。
- `except* T as v:` syntax — split-and-handle with sub-group binding。
- 未マッチは ExceptionGroup として再 raise。

#### S-24. PEP 695 type alias `type X = int` ✅ (R17)
- top-level / 関数内ともに `type NAME = expr` を plain assignment へ desugar。

#### S-21. parens-form `(a, b)` argument is a tuple, not unpacking
- `parens-wrapped multi-target with attr/subscript` は `with (cm1, cm2 as x):` 形式の
  multi-context manager (3.10+) も同じ理由で sup未。

### CPython テストスイート互換 (R18, 進行中)

`cpython/Lib/test/` (CPython 同梱 394 ファイル) で sweep:

| 結果 | カウント |
|---|---|
| 全 pass | 6 |
| 一部 pass / 一部 fail (mixed — 実際のテストロジックが動く) | 222 |
| parse error (pystro が不対応の Python 構文) | 76 |
| ModuleNotFoundError (CPython C 拡張依存) | 85 |
| crash / timeout | 5 |

実行用の env:
```sh
PYTHONPATH=cpytest_stubs:cpython/Lib ./pystro cpython/Lib/test/test_X.py
```

pystro 用のスタブパッケージ `cpytest_stubs/test/` (test.support 等) を
PYTHONPATH 先頭に。CPython 純正の test.support は annotationlib 経由で
`type.__dict__["__annotations__"].__get__` 等の C-level descriptor を
触るので、stub に逃がす必要あり。

**残 parse error の主要カテゴリ** (stdlib + test):
- `lambda: (yield)` — lambda 内 yield (無視可、レア)
- `case ast.Return(value=ast.Call()):` 形式 ✅ (fix済)
- `for x, *rest in pairs` ✅ (fix済)
- `for (k, v) in ...` paren-wrapped target ✅ (fix済)
- `with cm as (a, b):` tuple unpack ✅ (fix済)
- 巨大 dict/tuple リテラル (パーサ buffer 不足) ✅ (大型化済)
- `expected '(', got ','` 系 — まだ調査中
- 一部 f-string 高度形式 (PEP 701) — 未調査
- `lazy import x` (PEP 690) ✅ (eager 扱いで通す)
- `t"..."` template (PEP 750) ✅ (f-string 扱いで通す)

**残 ModuleNotFoundError** — pystro が用意していない CPython 内部モジュール:
`_codecs` / `_locale` / `_opcode` / `_collections` / `_threading_local` /
`importlib._bootstrap` / `array` / `ctypes` / 他多数。CPython 実装の
内部 C 拡張に依存しているため、原則 stub では足りない。

#### S-12. `re` (正規表現)
- `sample/astrorge` integrate 待ち (memory: project_regexp_astrorge)。
- 自前で書かない方針。

### 性能 (要 ASTroGen 改修)

#### BR-5. 全 NODE 型に SD bake
- 現状 ASTroGen は hot path のみ SD 化。cold path も SD 化すれば PLT hop が減る。
- pystro 単体では着手できない。

#### BR-6. dict_bench で python3 を抜く
- 現状 0.49× (本ラウンド全 features 入れて)。
- 残ボトルネック:
  - `py_dict_set` / `py_hash` の関数呼び出しコスト
  - AST dispatcher 自体のオーバーヘッド
- 改善案: `py_dict_set` / `pydict_find` を `static inline` 化、または
  CPython 風 string-only dict layout 追加。
- これ以上は pystro の AST direct dispatch 設計の限界に近い。

---

## 設計上の妥協 (変えない)

- Boehm GC (refcount + cycle collector ではない)
- インデント = スペース推奨 (タブ = 8 spaces)
- decorator は `def` / `class` 上のみ
- C3 MRO 不整合は BFS フォールバック
- string slice は buffer 共有 (slice borrow)
- set は (Python 仕様も) unordered

---

## 完了項目アーカイブ

### R9 (2026-05-05)

| 項目 | 内容 |
|---|---|
| M-6 完全実装 | `class M(list/dict/str/...)` で built-in operations が継承される。`inst.primary` 持って built-in method dispatch 時に primary に fallback (append/__getitem__/__setitem__/__iter__/len) |
| property setter | `@x.setter` 完全動作。`prop.setter(fn)` は新 property を返す。class body の name lookup を実装 (`@x.setter` が前の `@property` def を参照可能に) |
| class body name lookup | `node_class_body_load` 追加。class body 内で `def x` の前後で `x` 参照が可能 |
| docstring (class/func) | `class C: """..."""` → `C.__doc__`、`def f(): """..."""` → `f.__doc__`。triple-quoted string `"""..."""` の lexer 対応も含む |
| with CM 完全 protocol | `with` を `node_with` 化。`__exit__(type, val, tb)` が truthy 返したら例外を suppress (assertRaises 等が動く) |
| pre-scan bug fix | `obj.x = ...` の `x` が outer function の local として誤って登録されていた問題を修正 (T_DOT 直前 NAME を除外) |
| set operations | `\|`/`&`/`-`/`^` を set でサポート。`symmetric_difference`/`issubset`/`issuperset`/`isdisjoint`/`copy`/`clear`/`update` メソッド追加 |
| `__contains__` / `__delitem__` | user class の dunder を dispatch。`__iter__` 経由 fallback も実装 |
| `__neg__` / `__abs__` | user class の `-x` / `abs(x)` で dunder を呼ぶ |
| stdlib 追加 | `decimal` (固定小数), `fractions` (有理数), `threading` (synchronous stub), `logging` (print-based stub) |
| enumerate(start) | 第 2 引数 positional 形式対応 |
| dict `\|` | `dict \| dict` で merge dict (RHS wins) |
| unittest.assertRaises | 真の context manager として動作 (例外 suppress) |
| `__bool__` / `__len__` for truthiness | user instance の `if x:` で dunder dispatch (`py_is_truthy_instance`) |
| isinstance(True, int) | True/False が int の subclass として扱われる |
| str.format(name=val) | named field の format を実装 |
| iter(callable, sentinel) | 2-arg form 対応 (kind=4) |
| kw-only / pos-only marker | `def f(a, *, b)` と `def f(a, /, b)` の parse 対応 |
| spread in literal | `[*it]` `(*it,)` `{*it}` `{**d}` の parser desugar (list+extend / tuple()変換 / set+update / dict+update) |
| bound method __doc__/__name__/__self__/__func__ | `c.m.__doc__` 等が underlying func を forward |
| format spec extras | `:#x`/`:#o`/`:#b` (alt form prefix), `:,` (thousands), `:%` (percent), `:+d` (sign) |
| 例外クラス追加 | `ImportError` / `ModuleNotFoundError` / `OSError` / `FileNotFoundError` / `IOError` / `ArithmeticError` / `OverflowError` / `NotImplementedError` |
| import 失敗 → ImportError | `ModuleNotFoundError` を raise (RuntimeError ではなく) |
| uncaught exception 表示バグ | `setjmp` で longjmp 経由のときに traceback が出ないバグを修正 (main.c) |
| print(sep, end, file) | kwargs が無視されていたのを修正 |
| str method 追加 | `capitalize` / `rfind` / `rindex` / `index` (ValueError on miss) / `isnumeric` / `isdecimal` / `isascii` |
| enumerate / zip / map / filter を lazy 化 | 従来は eager で list 返却 → 無限 generator で hang していた。py_iter に kind=8/9/10/11 を追加して lazy iterator に |
| cpython port | test 58 (built-in subclass)、59 (property)、60 (decimal/fractions)、61 (set ops + custom container)、62 (dunders)、63 (syntax: iter sentinel/markers/spread/bound meta/format spec)、64 (exception classes / str methods) |

### R8 (2026-05-05)

| 項目 | 内容 |
|---|---|
| M-5 拡張 | `int / float / str / list / dict / tuple / set / frozenset / bytes / bytearray / range / type / object / complex / bool` を **真の class object** に。 `isinstance(int, type)` is True、`isinstance(M, type)` for class M も True |
| M-6 (部分) | `class M(int): pass` 等で base が真の class なので継承の syntax は動作 |
| pyclass.builtin_ctor | class struct に C 関数ポインタ追加。 py_apply on class with builtin_ctor は直接呼び出し |
| 副: meta attrs | `f.__name__/__qualname__/__module__/__doc__/__annotations__`、`C.__bases__/__mro__/__dict__/__doc__`、`c.__class__/__dict__` |
| 副: globals | `__name__` (\"__main__\")、`__import__()` builtin |
| バグ fix | `bi_type` で flonum を PY_PTR 経由で参照していた SEGV 修正 |
| cpython port | test_typesys (test 57): type は class、bases/MRO/dict、関数 meta、subclass 構文 |

### R7 (2026-05-05)

| 項目 | 内容 |
|---|---|
| B-4 | super proxy as value (`s = super(); s.method()`) — PY_T_SUPER 型追加 |
| BR-3 | 真の metaclass — `class C(metaclass=M):` で M(name, bases, attrs) を呼び出す。M.__new__ + M.__init__ chain も対応。`type(name, bases, attrs)` 3-arg 形式実装 |
| 副-A | `py_class_lookup_method` を non-static 化 |

### R6 (2026-05-05)

| 項目 | 内容 |
|---|---|
| BR-1 | `asyncio` 同期 stub (`async def` / `await` parse、`run / gather / sleep / Lock / Event / Queue`) |
| BR-2 | `typing` モジュール (`List/Dict/Optional/Union/Callable/TypeVar` 等 — no-op) |
| BR-3 (部分) | metaclass kwarg parse |
| BR-4 | `pickle` (text-based custom format) |
| S-7 | `hashlib` md5/sha256 (C 実装、known vector 一致) |
| 副 | bytearray.append/extend、`obj[a, b]` tuple subscript、async/await soft-keyword |
| cpython port | test 55 (typing/asyncio/pickle/hashlib/dataclass/argparse/enum/collections/functools) |

### R5 (2026-05-05)

| 項目 | 内容 |
|---|---|
| B-2 | complex literal `1j` / `1.5j` parse — lexer に T_IMAG token 追加 |
| M-4 | complex 型 — PY_T_COMPLEX、`+ - * / -` 演算、`==` 比較、`real / imag` 属性、`complex(re, im)` builtin |
| M-6 (部分) | `class M(list):` などが parse error / SEGV しない |
| cpython port | test_complex / test_func / test_misc (test 52〜54) |

### R4 (2026-05-05)

| 項目 | 内容 |
|---|---|
| M-5 | `type(5) is int` を True に |
| M-7 | descriptor protocol (`__get__` / `__set__`) |
| S-5 | `dataclasses` (`@dataclass` + `_fields`、`make_dataclass / asdict / astuple / fields`) |
| S-11 | `argparse` |
| 副 | `unittest` mini、dict/set 等価比較、`str.split(maxsplit)`、method state check、CPython test port (test 45〜51) |

### R3 (2026-05-05)

| 項目 | 内容 |
|---|---|
| B-1 | except as e: が外側 local を破壊 — py_run_try で env / globals / call_top を save/restore |
| B-3 | bytes literal `\x` escape |
| E-1〜8/12 | open/file I/O、id/dir/globals/locals/vars/hasattr/getattr/setattr/delattr/callable、eval/exec、int(s, base)、str/dict methods 多数 |
| M-1 | `with a, b:` 多重 context manager |
| M-2 | `raise X from Y` exception chaining |
| M-3 | f-string `{x=}` debug syntax |
| S-1〜10 | functools / operator / copy / enum / random / string / pathlib / io |
| 副 | `__truediv__` dunder dispatch、PY_T_ITER の for-loop、with-as pre-scan、multi-import、method spread |

### R2 (2026-05-05)

dict 0.49→0.78× の改善 (compact dict refactor、attr inline cache、traceback、parser 改善、stdlib 拡充、REPL)。

### R1 (2026-05-05)

A1〜F3 (18 項目): arith_cache、del NAME、循環参照 display、match 拡張、generator method、import dotted、bytes/bytearray、クラス属性、starred target、f-string conversion、% formatting / .format()、dict 挿入順、`__hash__` / Exception 伝播、nested gen テスト等。

---

## 進捗 (cumulative)

- test: 26 → **65** (CPython 互換 unittest 形式 20 ファイル含む)
- bench: 8/9 で python3 越え → R7 で全機能入れて 6/9 (新機能で dispatch コスト増加)
- 言語機能: subset → ほぼフル仕様 (super value / metaclass / complex / descriptor / async stub / typing / pickle / property setter / built-in subclass / docstring 等)
- stdlib: 0 → 21 モジュール (math/sys/os/time/json/collections/itertools/functools/operator/copy/enum/random/string/pathlib/io/typing/asyncio/pickle/hashlib/argparse/dataclasses/unittest/decimal/fractions/threading/logging)
