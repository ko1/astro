# cpython_compat_catalog.md — CPython stdlib 非互換カタログ

policy: pystro は CPython 3.12.13 (submodule pin) の純 Python stdlib
(`cpython/Lib/`) を **PYTHONPATH 経由でそのまま使う**。 互換性が取れない
箇所だけ pystro 側 (`stdlib/` または C runtime) で吸収する。

## sweep の現状 (2026-05-10)

### このセッションで進んだこと

主要な SEGV を 6 系統まとめて潰し、 大物テスト (test_set / test_dict /
test_list / test_tuple / test_userlist) のうち 4 つが SEGV → unittest
完走 (FAILED summary) に進化した。 残る test_dict / test_userdict /
test_compileall は Boehm GC 内部 SEGV で、 pystro 側ではなく Boehm
heap corruption が疑われる。

**修正:**

- **`gc.collect()` を Boehm `GC_gcollect()` に橋渡し** (これまで no-op):
  test_set フル実行時のピーク RSS が 3.3GB → ~100MB。
- **dict/set に version カウンタ + iterator snapshot** (bpo-46615 regression
  test 対応): 反復中の mutation で SEGV ではなく
  `RuntimeError("Set changed size during iteration")` を上げる。
- **`pydict_indices_lookup` / `pydict_find` で `pys_eq_bool` 後に
  `c->state == RAISE` をチェック**: __eq__ が例外を上げた後 lookup ループを
  回り続けて memory corruption に至る経路を塞いだ。
- **`pys_iter_init` の defensive default**: 非 iterable で TypeError を
  上げるとき kind/end を 0 に初期化。 caller が state を見ずに
  `pys_iter_next` を呼んでも空 iter として false 返却するだけに。
- **`sm_difference_update` / `sm_intersection_update` / `sm_set_update` /
  `sm_symmetric_difference_update`** で `argv[0]` を `pys_unwrap_primary`
  経由に変更 (SetSubclass instance を self に取った場合の SEGV)。
- **`pys_display` の visit-stack push/pop を pushed フラグで対称化**:
  64 段以上のネストで push が抜けて pop だけ走り visit_top が負になり
  SEGV する経路。 pushed フラグで均衡化。 また `__repr__` raise 後の
  pys_is_str(NULL) deref を回避。
- **`pys_list_slice` の alloca → GC_malloc**: 大きな slice で stack
  overflow。 また `(b - a + st - 1) / st` の overflow を回避する形に
  n 計算を書き直し。
- **`pys_cmp` / `lm_sort` で raise 伝播**: __lt__ や key 関数が raise
  したあと NULL を比較し続けて SEGV する経路を、 cmp 後に state 確認
  して即 return。

**結果 (file-level):**

- 安定 PASS = **19** (test_userdict が新たに pass)
- **test_userdict**: SEGV → `OK` (全 25 tests pass) ✓
- **test_list**: SEGV → `FAILED (failures=13, errors=6, skipped=1)` 完走
- **test_tuple**: SEGV → `FAILED (failures=6, errors=2, skipped=4)` 完走
- **test_userlist**: SEGV → `FAILED (failures=6, errors=1)` 完走
- **test_set**: 169 fails+errors → **68 fails+errors** (101 件回復)
  `FAILED (failures=31, errors=37, skipped=2)`
- **test_dict**: SEGV → `FAILED (failures=12, errors=18, skipped=13)`
  完走 (libgc 内部 SEGV は GC_INITIAL_HEAP_SIZE 128MiB で解消)。

**追加した shim / 機能拡張:**

- `list.__delitem__` (bi_dunder_delitem ↔ bi_pystro_del bridge)
- `list.__iadd__` (extend + return self、 raise propagation 付き)
- `reversed(list/tuple)` を proper PYS_T_ITER で返す
- set / frozenset binop の left-type 維持 (`pys_make_set_like`)
- set 系 binop で SetSubclass instance を `pys_unwrap_primary` 経由に統一
- `set.union/intersection/difference` を `*others` 多引数対応
- `dict.fromkeys` を classmethod として登録 (instance bind されないように)
- `iter.__length_hint__` (PEP 424)
- `set.remove` で `KeyError(key)` を投げる (args[0]=key)
- `sm_symmetric_difference` で raise propagation + 双方 materialize
- `sm_intersection` / `sm_difference` で iterator を一度 set へ drain
  (gen を pys_contains に複数回渡すと exhaust する問題)
- `pys_contains` で set→frozenset 変換 lookup
  (`set() in {frozenset()}` を CPython 仕様に揃える)
- 比較演算子 (lt/le/gt/ge) の reflected dunder fallback
  (`set < instance` で `instance.__gt__` を試す)
- `hash(int) == int` (CPython 仕様、 -1→-2 special case)
  これまで golden ratio 乗算で `hash(1) ≠ 1` だったため、 ユーザの
  `__hash__: return 1` が int の bucket と衝突せず __eq__ が呼ばれない
  状態。

**ヒープ・SEGV 系:**

- `GC_INITIAL_HEAP_SIZE` デフォルトを 64MiB → 128MiB に変更。 64MiB だと
  test_dict 等で heap expansion が走り、 Boehm 直後の mark phase で
  NULL deref する flake が頻発していた。 expansion せずに済む大きさで
  Boehm bug を回避。
- `bi_dict_fromkeys` の cls-shift 条件を `argc>=2` に厳しく
  (`dict.fromkeys()` が argv[0] = cls 一つだけのとき shift で argc=0
  になり undef memory deref)。
- node.h に runtime extern 宣言群を追加。 これまで node.def → node_eval.c
  から runtime 関数を呼ぶときに implicit-int で 32bit truncate されて
  bogus pointer 経由で SEGV する経路があった (node_lt rich comparison
  で実害発覚)。

**raise propagation 系:**

- `pys_eq` で `pys_try_binop_dunder` 後の RAISE 確認
- `pys_eq` の dict / set 比較ループで `pys_dict_has`/`pys_eq` 後の
  RAISE 確認
- `node_lt/le/gt/ge` で `pys_cmp` 後の RAISE 伝播 (lambda 経由で
  swallow されていた経路)

### 続き: 比較演算子と hash の CPython parity

- `node_lt/le/gt/ge` で `pys_cmp` 後の state==RAISE 伝播
  (lambda 経由で swallow されていた経路)。
- 比較演算子に reflected dunder fallback
  (`set < instance` で `instance.__gt__` を試す)。
- `hash(int) == int` (CPython 仕様、 -1→-2)。 これまで golden ratio 乗算で
  `hash(1) ≠ 1` だったため、 user の `__hash__: return 1` が int の
  bucket と衝突せず __eq__ が呼ばれなかった。
- **pys_hash_slow を 62bit non-negative にマスク**:
  FNV-1a で uint64 (高位 bit set) を返すと PYS_FIX overflow で
  truncation。 ユーザの __hash__ 経由 hash と直接 pys_hash 経由 hash が
  異なって dict lookup 失敗。 62bit mask で round-trip 一貫。
- **str subclass で primary の hash を継承**: `dict[StrSub('key3')] ==
  dict['key3']` が機能するように。

### List / Tuple / Dict の追加修正

- list: `__delitem__` / `__iadd__` / `__init__` shim、 `reversed()` を
  proper PYS_T_ITER で返す、 mutation-safe `index`/`remove`/`count`/
  `__contains__` (bpo-38610 regression: __eq__ で list を clear して
  も SEGV しない)、 type check で `TypeError "list indices must be ..."`、
  slice の `n` overflow 修正。
- set: 多引数 `union/intersection/difference`、 left-type 維持 + Subclass
  unwrap、 `set.remove` で `KeyError(key)`、 `sm_symmetric_difference`
  の materialize + raise、 generator materialize、 set-in-frozenset 変換
  lookup、 set.remove/discard で unhashable set→frozenset 変換。
- dict: `KeyError(key)` で args[0]=key、 mapping protocol (.keys() +
  __getitem__)、 dm_getitem/dm_delitem/dm_pop の raise propagation、
  `fromkeys` で subclass instance を構築。
- tuple: `index` / `count` の mutation safety + raise propagation。
- iter: `__iter__` / `__next__` / `__length_hint__` shim (PYS_T_ITER 用)。

### for-loop / repr / pys_to_repr の raise 伝播

- `node_for_local / node_for_global` で `__next__` raise 後の
  else-clause 突入を防ぐ。
- `pys_display` の visit-stack push/pop を pushed フラグで対称化、
  `__repr__` raise 後の NULL deref を回避。
- `pys_to_repr` 末尾 bottom path で memstream 完了後の state==RAISE 確認、
  partial 文字列を返さない。
- **UserDict.__repr__ を CPython 互換 (`repr(self.data)`)** に変更。

### Hash / SEGV / 構造

- `GC_INITIAL_HEAP_SIZE` 64MiB → 128MiB (Boehm の heap-expansion bug 回避)。
- `bi_dict_fromkeys` の cls-shift 条件を厳しく (argc=1 で argv[0]=cls で
  empty 引数 deref を回避)。
- **node.h に runtime extern 宣言群を追加**: node.def → node_eval.c から
  runtime 関数を呼ぶときに implicit-int truncation で bogus pointer 経由
  SEGV する経路があった (node_lt rich comparison で実害発覚)。

## sweep の現状 (2026-05-09)

### File-level 合格

ファイル単位で `rc=0` を返す test_*.py が **18 件 (安定)** に到達:

```
test__osx_support test_bigaddrspace test_colorsys test_contains
test_copyreg test_eintr test_embed test_errno test_int_literal
test_keyword test_lltrace test_osx_env test_sundry
test_type_annotations test_typechecks test_unary test_urllib_response
test_xml_dom_minicompat
```

- 8 件は all-skip (`OK (skipped=N)`): _osx_support (16) / bigaddrspace
  (6) / embed (71) / osx_env (1) / type_annotations / eintr / errno /
  lltrace (3)。 環境前提を満たさず skip だが unittest が走り
  SystemExit がきれいに 0 終了する。
- 10 件は実 assertion をパス: colorsys (7) / contains (4) / copyreg
  (6) / int_literal (6) / keyword (11) / sundry (1) / typechecks (6) /
  unary (6) / urllib_response (4) / xml_dom_minicompat (11)
  → 計 62 件。

以前 ~70% の頻度で exit-time に segv していた test_errno / test_typechecks /
test_bigaddrspace / test_sundry は GC 初期化を `setenv("GC_INITIAL_HEAP_SIZE",
"64MiB")` 経由に変更することで安定化。 これは Boehm GC の post-init
`GC_expand_hp` 経路での heap-corruption window を回避したもの。

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

