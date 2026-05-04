# done.md — pystro 実装済み

ASTro 上の Python サブセット。本書は **動く言語機能** を一覧する。
未実装は [todo.md](./todo.md)、ランタイム解説は [runtime.md](./runtime.md)。

## テストスイート

```
$ make test
  OK  test/00_basic.py
  OK  test/01_compare.py
  OK  test/02_control.py
  OK  test/03_func.py
  OK  test/04_string.py
  OK  test/05_recursion.py
  OK  test/06_bignum.py
  OK  test/07_containers.py
  OK  test/08_class.py
  OK  test/09_exception.py
  OK  test/10_func.py
---
passed=11  failed=0  total=11
```

AOT モード (`./pystro -c`) でも全 11 件 pass。

## 言語機能

### リテラル
- 整数: 62-bit fixnum + GMP `mpz_t` bignum (自動昇格)
- 浮動小数: heap-boxed double (inline flonum 化は未着手)
- 文字列 `"..."` / `'...'`、エスケープ `\n \t \r \\ \' \" \0`
- f-string `f"x={expr}"` (`{...}` 内は再帰的にトークン化 + parse)
- リスト `[1, 2, 3]`
- タプル `(1, 2, 3)`、空 `()`、単要素 `(x,)`
- 辞書 `{"k": v, ...}`
- `True` / `False` / `None`
- `0xFF` / `0b101` / `0o17`、underscore separator (`1_000_000`)

### 演算子
- 算術 `+ - * // / % ** ` (整数同士は overflow check 付き fixnum fast path、はみ出したら GMP bignum)
  - `/` は **必ず float**、`//` は floor division
- 単項 `+x` / `-x` / `~x`
- 比較 `< <= > >= == !=`、**比較連鎖** (`a < b < c`)
- 論理 `and` / `or` / `not` (short-circuit、Python 流に operand を返す)
- ビット演算 `& | ^ ~ << >>`
- `is` / `is not` (オブジェクト identity)
- `in` / `not in` (list / tuple / dict / str / range)
- 拡張代入 `+= -= *= /= //= %= **= &= |= ^= <<= >>=`
- 条件式 `x if cond else y`

### 文
- `pass` / `break` / `continue`
- `if/elif/else`
- `while`
- `for x in iter:` (list / tuple / dict / str / range)
  - flat な tuple target (`for k, v in items:`) も対応
- `def f(p1, p2=default, ...):`、`return`
- `lambda x: expr`、`lambda x, y=1: ...`
- `class Name(Base):` (継承 1 段、`__init__`、メソッド、属性 `self.x`)
- `try / except / except E as e / finally`
- `raise expr` / `raise` (再 raise)
- `global x, y` (関数内 global 宣言)
- 代入: `x = e`、`a.b = e`、`a[i] = e`
- 多重代入 (flat tuple unpack): `a, b, c = (1, 2, 3)` / `a, b = b, a`
- 式文

### スコープ
- トップレベル代入 → global
- 関数内代入 → local (parser の suite pre-scan で抽出、ネストした def の body はスキップ)
- `global x` 宣言で local シャドウを抑制

### 組み込み関数
| 名前 | 機能 |
|---|---|
| `print(*a)` | 空白区切りで各 arg を表示し改行 |
| `str(x) / repr(x)` | 文字列化 |
| `int(x) / float(x) / bool(x)` | 数値・真偽変換 |
| `len(x)` | str / list / tuple / dict 長 |
| `abs(x)` | 絶対値 (int/bignum/float) |
| `range([s,] t [,step])` | 整数 range |
| `list(it) / tuple(it) / dict()` | コンテナ生成 |
| `type(x)` | 型名文字列 |
| `isinstance(x, cls)` | 継承チェーン込み判定 |
| `min / max / sum / sorted` | 集約 |
| `enumerate / zip` | リスト返し (eager) |
| `chr / ord / hex / bin` | 文字 / 数値変換 |
| `input([prompt])` | 1 行読込 |
| `hash(x)` | ハッシュ値 |

### 組み込み例外クラス
`Exception` を頂点として `TypeError / ValueError / NameError /
IndexError / KeyError / ZeroDivisionError / AttributeError /
RuntimeError / StopIteration` が登録済み。`raise X("msg")` は自動的に
`e.args = ("msg",)` と `e.message = "msg"` をセット。`isinstance` で
継承を辿って except マッチ。

### 文字列メソッド
`split / join / upper / lower / strip / startswith / endswith /
find / replace / count`。すべて `s.method(args)` 構文 (bound method)。

### リストメソッド
`append / pop / extend / insert / index / reverse / sort` (in-place)。

### dict メソッド
`get / keys / values / items / pop`。

### スライス
- `s[i]`、`s[i:j]`、`s[i:j:k]`、負インデックス、step、`s[::-1]` 反転 OK
- str / list / tuple に対応 (slice 結果は元と同型)

### 実行モード
| フラグ | 動作 |
|---|---|
| (なし)             | interpreter (code_store/all.so があれば dlopen 利用) |
| `-c`               | AOT-bake してから実行 |
| `--aot-compile`    | AOT-bake のみ |
| `--no-compile`     | code_store 不使用 (純 interpreter) |
| `-e <code>`        | コマンドライン文字列を実行 |

## アーキテクチャ的に既に入れてある仕組み

- AST ノードの structural hash → SD 単位の dedup
- `astro_cs_compile` / `_build` / `_load` / `_reload` の AOT パイプライン
- `try_stack` で nested try/except を `setjmp` / `longjmp` で正しく捕捉
- `c->state` ベースの `return / break / continue` 伝播 (longjmp 不要)
- closure 環境チェイン (`pyframe.parent`) — nested def ランタイム側は対応、parser 側は global lookup に fall back

## 性能 (vs. CPython 3.12)

`make bench` の結果。詳細・各最適化の解説は [`perf.md`](./perf.md)。

| bench | python3 | pystro AOT | pystro/python3 |
|---|---:|---:|---:|
| fib(35) | 1.21 s | 0.65 s | **1.86×** |
| while_loop 10M | 0.92 s | 0.05 s | **18×** |
| for_range 15M | 1.00 s | 0.08 s | **12.5×** |
| list 7M append+sum | 0.93 s | 0.19 s | **4.9×** |
| string 2M split | 0.59 s | 0.48 s | **1.23×** |
| dict 3M put+get | 0.78 s | 0.75 s | **1.04×** |

入った最適化 (`perf.md` §1-§8):

1. `gref_cache @ref` (fib 5×)
2. `globals_serial` を構造変化のみで bump (while 71×)
3. `node_for_global` 内蔵キャッシュ (for_range 23×)
4. `method_cache @ref` (list 12×)
5. `py_apply` を `node.h` に static inline (fib 1.15×)
6. leaf func の alloca フレーム (fib 1.5×)
7. dict identity-equal fast path (dict 1.1×)
8. string slice の buffer 共有 + 小サイズ alloc (string 1.6×)
