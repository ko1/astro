# cpython_compat_catalog.md — CPython stdlib 非互換カタログ

policy: pystro は CPython 3.12.13 (submodule pin) の純 Python stdlib
(`cpython/Lib/`) を **PYTHONPATH 経由でそのまま使う**。 互換性が取れない
箇所だけ pystro 側 (`stdlib/` または C runtime) で吸収する。

## sweep の現状 (2026-05-09)

### File-level 合格

ファイル単位で `rc=0` を返す test_*.py が **14 件** に到達:

```
test_bigaddrspace test_colorsys test_contains test_copyreg test_eintr
test_embed test_int_literal test_keyword test_osx_env test_sundry
test_type_annotations test_unary test_urllib_response
test_xml_dom_minicompat
```

- 6 件は all-skip (`OK (skipped=N)`): bigaddrspace / embed / osx_env /
  sundry / type_annotations / eintr。 環境前提を満たさず skip だが
  unittest が走り SystemExit がきれいに 0 終了する。
- 8 件は実 assertion をパス: colorsys (7) / contains (4) / copyreg
  (6) / int_literal (6) / keyword (11) / unary (6) /
  urllib_response (4) / xml_dom_minicompat (11) → 計 55 件。

### 個別 unit-test 合格

42 個の test_*.py ファイルが unittest runner に到達 (`Ran N tests` を
出力)、 その中で **287 個以上の unit-test が pass している**:

- `test_abc` 13/72, `test_dict` 多数, `test_property` 8/32,
  `test_xml_dom_minicompat` 10/11, `test_typechecks` 6/6 (exit 時
  segv フレーキー), etc.

## このセッションでの主な改善 (1日 / 30+ commits)

**Runtime / parser 修正:**
- `super(C, cls).__new__` (functools.partial / 2-arg form)
- `super(): instance OR class` (functools.wraps + dataclasses)
- TracebackType chain on `e.__traceback__` (`tb.tb_frame.f_code.co_name`)
- 例外 instance に必ず __traceback__ / __context__ / __cause__ /
  filename / lineno / msg / errno / strerror / name / path /
  encoding / object / start / end / reason / obj / __notes__ を設定
- StopIteration / GeneratorExit が full attribute set を持つ
- Class-attr lookup が "absent" vs "value is None" を区別 (50+ tests)
- Class-body 名前解決を MRO traverse しない (selectors / 10 tests)
- `int.from_bytes(iter)` map イテレータ受理 (35+)
- `int(int_subclass(x))` recursion を primary value 経由で短絡
- `__init_subclass__` 暗黙 classmethod, `@classmethod` 経由両対応
- `complex %` / `complex //` → TypeError (CPython parity)
- `'in <string>' requires str` enforced
- `__contains__ = None` blocks iter fallback
- `[nan,...] == [nan,...]` 同一オブジェクト identity-shortcut
- `name, = (x,)` 1-target trailing-comma tuple unpack (pickle 等)
- async / await as identifier → SyntaxError (CPython 3.7+ kw)
- `node_in` / `node_not_in` propagates exceptions
- ABC register propagates to ABCMeta parents (numbers tower)
- `cls.mro()` (list form) added
- issubclass dispatches via metaclass __subclasscheck__
- ABC class uses ABCMeta as metaclass

**Synthetic dunders / type系:**
- `Exception.with_traceback(tb)` / `add_note(s)` (CPython 3.11+)
- `int.__hash__(self)` (unbound dunder via class)
- `type.__dict__['__mro__'/'__bases__'/'__dict__']` synthetic descriptors
- `float.__getformat__("double")`
- 全 instance に universal `__reduce_ex__` / `__reduce__` /
  `__sizeof__` / `__dir__` / `__init_subclass__` / `__subclasshook__` /
  `__format__` / `__getattribute__` / `__class_getitem__` shim
- `bytes/bytearray.copy()`

**Module / runtime:**
- SystemExit translates to process exit code (CPython parity)
- 全 module に `__doc__` / `__spec__` / `__loader__` / `__cached__`
- `__import__` accepts CPython 5-arg signature
- `__pystro_stat__` builtin (posix.stat real syscall)
- `_simple_enum` decorator が auto() を sequential int に rewrite
  (ast.py / 160+ tests)
- post-load injection of `inspect.CO_*` flags
- `globals()` 系 mutation を真の write-back proxy では未対応
  (inspect は hand-injected で逃げ)

**stdlib 追加 / 拡張:**
- 新規: `_struct` (struct calcsize/pack/unpack), `_random` (xorshift),
  `_typing` (TypeVar / Generic), `socket` (constants + stub),
  `sysconfig`, `_dbm` (in-memory), `_csv`, `ipaddress` (full stub
  bypassing CPython's __slots__-heavy chain), `__future__`
- 拡張: `posix.stat_result` / `register_at_fork` / WNOHANG / SEEK_*
  / O_PATH / O_TMPFILE / O_*; `sys.platlibdir` / `flags` /
  `float_info` / `int_info` / `hash_info` / `intern` /
  `getrecursionlimit`; `_thread.RLock` 真の reentrant +
  `_set_sentinel` / `_at_fork_reinit` / `_ThreadHandle`;
  `binascii.b2a_base64` / `a2b_base64` / qp; `hashlib.shake_256` /
  `sha1` / `sha3_*` / `blake2s` / `pbkdf2_hmac`; `BytesIO.getbuffer`
  / readline / writelines

**pystro_first list (pystro stub が PYTHONPATH より優先):**
- `types.py`, `abc.py`, `_py_abc.py`, `enum.py`, `re.py`,
  `importlib.py`, `signal.py`, `sysconfig.py`, `hashlib.py`,
  `socket.py`, `typing.py`, `ipaddress.py`

## 残ってる top blocker

| 件数 | 原因 |
|---:|---|
| 多数 (1セッションで50+) | `core dumped` — 多くは exit-time のみ (test 自体は OK)。 GC / __del__ / atexit 周りで再現性なし |
| ? | unittest framework 内部の細かい assert (各 test 1-2 件) |
| 11 | `test.multibytecodec_support` — cpytest_stubs に shim 追加が必要 |
| 11 | `unittest.mock` — asyncio chain で core dump |
| 8 | `asyncio.staggered` — asyncio package の submodule access |
| 12 | `'super' object has no attribute '__init__'` — typing 等で多重継承 super 欠落 |
| 4 | `_testinternalcapi` — CPython 内部用 |

## 主要な未解決バグ (一覧)

### exit-time segfault (フレーキー)

unittest が完走して `Ran N tests / OK` を出力しても、 SystemExit
処理後に低確率で core dump。 _exit() を試したが効果なし。 GC か
finalizer 経路での問題と推測。 同じテストを 5 回走らせると 1-2 回
segv する程度のフレーキーさ。

### `int(self.netmask)` returns None inside ipaddress.IPv6Network (回避済み)

CPython ipaddress.py の IPv6Network('fe80::/10') 構築チェーンで
attribute access specialization が誤って None を返すバグ。 仕様を
理解するには pystro の attribute IC + __slots__ + 多重継承 + bignum
の組合せを深掘りする必要があり未解決。 stdlib/ipaddress.py stub で
回避中。

### globals() が live でない

`mod_dict = globals(); mod_dict[key] = val` が反映されない。
`bi_globals` は dict snapshot を返す。 inspect 用に CO_* を
post-load injection で逃げているが、 真の修正は live globals proxy。

