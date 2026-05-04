# todo.md — pystro 未実装 / 今後

実装済みは [done.md](./done.md) を参照。

## 言語機能 — 未実装

### 関数 / クロージャ
- ネストした `def` での自由変数 capture (parser 側のみ未対応)
- `nonlocal` 宣言
- `*args` / `**kwargs`
- キーワード引数 `f(x=1)`
- 装飾子 `@dec`
- 多重代入 `a = b = expr` (parser 側の限定 todo)

### 制御フロー / 例外
- `with` (context manager)
- `else` 節 (`for ... else:` / `try ... else:`)
- 例外チェイン `raise A from B`
- `raise` での tracebacks (現状は class 名 + message のみ)

### コンテナ
- list / dict 内包表記 `[x*x for x in xs if x > 0]`
- set / frozenset 一切
- list slicing 代入 `a[1:3] = [...]`
- dict ordered 反復 (現状はバケット順)
- `bytes` / `bytearray`

### クラス
- 多重継承 / MRO (現状は base 1 段のみ)
- `super()`
- classmethod / staticmethod / property
- `__repr__` / `__str__` / `__eq__` / `__hash__` / `__add__` 等の dunder 連動
- メタクラス
- `__slots__`

### モジュール
- `import` 文は parser ではスキップ済み (no-op)、本物の機能は未着手
- 標準ライブラリ無し

### イテレータ / ジェネレータ
- `yield` / generator function
- `iter()` / `next()` プロトコル (組み込みは内部 `py_iter` 経由のみ)

### その他
- 文字列フォーマット `"%d" % x`、`str.format`
- f-string: フォーマット指定 (`{x:.2f}` など)
- assignment expression `:=`
- 条件式の入れ子は OK だが、tuple 化なしの代入式 `(a, b) = (1, 2)` のような括弧付きは未対応

## 性能 — 投入済 / 候補

詳細な数値とプロファイル分析は [`perf.md`](./perf.md) を参照。

### 投入済 (`perf.md` §1-§8)

| # | 改善 | 効果 |
|---|---|---|
| §1 | `gref_cache @ref` | fib 5× |
| §2 | `globals_serial` を構造変化のみで bump | while 71× |
| §3 | `node_for_global` 内蔵キャッシュ | for_range 23× |
| §4 | `method_cache @ref` | list 12× |
| §5 | `py_apply` を `node.h` に static inline | fib 1.15× |
| §6 | leaf func の alloca フレーム | fib 1.5× |
| §7 | dict identity-equal fast path | dict 1.1× |
| §8 | string slice の buffer 共有 + 小サイズ alloc | string 1.6× |

結果: 全 6 ベンチで CPython 3.12 を上回り (1.04× 〜 18× 速い)。

### 候補
| 項目 | 期待効果 | 備考 |
|---|---|---|
| `arith_cache` 付き専用 arith ノード | int+int の super-hot path 高速化 | Python の operator override は常に fast (class 未実装の限り) |
| inline flonum (CRuby スキーム) | float-heavy ベンチで heap-box 削減 | mandel/nbody 入れる前に必要 |
| 小整数 singleton (-5..256) | `PY_FIX(0)/PY_FIX(1)` 多用箇所で微改善 |  |
| string interning | 短文字列の重複 alloc を削減 | string_bench を更に詰める |
| dict のサイズ別ハッシュ多変 | int キー専用 layout 等 | CPython と並ぶには工夫が要る |
| ネスト def の closure capture | nonlocal 対応 | 言語機能でもある |

## 既知の制限・妥協

- **比較連鎖 `a < b < c` で b が 2 回評価される**。Python は単一評価を保証する。
- **GC は Boehm**。CPython の refcount + cycle-collector ではない (`__del__` の即時呼び出しは未保証)。
- **エラー時の traceback がない**。`RuntimeError: msg` のような 1 行メッセージのみ。
- **大整数 / 整数除算で float に逃げない**: bignum で計算するので Python 3 と一致。ただし `**` で負の指数を渡した場合は float になる (Python 3 同じ)。
- **インデント**: スペース推奨。タブは 8 スペース換算 (`tabnanny` 級の厳格チェックなし)。
- **REPL なし**。`-e` / file 実行のみ。
