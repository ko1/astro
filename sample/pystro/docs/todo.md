# todo.md — pystro 残作業

実装済みは [done.md](./done.md)、ベンチは [perf.md](./perf.md)。
優先度順 (上 = 既存実装のバグ / 看板倒れ、下 = 未着手 / 性能 / 未確証)。

## A. 既存実装のバグ・看板倒れ (4 件)

| # | 項目 | 内容 |
|---|---|---|
| A1 | **`arith_cache @ref` が看板倒れ** | 実装したのは `py_try_binop_dunder` の早期 return だけ。ascheme 流の "last-seen types を覚える + override 監視 + invalid 化" の inline cache 本体は未実装 |
| A2 | **generator + try/except の相互作用** | `yield` を含む `try:` ブロックがあって、caller 側が別の try に入ると `c->try_top` が混線。gen swap 時に try_stack を save/restore してない |
| A3 | **`del NAME` の local が真の unbind ではない** | スロットに PY_NONE を書くだけ。`del x; print(x)` は None と表示 (Python なら NameError)。フレームスロットに valid bit を持たせるか、特殊な "undefined" sentinel が要る |
| A4 | **循環参照コンテナの display で stack overflow** | `a = []; a.append(a); print(a)` が無限再帰。`py_display` に visiting set (見ている pyobj* の集合) を持たせて自己参照を `[...]` 表示する |

## B. 部分実装で穴がある機能 (4 件)

| # | 項目 | 不足 |
|---|---|---|
| B1 | **`match` パターン** | mapping パターン `{"k": pat}` と class-with-attrs `Point(x=0, y=y)` 未実装。class は isinstance チェックのみで属性照合してない |
| B2 | **generator のメソッド** | `g.send(value)` / `g.throw(exc)` / `g.close()` 未実装。`next(g)` と `for x in g` のみ |
| B3 | **`import` 文** | `import a.b.c` (dotted) / `from a.b import c` / `from m import *` 全部未対応。モジュール検索パスは CWD 固定 (`sys.path` のような仕組みなし) |
| B4 | **bytes / bytearray** | `.decode()` / `.encode()` / `.find()` / `.split()` / `.startswith()` 等の主要メソッドが無い。indexing (→ int) と iter (→ int 列) のみ |

## C. 完全に未着手の言語機能 (5 件)

| # | 項目 | 内容 |
|---|---|---|
| C1 | **クラス属性 (非メソッド)** | `class C: x = 5` の `x` を class オブジェクトに保存する仕組みが無い (`def` のみ class.methods に登録)。class body 評価中に `make_store` を class dict に向けるルートが必要 |
| C2 | **starred target** | `a, *rest = [1, 2, 3, 4]` での `*pat`、関数引数定義側の bare `*` (kwonly 区切り) は OK だが代入側は未対応 |
| C3 | **f-string conversion** | `{expr!r}` / `{expr!s}` / `{expr!a}` のサフィックス付き変換 |
| C4 | **文字列フォーマット** | `"%d %s" % args` 演算子と `"...".format(...)` 未実装。f-string と builtin `format(value, spec)` のみ |
| C5 | **dict 挿入順保持** | Python 3.7+ は挿入順を仕様化。pystro はバケット順 (open-addressing 順) — 順序依存コードは壊れる |

## D. ライブラリ・ランタイム (1 件、規模特大)

| # | 項目 | 内容 |
|---|---|---|
| D1 | **標準ライブラリ** | `sys` / `os` / `math` / `json` / `re` / `collections` / `itertools` 等が一切ない。pure-Python で書けるものはユーザモジュールでなんとかなるが、`open` / `os.path` / `re` の後ろ盾は C 側で要実装 |

## E. 性能 — 残ボトルネック (2 件)

| # | 項目 | 期待効果 |
|---|---|---|
| E1 | **`dict_bench` 0.92×** | CPython の dict は数十年磨かれた C コード。pystro が並ぶには str-key 専用 layout (`PyDictKeysObject` 風)、PyObject_Hash の専用パス、サイズ別 layout 等が要る |
| E2 | **真の `arith_cache`** (= A1 の本実装) | hot 数値ループで dunder lookup 経路を完全に排除。AOT 後に SD 内に直接 fixnum/flonum 経路が畳まれるよう @ref で baked-in |

## F. 未テスト・たぶん動くけど確証なし (3 件)

| # | 項目 | 注意 |
|---|---|---|
| F1 | **generator の中の generator** (ネスト) | `prev_gen` チェーンは pygen に入れたが、ネスト generator + 互いに `yield from` する経路は踏んだテストが無い |
| F2 | **`import` 中の例外伝播** | `bi_import` がモジュール body の RAISE を NORMAL に戻して PY_NONE を返す実装。本来は import の caller に再 raise すべき |
| F3 | **ユーザクラスの `__hash__` override** | `py_hash` の instance ケースは address-based。`__hash__` の dunder lookup → 呼び出し → int 変換のパスを入れる |

---

合計 **18 項目**。

## 既存の妥協 / 制限 (仕様としての設計判断)

- GC は Boehm-Demers-Weiser。CPython の refcount + cycle-collector ではない (`__del__` の即時実行は未保証)
- 例外 traceback は class 名 + message のみ (フレームスタック表示なし)
- 整数オーバーフロー時に float に逃げない — bignum 昇格 (Python 3 と一致)
- インデント: スペース推奨。タブは 8 スペース換算 (`tabnanny` 級チェックなし)
- REPL なし。`-e <code>` か file 実行のみ
- decorator は `def` / `class` 上にのみ。式 `(a := dec(b))` のような修飾は不可
- C3 MRO の不整合検出: 厳格に MRO error を投げる代わりに BFS フォールバック (Python は TypeError)
