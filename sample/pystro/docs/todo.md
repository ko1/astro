# todo.md — pystro 未実装 / 今後

実装済みは [done.md](./done.md)、ベンチは [perf.md](./perf.md)。

## 言語機能 — 未実装

### モジュール / インポート
- `import name` / `from name import x` (parser はスキップ済み、no-op stub)
- 標準ライブラリなし

### 関数 / 呼び出し
- 呼び出し側 `f(*list)` / `f(**dict)` 展開
- ネストした tuple unpack target (現状 flat name のみ)

### イテレーション
- 真の iterator / generator (現状 eager — `def f(): yield ...` は list 化される)
- `iter()` / `next()` プロトコル (現状はスタブ)

### クラス
- 多重継承 / MRO (現状は base 1 段、左→右の depth-first 走査になっていない)
- `__del__` / weak refs / metaclass / `__slots__`
- 演算子の右辺 dunder 拡充 (`__rmul__` / `__rsub__` 等の片対応はまだ部分的)
- comparison 全体 (`__gt__` / `__ge__` / `__hash__` の dunder)

### 例外
- `raise A from B` (chaining)
- traceback (現状は class 名 + message のみ)

### コンテナ
- `bytes` / `bytearray`
- `frozenset` (set のみ)
- list `*` 演算子の dunder 経由 (現状 builtin で処理)

### その他
- `match` 文 (3.10+)
- `assert` 文 (parser 未対応)
- `del` 文 (一部しか対応していない)
- `*pattern` in unpack assignment
- type hints は token は受けるけど無視 (`def f(x: int) -> int:` は失敗するかも)

## 性能 — 投入済み

詳細は [perf.md](./perf.md) §1〜§N。最大の効きは:

| 改善 | 主に効くベンチ | 効果 |
|---|---|---|
| `gref_cache @ref` | fib (global 多用) | 5× |
| `globals_serial` 構造変化のみ bump | while_loop (tight augassign) | 71× |
| `node_for_global` 内蔵キャッシュ | for_range | 23× |
| `method_cache @ref` | list.append 等 | 12× |
| `py_apply` static inline | fib | 1.15× |
| leaf func の alloca フレーム | fib | 1.5× |
| dict identity-equal fast path | dict (int key) | 1.1× |
| string slice の buffer 共有 | string | 1.6× |
| inline flonum + arith ノード fast path | mandel / nqueens | 2.6× / 1.8× |
| node_eq fixnum fast path | nqueens (== 多用) | 1.8× |

## 性能 — 候補

| 項目 | 期待 | 備考 |
|---|---|---|
| dict のサイズ別レイアウト / 文字列キー intern | dict_bench を逆転 | CPython の dict v8 系に追いつく必要 |
| `arith_cache @ref` で dunder lookup を invalid 化付きにスキップ | mandel / 数値ベンチ | 現状 py_try_binop_dunder の関数呼び出しが残る |
| `node_call_cache` (関数 call site → resolved closure) | 全関数呼び出し | gref_cache の関数版 |
| 真の iterator object + lazy generator | infinite gen サポート | ucontext 必要 |
| PG (profile-guided) compile | hot path 集中時 | naruby と同じパターン |

## 既知の妥協 / 制限

- 比較連鎖 `a < b < c` で b が 2 回評価される (Python は単一評価)
- GC は Boehm。CPython の refcount + cycle GC ではない
- generator は eager (yield を list に collect)。無限 generator は不可
- 例外 traceback は 1 行 message のみ
- 整数除算で float に逃げず bignum で計算 (Python 3 と一致)
- インデント: スペース推奨。タブは 8 スペース換算 (tabnanny 級チェックなし)
- REPL なし
- decorator は def / class にのみ。式の修飾は不可
- `*list` / `**dict` 呼び出し展開未対応 (関数定義側の `*args` / `**kwargs` 受け取りは可)
