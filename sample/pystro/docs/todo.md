# todo.md — pystro 残作業

実装済みは [done.md](./done.md)、ベンチは [perf.md](./perf.md)。

---

## 残課題 (現在)

R11–R16 で深掘り (test 78–167 追加, **168 unit tests passing**)。 [done.md](./done.md) に詳細。

### 残存する仕様上の差分 (低優先)

#### S-15. async/await
- `async def` をパース error とすべきが現状黙って受け入れる。coroutine model なし。

#### S-16. Wide Unicode support
- `\U` 8-digit エスケープ未対応。`chr(>0xFF)` で multi-byte 不可。
- `①`.isnumeric() 等の Unicode 系判定が False を返す。

#### S-17. metaclass による class iteration ✅ (R16)
- `len(EnumClass)` / `for m in EnumClass:` / `member in EnumClass`
  動作。py_seq_len / py_iter_init / py_contains が `__metaclass__` 経由
  で `__len__` / `__iter__` / `__contains__` を呼ぶ。

#### S-18. genexp が eager
- `(x for x in xs)` は list を返す (本来は generator)。
- 無限 source を `for x in genexp:` で使うと OOM。
- 大半の用途 (`list(genexp)`, `sum(genexp)`) は OK。

#### S-21. parens-form `(a, b)` argument is a tuple, not unpacking
- `parens-wrapped multi-target with attr/subscript` は `with (cm1, cm2 as x):` 形式の
  multi-context manager (3.10+) も同じ理由で sup未。

### deferred (外部 sample 依存)

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
