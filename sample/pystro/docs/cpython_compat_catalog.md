# cpython_compat_catalog.md — CPython stdlib 非互換カタログ

policy: pystro は CPython 3.12.13 (submodule pin) の純 Python stdlib
(`cpython/Lib/`) を **PYTHONPATH 経由でそのまま使う**。 互換性が取れない
箇所だけ pystro 側 (`stdlib/` または C runtime) で吸収する。

このドキュメントは sweep (`for t in cpython/Lib/test/test_*.py;
do ./pystro $t; done`) で出る非互換を分類し、 修正方針を整理する。

## sweep の現状 (2026-05-09)

最新の sweep スクリプト (`/tmp/pystro_sweep/run.sh`) は ファイル単位で
exit code を見るため、 1 件でも fail があると "crash" にカウントされる:
```
total=406  pass=0(file)  mixed=0  crash=375  parse_err=22  import_err=9
```

ただし test ファイル単位の exit は 厳しすぎる指標。 **個別 unit-test 単位**
で見ると、 42 個の test ファイルが unittest 実行に達し (即 ImportError /
SyntaxError で死なずに `Ran N tests` を出力する状態)、 その中で
**287 個の個別 unit-test が pass している**:

```
ran_count=42
total_individual_pass=287
```

例: `test_abc.py` は `Ran 72 tests in 0.211s, FAILED (failures=41, errors=18)`
→ 13 件 pass。 セッション開始時は file の load すら出来なかった所から
ここまで届くようになった。

## このセッションでの主な改善

- `super(C, cls).__new__` (CPython の `super(partial, cls)` 形式) → 200+
- `bytes / bytearray.copy` → 27
- `math.lgamma / gamma / erf / erfc` (Lanczos approx) → 17
- `int.from_bytes(iter)` map イテレータ受理 → 35+
- `super(): instance OR subclass` → 4 件 (functools.wraps + dataclasses)
- `__init_subclass__` 暗黙 classmethod 化 → 9
- class body 名前解決を MRO traverse しない → ~10 件 (selectors, etc.)
- `type.__dict__['__mro__'] / ['__bases__'] / ['__dict__']` 合成 descriptor
  → inspect が import 出来る → 13+
- `float.__getformat__` → test.support load 可
- `__pystro_stat__` builtin (posix.stat 実装) → test.support / sysconfig
  全パス
- `_simple_enum` 実装 (auto() を sequential int に rewrite) → ast.py
  `_Precedence.NOT` 等が使える → 160+
- 例外 instance に必ず __traceback__ / __context__ / __cause__ /
  __suppress_context__ / msg / lineno / filename / errno / strerror /
  name / path / encoding / object / start / end / reason / obj を 設定。
  StopIteration 含む。
- traceback chain (TracebackType-like / FrameType-like) を 例外時に
  生成 (tb.tb_frame.f_code.co_name で walk 可能) → ~16
- bundle: `_struct.py` (struct.{calcsize, pack, unpack, ...}) → 34
- bundle: `_random.py` (xorshift Random for `random.py`) → 20
- bundle: `_typing.py` (typing が import 出来る、 完全動作はせず)
- bundle: `socket.py` (constants + non-functional socket()) → 15+
- bundle: `sysconfig.py` (`_sysconfigdata_*` 不要)
- bundle: `_thread.py` の RLock 真の reentrant 実装 → logging が動く
- bundle: `hashlib.py` shake_256 / sha1 / sha3_* / blake2 alias →
  全 hash type が `hashlib.new(...)` できる → 14
- bundle: `binascii.b2a_base64 / a2b_base64 / b2a_qp / a2b_qp` →
  email.header chain
- pystro_first list: `signal.py / importlib.py / sysconfig.py /
  hashlib.py / socket.py` を bundle に migrate
- `os.register_at_fork`, `WNOHANG`, `SEEK_*`, `O_PATH` 等 posix
  constants → 18 件
- `sys.platlibdir`, `sys.flags`, `sys.float_info`, `sys.int_info`,
  `sys.hash_info`, `sys.intern`, `sys.getrecursionlimit` 等 →
  sysconfig が import 出来る → 30+

## 残ってる top blocker (sweep 数字付き)

| 件数 | エラー | 原因 |
|---:|---|---|
| 130 | (no error line) | unittest fail / exit のうち分類不能 |
| 28 | TypeError: unsupported operand type(s) for & | ipaddress.py の IPv6Network init で `int(self.netmask)` が None を返す pystro バグ (再現は ipaddress.py 内のみ; getattr() / vars() 経由なら正しい値) |
| 15 | inspect.CO_GENERATOR 欠落 | inspect.py が `mod_dict["CO_" + v] = k` で globals() を mutate しても 反映されない (globals() copy を返すため) |
| 11 | test.multibytecodec_support | cpytest_stubs に shim 追加が必要 |
| 9 | test.support.script_helper | zipfile chain (& bug が原因) |
| 8 | asyncio.staggered | asyncio package を package として扱えない |
| 6 | unittest.mock | mock import で core dump (asyncio.iscoroutinefunction 経由) |
| 6 | super().__init__ | typing.py 等 / NetworkInterface chain で多重継承 super 欠落 |

## 主要な未解決バグ

### ipaddress `int(self.netmask)` が None を返す (28 件 / blocking script_helper / zipfile chain)

再現条件: ipaddress.py を import した中で IPv6Network('fe80::/10') を
作る。 IPv6Network.__init__ の `int(self.netmask)` が None を返す。

確認できたこと:
- `self.netmask._ip` は正しい bignum (0xffc0...0) を持つ
- `vars(self.netmask)['_ip']` は正しい bignum
- `getattr(self.netmask, '_ip')` は正しい bignum
- しかし `__int__` body の `return self._ip` の `self._ip` は None
- `self.__dict__['_ip']` は別の bignum (network_address._ip) — 共有された?
- 同じ class 階層を ipaddress.py 外で再現してもバグは起きない

仮説: pystro の attribute access specialization (shape_version) と
__slots__ + 多重継承 + bignum の組み合わせで 起きる。 修正には
specialization invalidation のデバッグが必要。

### globals() が live でない (15 件)

`inspect.py` の `mod_dict = globals(); mod_dict["CO_" + v] = k` が
反映されない。 `bi_globals` は dict snapshot を返す。 修正には
真の write-back proxy が必要、 もしくは `__pystro_set_global__` 
builtin を inspect 用に injection。

### `from foo import *` が module 以外を拒否

例: `from os import *` で OSError 等の dunder が pull されない。

