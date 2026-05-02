# naruby 言語仕様

実装状態 (どのノードがどう動くか) は [done.md](done.md)、未対応項目は
[todo.md](todo.md)、ランタイム構造は [runtime.md](runtime.md) を参照。

naruby は ASTro 論文 (VMIL 2025 / PPL 2026) で評価対象に使った Ruby
サブセット。**4 つの実行モード — Plain / AOT / Profile-Guided
/ JIT — をすべて評価できる**唯一のサンプルを目指して、言語表面は意図的に
最小に絞ってある。

> 「Ruby で書いたつもりが naruby だった」という驚きを最小化するため、
> 文法は Ruby 互換 (Prism でパースして AST を読み替える) だが、評価できる
> 値・式は下記の表だけ。範囲外は parser が `unsupported node` で拒否する。

## 値

| 型 | 表現 | 備考 |
|---|---|---|
| 整数 | `int64_t` (`VALUE = int64_t`) | 唯一の値型。範囲チェックなし、オーバーフローは C と同じく未定義扱い |
| 真偽 | 整数で代用 (0 = false、それ以外 = true) | `true` / `false` キーワード自体は未対応 |

`nil`、`true`/`false` キーワード、浮動小数、文字列、Symbol、Array、
Hash、Range、Regexp、Object、Class — すべて未対応 (parser が unsupported)。

## リテラル

```ruby
0
42
-3            # 単項マイナスは 0 - 3 にせず PM_INTEGER_NODE で直接扱う場合と
              # CallNode を経由する場合があり、サポート範囲は parser 次第
0xff
0b1010
```

整数リテラルは Prism の `pm_integer_t` の絶対値部分を `int` 化して
`ALLOC_node_num(val)` に渡す。bignum 範囲は超えると C 側で truncate される。

## 変数

ローカル変数のみ。

```ruby
x = 10
y = x + 1
```

スコープは `def` の本体ごと、+ トップレベル全体に 1 つ。グローバル変数
(`$x`)、インスタンス変数 (`@x`)、クラス変数、定数はすべて未対応。

ローカル変数のスロット番号は parser が決め打つ。`x = ...` で初出のスロット
を割り当て、以降の参照は同じスロット。代入は `node_lset(slot, rhs)`、
参照は `node_lget(slot)` に下りる。

## 式

### 算術

```ruby
a + b    a - b    a * b    a / b    a % b
```

二項演算は parser が直接 `node_add` / `node_sub` / `node_mul` /
`node_div` / `node_mod` に下ろす (`alloc_binop`)。Ruby 流のメソッド
ディスパッチは経由しない — Integer#+ / Integer#- も組まない。

### 比較

```ruby
a < b    a <= b    a > b    a >= b    a == b    a != b
```

戻り値は Ruby の `true`/`false` ではなく整数 0/1。`if` の条件として
そのまま使える (0 が偽)。

### 関数呼び出し

```ruby
fib(n - 1)
p(42)
```

`fname(args...)` のみ。レシーバ付き呼び出し (`obj.method`) は parser が
unsupported を返す (PM_CALL_NODE で receiver を処理しない経路のみ実装)。

引数は左から順にローカルスロット (`arg_index`) に書き込まれ、
`node_call` がフレームを切り替えて呼び先の `node_scope` へ入る。

#### 呼び出し方の variant (実行モード依存)

parser の `alloc_call` (naruby_parse.c:69) はオプションで形を変える:

| モード | 確保するノード | 解説 |
|---|---|---|
| 既定 | `node_call(name, argc, idx, callcache *)` | 名前 → `function_entry` をキャッシュで引いて呼ぶ |
| `-s` (static_lang) | `node_call_static(NODE *body, idx)` | parse 時に `code_repo_find_by_name` で body を解決済 |
| `-p` (pg_mode) | `node_call2(name, argc, idx, callcache *, NODE *sp_body)` | sp_body を初期 `node_num(0)` で置き、初回呼び出しで slowpath が patch |

`callcache` は `{ state_serial_t serial; NODE *body; }`。`c->serial` が
進む (= `def` の再定義) たびにキャッシュは無効化される。

## 文 (statement)

### `if` / `else`

```ruby
if cond
  then_part
else
  else_part
end
```

条件は整数で評価される (`!= 0`)。`else` 節がない場合は parser が
`ALLOC_node_num(0)` で埋める。`elsif` は Ruby の AST 上 `if` のネストに
なるのでそのまま動く。

### `while`

```ruby
while cond
  body
end
```

`break` / `next` / `redo` は未対応 (parser が unsupported)。
`until` は Prism 上は `WhileNode` の variant だが、未テスト。

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

仮引数は positional のみ。デフォルト値、キーワード引数、可変長引数、
ブロック引数 (`&blk`) はすべて未対応。`def` ノードは実行時に
`function_entry` をスコープに登録する形で動く (cfunc 風)。

`return` は実装済み (2026-05-02 で対応):

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

`return value` は `RESULT { value, RESULT_RETURN }` を発行する。
caller (`call_body` / `node_call2` / `node_call_static`) が関数境界で
`RESULT_RETURN` を `RESULT_NORMAL` に変換するので、再帰関数の
ネストで複数 return が積み重なっても問題ない。詳細は
[runtime.md §RESULT 型と非局所脱出](runtime.md) を参照。

`return` (引数なし) と複数値 `return a, b` は単に `0` と最初の値を
返す。`break` / `next` / `redo` は未対応 (parser が unsupported)。

#### 再定義 (Ruby と同じ)

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

最後に評価された `def` が勝つ。`def f` は `node_def` ノードとして
AST に残るので、`if` / `while` の中の動的 def もそのまま動く:

```ruby
def f(x) = x + 1
if cond
  def f(x) = x * 2    # cond が真のとき再定義される
end
f(10)                  # cond で挙動が変わる
```

#### callcache と serial の役割

呼び出し側の高速化として、`node_call` / `node_call2` の各ノードは
**インラインキャッシュ** (`struct callcache { state_serial_t serial;
NODE *body; }`) を `@ref` フィールドで埋め込む。実行時の流れ:

1. 呼び出し時、`cc->serial == c->serial` なら cache hit、`cc->body`
   を直接 EVAL する。
2. ミスなら slowpath が `function_entry` を線形検索して body を
   解決、`cc->body = body` / `cc->serial = c->serial` で更新。
3. `node_def` が走るたびに `c->serial++` する — これで全 callcache
   が一括して無効化され、次回の呼び出しは slowpath を経由して
   新しい body に切り替わる。

つまり「serial bump → cache miss → 再解決」というインラインキャッシュ
の典型形。Ruby の `RubyVM` の global method state やメソッドの世代
カウンタと同じ発想。

> **AOT specialize との関係**: `code_store/all.so` にベイクされた
> SD は `cc` を `@ref` 経由で読みにいくだけで、再定義されても dlopen
> し直す必要はない。serial mismatch を見て slowpath に落ち、新しい
> body をキャッシュすれば、その後の呼び出しは再び direct EVAL 経路
> に戻る (PG モードの場合は SD が `cc->body` 経由で indirect dispatch)。
> 詳細は [runtime.md](runtime.md) §5.3 を参照。

### トップレベル

ファイル全体は `node_seq` の鎖。トップレベルにも `def` / 式を直接
書ける。

## 組み込み関数

`main.c:42` の `define_builtin_functions` で 3 つだけ登録される:

| 名前 | C 実装 | 意味 |
|---|---|---|
| `p` | `narb_p(VALUE)` | `printf("p:%ld\n", v)` して `v` をそのまま返す |
| `zero` | `narb_zero(VALUE)` | 引数を無視して常に 0 を返す |
| `bf_add` | `narb_add(VALUE, VALUE)` | `a + b` を返す (Ruby 演算子と等価) |

組み込み関数の本体は静的にリンクされ、`node_call_builtin` が直接 C 関数
ポインタを呼ぶ。`code_repo` にも登録されるので `astro_cs_compile` が
specialize 対象に含める。

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

これが naruby で書ける式の上限近辺。`p` で出力もできる:

```ruby
def f(x) = x * x
p(f(7))    # 出力: p:49
```

> Note: one-liner def (`def f(x) = expr`) は Prism が `DefNode` で
> `body = StatementsNode([expr])` として返すので、def ノードの通常
> 経路で動く。
