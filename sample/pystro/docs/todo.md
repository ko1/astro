# todo.md — pystro 残作業

実装済みは [done.md](./done.md)、ベンチは [perf.md](./perf.md)、
ランタイム解説は [runtime.md](./runtime.md)、言語仕様は
[spec.md](./spec.md) を参照。

---

## 現状スナップショット (2026-05-08, R18 + Phase 8)

### テスト

```
$ make test
... 213 tests OK ...
passed=213  failed=0  total=213
```

CPython 公式テスト sweep:

```
total=394  pass=28  mixed=322  crash/timeout=18  parse_err=22  import_err=4
```

### ベンチ ([perf.md](./perf.md) より)

5 条件 best-of-5:

| ベンチ | py3.12 | py3.14 | py3.14+JIT | pystro AOT |
|---|---:|---:|---:|---:|
| richards     | 1.07 s | 0.90 s | 0.84 s | **0.48 s** |
| crypto_pyaes | 0.56 s | 0.49 s | 0.54 s | **0.40 s** |
| deltablue    | 0.17 s | 0.15 s | 0.16 s | **0.14 s** |
| raytrace     | 0.91 s | **0.68 s** | 0.72 s | 0.86 s |

- vs CPython 3.12: macro **4/4** + micro 9/10
- vs CPython 3.14: macro **3/4** (raytrace で逆転) + micro 9/10
- vs CPython 3.14 +JIT: macro **3/4** + micro 9/10 (JIT は本規模では
  中立 or 微悪化)

Phase 4〜7 で IC 系を整備、 Phase 8 で deltablue/pyaes/raytrace を
詰めた。 残作業は **raytrace の 3.14 逆転を取り戻す** (minor operator
dunder の slot 化)、 dict_bench の str-key 専用パス、 仕様準拠の
積み残し。 比較分析の詳細は [`vs_cpython.md`](./vs_cpython.md)。

---

## 残課題

### 優先度 高

#### parse error 残り 22 件

CPython テスト sweep でまだパースできない構文。 ファイル別:

- `test_pprint.py:1` — `<INDENT>` (たぶん大きな triple-quoted string)
- `test_asyncgen.py:1815` — `(i * 2 async for i in arange(n))` 形式の async genexp
- `test_coroutines.py:1911` — async setcomp `{i + 1 async for i in ...}`
- `test_typing.py:1605` — `class I(*Ts):` の中で更に PEP 646 syntax
- `test_descr.py:1074` — `(1).__class__ = MyInt` (literal の attr に assign)
- `test_super.py:54` / `test_generators.py:59` — nonlocal `__class__` /
  nonlocal の binding 探索失敗
- `test_listcomps.py:433` — `[1 for (l[0], l) in [[1, 2]]]`
  (comprehension target に subscript / chained tuple)
- `test_with.py:672` — `with cm as list(targets.values())[0][1]:`
  (with-as に call 含む trailer)
- `test_patma.py:732` — `case <bytes literal>` pattern
- `test_tstring.py:145` — t-string の高度形式 (PEP 750)
- `test_tokenize.py:3018` / `test_strptime.py:396` /
  `test_socket.py:1975` / `test_venv.py:777` — invalid assignment
  target / 不規則な NAME tuple target

それぞれ単発の文法案件。 後続の作業で 1 件ずつ。

#### 共通失敗カテゴリ (mixed の中)

| カウント | 失敗メッセージ | 主因 |
|---:|---|---|
| 65 | `<class 'TypeError'> not raised` | 各種演算で TypeError を上げ忘れ |
| 36 | `don't know how to disassemble object objects` | dis module の bytecode 寄り (CPython 内部依存) |
| 27 | `<class 'SyntaxError'> not raised` | pystro が CPython では SyntaxError な構文を受理 (`(a,b) += 1` 等) |
| 26 | `0` | assertEqual で生 `0` を期待してる箇所 (個別 trace 必要) |
| 20 | `'NoneType' object has no attribute 'items'` | configparser 内部 |
| 17 | `object is not callable` | descriptor 周りの edge case |
| 15 | `unsupported operand type(s) for *` | str * negative_int / NotImplemented chain |
| 13 | `expected an integer index` / `<class 'ValueError'> not raised` | bool/int 系のエラー条件 |
| 12 | `maximum recursion depth exceeded` | cyclical repr (recursion guard 未実装) |
| 12 | `'object' object has no attribute '__next__'` | iter() が non-iter object を返す path |

それぞれ「単発で対処」or「設計レベルの差分」。

#### `<class 'SyntaxError'> not raised` を減らす

pystro の parser は意図的に許容範囲が広い。 例えば `(a, b) += 1` を
受理してから runtime で TypeError にしている。 CPython は SyntaxError。
parse 時の検証強化は段階的に。

### 優先度 中

#### S-12. `re` (正規表現) — astrorge integration

- `sample/astrorge` が成熟したら integrate (memory:
  `project_regexp_astrorge`)。
- 自前で書かない方針。
- 現状: alternation `|`、 char class、 `*+?` quantifier、 `^$` anchor、
  group `(...)`、 lazy `*?+??`、 IGNORECASE は対応。
  `{n,m}` brace quantifier、 lookaround、 後方参照、 Unicode property は未対応。

#### raytrace を CPython 3.14 から取り戻す

3.14 が 3.12 比で 25% 速くなり (Tier 2 uops + Vector arithmetic
specialization と推測)、 pystro AOT (0.86 s) が 3.14 (0.68 s) に
1.27× 負ける。 攻め所:

- minor operator dunder (`__truediv__` / `__neg__` / `__rmod__` /
  `__matmul__` 等) を slot 化。 現在 MRO walk + strcmp に落ちている
- per-pixel の Vector alloc を pool 化 (毎回 GC_malloc を回避)
- `__init__` の attr_set 経路をさらに削る

詳細は [vs_cpython.md](./vs_cpython.md)。

#### dict_bench で python3 を抜く

micro 唯一の負け (vs 3.12 で 1.31× slow、 vs 3.14 で 1.23× slow)。
CPython は str-key 専用 layout など細かい最適化を持っていて、
generic open-addressing 1 種の pystro では届かない。 改善案:
- str-key 専用 dict layout 追加 (CPython 風)
- str-key の hash を `pyobj.str` 内 cache、 strcmp の代わりに
  pointer-compare でショートカット
- `py_dict_set` / `pydict_find` を `static inline` 化
- `PYSTRO_BI_KWC` save/restore (metaclass __call__) の overhead 削減

#### minor operator dunder の slot 化 (raytrace 残存)

主要 dunder 24 種は既に slot 化済み。 残る `__truediv__` /
`__rmod__` / `__matmul__` / `__floordiv__` 等が MRO walk + strcmp に
落ちる。 pyclass struct を肥らせない方法:

- slot を別 struct に切り出して pyclass→slot table へポインタ 1 個
  だけ持たせる (struct pyclass 自体は膨らまない)
- perfect hash dispatch — `PYSTRO_INTERN_*` の固定アドレスを bucket
  化して 1〜2 compare で slot 解決

raytrace でさらに 5% 程度削れる見込み。

#### bytes 操作のさらなる削減 (pyaes 残存)

Phase 8 で `arr[i]` fast path、 `lst += [...]` の in-place 化、 GMP
fixnum 経路撤去で 5.2× → 1.46× faster になった。 残る memmove / memset
は libc + Boehm の GC alloc 経路で SD から触れにくい。 撤去するには
ASTro 全体の GC 方針変更 (`idea.md`) が必要。

#### chained-raise propagation の overhead 削減

R18 で `raiser().attr` の例外伝播を直したぶん、 hot な `node_attr_get` /
`node_subscript_get` / `node_method_*` に state check が増えた。 fib /
tak で 5〜15% の overhead。 削るなら ASTroGen の dataflow analysis で
省略できる箇所を inline 化。 v0 では trade-off を受け入れる。

### 優先度 低 (持たない)

- `async` / `await` の真の非同期 (sync 通すフォールバックで済ます方針)
- C extensions / `ctypes` / `multiprocessing` / 真の thread
- `__slots__` による属性制限 (declared だが ignore)
- 完全な PEP 695/646 (TypeVarTuple / ParamSpec / Concatenate の semantics)
- CPython 内部 C 拡張依存モジュール (`dis` の disassemble、 `_lsprof`、
  真の `_socket` 機能、 `pyexpat`、 `_ssl`、 `ctypes` 等)

---

## 設計上の妥協 (変えない)

- Boehm GC (refcount + cycle collector ではない)
- インデント = スペース推奨 (タブ = 8 spaces)
- decorator は `def` / `class` 上のみ
- C3 MRO 不整合は BFS フォールバック
- string slice は buffer 共有 (slice borrow)
- set は (Python 仕様も) unordered

---

## 完了項目アーカイブ (R1〜R18)

R0〜R18 で **0 → 213/213 internal tests + 28/394 CPython
fully-pass** + 322 mixed まで来た。 詳細は [done.md](./done.md) の
各節を参照。

| ラウンド | 主トピック |
|---|---|
| R1 (5/5) | A1〜F3: arith_cache, del NAME, generator method, import dotted, bytes/bytearray, クラス属性, starred target, f-string conv, % formatting, dict 挿入順, etc. |
| R2 (5/5) | dict 0.49→0.78× 改善 (compact dict, attr inline cache, traceback, REPL) |
| R3 (5/5) | except as e env restore, file I/O, eval/exec, int(s,base), str/dict methods, multi-CM with, raise from, f-string `{x=}`, functools/operator/copy/enum/random/string/pathlib/io |
| R4 (5/5) | type identity, descriptor protocol, dataclasses, argparse |
| R5 (5/5) | complex literal + 型, class M(list) |
| R6 (5/5) | asyncio sync stub, typing, metaclass kwarg, pickle, hashlib |
| R7 (5/5) | super value, true metaclass (M(name, bases, attrs)) |
| R8 (5/5) | int/float/etc を真の class object に, isinstance(int, type), pyclass.builtin_ctor |
| R9 (5/5) | M-6 完全実装 (built-in subclass + primary), property setter, class body name lookup, docstring, with full protocol, set ops, `__contains__`/`__delitem__`/`__neg__`/`__abs__` dunder, decimal/fractions/threading/logging, dict `\|`, lazy enumerate/zip/map/filter |
| R10 (5/5) | inline flonum (CRuby 流 3-bit rotate), 算術ノード fast path (mandel 2.6×, nqueens 1.8×), node_eq fixnum fast path |
| R11〜R16 (5/5〜5/6) | string slice buffer 共有, dict identity-eq, NotImplemented chain, math dunder dispatch, bytes/bytearray ops, dataclass(frozen/eq/order), match patterns, statistics extras, 多数のエッジ fix |
| R17 (5/6) | UTF-8 codepoint str, `@` matmul, async sync, eval/exec ns dict, PEP 604 union, collections.abc + typing.Generic, metaclass `__instancecheck__`, lazy genexp, function annotations, PEP 654 `except*`, PEP 695 type alias |
| R18 (5/7) | CPython テスト互換 (28 fully-pass)、 chained-raise propagation, PYSTRO_BI_KWC leak fix, __class_getitem__ classmethod unwrap, types.MethodType constructible, ABCMeta `_abc_registry`, parser 拡張多数 (PEP 646 starred, multi-element subscript with slice, for-target trailer, chain assign 256), 25+ stdlib stub modules |
| Phase 8 (5/8) | macro 4/4 で python3 を上回る。 fixnum subscript/mod/floordiv fast path, `node_iadd` (list `+=` の in-place 化), attr_cache に instance-receiver class-attr slot (`inst_ca_*`), `pyclass.fast_new` flag, classmethod IC, **parser LHS の `dot+call` fuse** (deltablue 4×) |
