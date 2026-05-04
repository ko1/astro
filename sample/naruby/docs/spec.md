# naruby 言語仕様

実装済み機能は [done.md](done.md)、未対応項目は [todo.md](todo.md)、
ランタイム構造・実装詳細は [runtime.md](runtime.md) を参照。
**このドキュメントはユーザから見える言語の意味論のみを記述する**
(「naruby はどういう Ruby サブセットか」)。実装上どのノードに落ちるか、
parser の挙動、callcache の動作などは runtime.md 側にまとめる。

naruby は ASTro 論文 (VMIL 2025 / PPL 2026) で評価対象に使った Ruby
サブセット。**4 つの実行モード — Plain / AOT / Profile-Guided / JIT —
をすべて評価できる**唯一のサンプルを目指して、言語表面は意図的に
最小に絞ってある。

> 「Ruby で書いたつもりが naruby だった」という驚きを最小化するため、
> 文法は Ruby 互換 (Prism でパースして AST を読み替える) だが、評価できる
> 値・式は下記の表だけ。範囲外は parser が `unsupported node` で拒否する。

## 値

整数のみ。**符号付き 64bit、オーバーフローは未定義**。
Ruby の Integer の意味論ではなく C の int64 の意味論で、
範囲チェック・bignum 自動拡張・型タグはすべてなし。

未対応の値型:

- 浮動小数 / 文字列 / Symbol / Array / Hash / Range / Regexp
- `nil` / `true` / `false` キーワード
- Object / Class

真偽は整数で代用 — **0 は偽、それ以外は真**。

## リテラル

```ruby
0
42
-3
0xff
0b1010
```

64bit を超える整数リテラルは未対応 (truncate される)。

## 変数

ローカル変数のみ。

```ruby
x = 10
y = x + 1
```

スコープは `def` の本体ごと、+ トップレベル全体に 1 つ。
グローバル変数 (`$x`)、インスタンス変数 (`@x`)、クラス変数、定数は
すべて未対応。

## 式

### 算術

```ruby
a + b    a - b    a * b    a / b    a % b
```

**メソッドディスパッチを経由しない** — `Integer#+` 等の動的呼び出しは
組まない。両辺は整数、結果も整数。

### 比較

```ruby
a < b    a <= b    a > b    a >= b    a == b    a != b
```

**戻り値は Ruby の `true` / `false` ではなく整数 0 / 1**。
`if` の条件としてそのまま使える。

### 関数呼び出し

```ruby
fib(n - 1)
p(42)
```

`fname(args...)` のみ。**レシーバ付き呼び出し (`obj.method`) は未対応**。
仮引数は positional のみ — デフォルト値・キーワード引数・可変長・
ブロック引数 (`&blk`) はすべて未対応。

## 文 (statement)

### `if` / `elsif` / `else`

```ruby
if cond
  then_part
elsif cond2
  ...
else
  else_part
end
```

条件は整数で評価される (0 が偽、それ以外が真)。`else` 節は省略可能で、
省略時の値は 0。

### `while`

```ruby
while cond
  body
end
```

戻り値は 0。`break` / `next` / `redo` は未対応。
`until` は未テスト。

### `def`

```ruby
def fib(n)
  if n < 2
    1
  else
    fib(n - 2) + fib(n - 1)
  end
end
```

仮引数は positional のみ。

#### `return`

`return value` で関数から早期復帰できる (Ruby 通り):

```ruby
def f(x)
  if x < 0
    return 999
  end
  x * 2
end

p(f(5))    # => p:10
p(f(-3))   # => p:999
```

- `return` (引数なし) は 0 を返す。
- 複数値 `return a, b` は最初の値のみを返す。
- `break` / `next` / `redo` は未対応。

#### 再定義 (Ruby と同じセマンティクス)

メソッドは実行時にいつでも再定義できる:

```ruby
def f(x)
  x + 1
end

p(f(10))    # => p:11

def f(x)
  x * 2
end

p(f(10))    # => p:20  (定義し直し後の f が見える)
```

最後に評価された `def` が勝つ。`if` / `while` の中の動的 def もそのまま動く:

```ruby
def f(x) = x + 1
if cond
  def f(x) = x * 2    # cond が真のとき再定義される
end
f(10)                  # cond で挙動が変わる
```

(再定義時のキャッシュ無効化機構は [runtime.md §5.3](runtime.md) を参照。)

### トップレベル

ファイル全体を 1 つのスコープとして扱う。式 / `def` を直接書ける。

## 組み込み関数

3 つのみ。

| 名前 | 動作 |
|---|---|
| `p(v)` | `printf("p:%ld\n", v)` 相当を実行し、`v` をそのまま返す |
| `zero(v)` | 引数を無視して常に 0 を返す |
| `bf_add(a, b)` | `a + b` を返す (`+` 演算子と等価) |

## サンプル

```ruby
def fib(n)
  if n < 2
    1
  else
    fib(n - 2) + fib(n - 1)
  end
end

fib(40)    # => 165580141
```

`p` で出力もできる:

```ruby
def f(x) = x * x
p(f(7))    # 出力: p:49
```

one-liner def (`def f(x) = expr`) は通常の `def` と同じ意味。
