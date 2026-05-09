# cpython_compat_catalog.md — CPython stdlib 非互換カタログ

policy: pystro は CPython 3.12.13 (submodule pin) の純 Python stdlib
(`cpython/Lib/`) を **PYTHONPATH 経由でそのまま使う**。 互換性が取れない
箇所だけ pystro 側 (`stdlib/` または C runtime) で吸収する。

このドキュメントは sweep (`for t in cpython/Lib/test/test_*.py;
do ./pystro $t; done`) で出る非互換を分類し、 修正方針を整理する。

## sweep 現状 (2026-05-09)

```
total=406  pass=0  parse_err=16  crash=390
```

(`pystro_first_modules = { "types.py" }` のみ強制 override 状態。)

## カテゴリ別非互換

### A. 致命的 (1 件で多数の test を巻き込み)

#### A1. `type.__new__(meta, name, bases, dict)` が新クラスを作らない (165 件影響)

**症状**: `'_abc_registry'` 属性なしエラー。 ABCMeta(metaclass) の
`__new__` で `super().__new__(mcls, ...)` を呼ぶが、 pystro の
`type.__new__` は新規クラスを作らず `type` 自体を返してしまう。

**再現**:
```py
class M(type):
    def __new__(mcls, name, bases, ns):
        cls = super().__new__(mcls, name, bases, ns)
        cls.foo = 42  # works in CPython, missed in pystro because cls=type
        return cls
class C(metaclass=M): pass
print(getattr(C, "foo", "MISSING"))  # CPython: 42, pystro: MISSING
```

**対応方針**: pystro の `bi_type` 3-arg 形式を `type.__new__(...)`
ルートに紐づける。 もしくは `type.__new__` を独立に実装し、
`__class__ = mcls` を立てる。

#### A2. `_io.text_encoding` 欠落 (137 件)

**症状**: `unittest` / `pathlib` 等が `import _io; _io.text_encoding(None)`
を呼ぶ。

**対応**: `stdlib/_io.py` に stub 追加 (PEP 597 の text_encoding 関数)。
2-3 行で済む。

#### A3. `types.DynamicClassAttribute` 欠落 (26 件)

**症状**: `enum.property` で使われる descriptor。 pystro の
types.py に存在せず。

**対応**: `stdlib/types.py` に DynamicClassAttribute クラス追加。
(`@property` の薄い wrapper でよい)

#### A4. `_weakref._remove_dead_weakref` 欠落 (13 件)

**対応**: pystro の `stdlib/_weakref.py` に no-op stub 追加。

### B. test runner 由来 (本体不問)

#### B1. `test.support` 欠落 (16 件)

**症状**: `from test import support` が pystro の `cpytest_stubs/test/`
を見つけるはずだが load 失敗。

**対応**: cpytest_stubs の test package が module 化されているか確認。

#### B2. `test.multibytecodec_support` (11 件) / `test.list_tests` (1 件) /
        `test.mapping_tests` (1 件)

**対応**: cpython/Lib/test/ から該当ファイルを stub したいが、 大きい。
影響テストを skip する方針もあり。

### C. parser 限界 (16 件)

(これは sweep の "parse_err" カテゴリ)

- 6 件: PEP 646 starred type / async genexp / 複雑な parser 限界
- 4 件: `(a, b) += 1` 系 (既に SyntaxError 化済み)
- 残り: 個別の構文 edge case

詳細は `todo.md` を参照。

### D. Module-specific minor

- `importlib._bootstrap` 派生 (4 件): pystro の importlib stub 拡張。
- `encodings.aliases` (4 件): `cpython/Lib/encodings/__init__.py` が
  `from . import aliases` を期待するが、 pystro が見つけられない。
- `_warnings.filters` (3 件): warnings list を attr で expose。
- `_locale.RADIXCHAR` (1 件): locale 定数追加。
- `multiprocessing.context` (1 件): MP は持たない方針なので skip。
- `xml.sax.handler` (1 件): XML 関連 stub 拡張。

## 修正の段階

### Phase 1: 巨大効果 (優先度 高)

- [x] **A1a**: `type.__new__(mcls, name, bases, dict)` 4-arg 形式を bi_type
  に forward (`bi_type_new`)。 metaclass 経由の class 構築が動くように。
- [ ] **A1b**: `super().__new__(mcls, ..., **kwargs)` の `**kwargs` spread
  が pystro super 経由で壊れる。 `super().method(*args, **kw)` 全般で
  `TypeError: object is not callable`。
  → 暫定対処: `pystro_first_modules` に `abc.py` / `_py_abc.py` 追加。
  根本: pystro の super 実装で `**kwargs` spread を直す必要あり。
- [x] **A2**: `_io.text_encoding`
- [x] **A3**: `types.DynamicClassAttribute`
- [x] **A4**: `_weakref._remove_dead_weakref`

### Phase 1.5: 続けて出てきた blocker

- [ ] **`async def` の真の coroutine** (現在は fake coroutine)。 CPython
  test の中で `coro.send(...)` `await coro` を実行する箇所が失敗する。
- [ ] **`f.__closure__` が真の cell に bind してない**。 captured value は
  snapshot で、 cell 経由の writeback が効かない。
- [ ] **`type(x)` のクラス取り出しが metaclass 経由で正しいか** —
  CPython の `_DeprecateByteStringMeta` のような特殊 metaclass を持つ
  class の生成で TypeError。 まだ深掘りせず。

### Phase 2: test runner 整備

- [ ] **B1, B2**: cpytest_stubs の整備

### Phase 3: minor stubs

- [ ] **D**: 個別追加

### Phase 4: parser edge

- [ ] **C**: 残り 8 件くらいの parser 問題
