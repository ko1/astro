# abruby 言語仕様

abruby (a bit Ruby) は Ruby のサブセット言語。CRuby の VALUE をそのまま利用する。

## リテラル

- 整数: `42`, `-3`, `100000000000000000000` (Fixnum + Bignum)
- 浮動小数点: `1.5`, `3.14`, `1.0e100` (Flonum + heap Float)
- 文字列: `"hello"`, `"value is #{expr}"` (文字列補間対応)
- Symbol: `:foo`, `:empty?`, `:save!` (CRuby 即値)
- 配列: `[1, 2, 3]`, `[]`
- ハッシュ: `{"a" => 1}`, `{a: 1}` (Symbol キー対応), `{}`
- Range: `1..10` (inclusive), `1...10` (exclusive)
- Regexp: `/pattern/`, `/pattern/i`
- ヒアドキュメント: `<<~HEREDOC ... HEREDOC`
- `%w(a b c)` → `["a", "b", "c"]`
- `%i(a b c)` → `[:a, :b, :c]`
- Rational: `3r`, `Rational(1, 3)`
- Complex (虚数): `2i`, `Complex(1, 2)`
- `true`, `false`, `nil`
- 定数参照: `Float::INFINITY`, `Float::NAN`

## 変数

### ローカル変数
```ruby
a = 1
b = a + 2
```

### 複合代入
```ruby
a += 1   # a = a.+(1)
a -= 1
a *= 2
```

### インスタンス変数
クラスのメソッド内で使用:
```ruby
@x = 1
@x       #=> 1
@x += 1  #=> 2
```
未初期化のインスタンス変数は `nil` を返す。4 slots inline + heap overflow で制限なし。

### グローバル変数
```ruby
$x = 1
$x       #=> 1
$x += 1  #=> 2
```
VM インスタンスごとに独立。未初期化は `nil`。`ab_id_table` で動的管理（制限なし）。

### 多重代入
```ruby
a, b = 1, 2       # ローカル変数
@x, @y = 3, 4     # インスタンス変数
$a, $b = 5, 6     # グローバル変数
a, b = b, a        # swap
```
右辺が足りない場合は `nil`、余る場合は無視。

### 定数代入
```ruby
FOO = 42
```
トップレベルでは main_class に、クラス内ではそのクラスに格納。

### self
メソッド内で現在のレシーバを参照:
```ruby
class Foo
  def me; self; end
end
```
トップレベルでは main オブジェクト (Object のインスタンス)。

## 制御構造

### if / elsif / unless
```ruby
if cond
  expr1
elsif cond2
  expr2
else
  expr3
end

unless cond
  expr1
else
  expr2
end
```
`false` と `nil` が偽、それ以外は真 (CRuby の `RTEST` と同じ)。
else 節は省略可能（省略時は `nil`）。

### 後置 if / unless
```ruby
expr if cond
expr unless cond
```

### 三項演算子
```ruby
cond ? a : b
```

### while / until
```ruby
while cond
  body
end

until cond
  body
end
```
返り値は常に `nil`。

### return
```ruby
def f(x)
  return x + 1 if x > 0
  0
end
```
`return` はメソッドから即座に脱出する。`return expr` で値を返す。`return` のみは `nil` を返す。
制御フロー（if, while 等）の中からも脱出可能。

### break
```ruby
while true
  break if cond       # ループを脱出、while は nil を返す
end

result = while true
  break 42            # while が 42 を返す
end
```
`break` は最も内側の while/until から脱出する。値を指定しない場合は `nil`。

### ブロック / yield

```ruby
def each_pair
  yield 1, 2
  yield 3, 4
  self
end

sum = 0
each_pair { |a, b| sum += a + b }   # => 10
```

- `{ ... }` / `do ... end` の両方でブロックリテラルを書ける
- パラメータ: `|x|`, `|x, y|`, `|| ...|`（空 params）, なし
  - MVP は required パラメータのみ（デフォルト値/可変長/キーワード/`_1`/`_2` は未対応）
- `yield` / `yield args` でブロックを実行。ブロック無しで呼び出されたメソッドで `yield` すると `"no block given (yield)"` の例外が発生
- `block_given?` — メソッドが block 付きで呼ばれたかを判定
- `next` / `next value` — block body から脱出、yield の返り値になる
- `break` / `break value` — block から脱出、yielding メソッドの呼び出し元に値を返す
- **非ローカル `return`** — block 内 `return` は block を *定義した* メソッドから脱出する（Ruby 互換）。yield 連鎖が深くても正しく unwind する
- ブロックは closure: 外側メソッドのローカル変数を読み書きできる
- 同名のブロックパラメータは外側のローカル変数をシャドーする（outer の値は変更されない）
- `super`（暗黙）は現メソッドが受け取ったブロックを親メソッドに転送。`super() { ... }` で明示的に別ブロックを渡せる

#### ブロック関連の制約

- `->` lambda リテラル構文は未対応（`lambda { ... }` は可）
- `&:symbol` sugar は未対応
- block パラメータはデフォルト値/可変長/キーワード未対応（required パラメータのみ）
- `_1`, `_2` 番号付きパラメータは未対応
- `redo` は未対応

### Proc / lambda

```ruby
pr = Proc.new { |x| x + 1 }
pr.call(10)      #=> 11
pr[10]            #=> 11 (alias)

la = lambda { |x| x * 2 }
la.call(5)        #=> 10
la.lambda?        #=> true
la.arity          #=> 1

def f(&blk)
  blk.call(42)
end
f { |x| x + 1 }  #=> 43

g = proc { |x| x }
h(&g)             # Proc をブロックとして渡す
```

- `Proc.new { ... }` / `proc { ... }` — ブロックを Proc オブジェクトに変換
- `lambda { ... }` — lambda 生成（return で lambda 自身からのみ脱出）
- `Proc#call` / `Proc#[]` / `Proc#yield` — Proc の実行
- `Proc#arity` — パラメータ数
- `Proc#lambda?` — lambda かどうか
- `Proc#to_proc` — 自身を返す
- `def f(&blk)` — ブロックを Proc として受け取る
- `f(&my_proc)` — Proc をブロックとして渡す
- closure: 定義スコープのローカル変数を読み書き可能

#### Proc / lambda の制約

- `->` lambda リテラル構文は未対応
- `&:symbol` sugar は未対応
- block パラメータはデフォルト値/可変長/キーワード未対応

### Fiber

```ruby
fib = Fiber.new { |x|
  Fiber.yield(x + 1)
  Fiber.yield(x + 2)
  x + 3
}
fib.resume(10)    #=> 11
fib.resume        #=> 12
fib.resume        #=> 13
fib.alive?        #=> false
```

- `Fiber.new { ... }` — ファイバー生成
- `Fiber#resume(args)` — ファイバー開始/再開
- `Fiber.yield(value)` — ファイバー中断、resumer に値を返す
- `Fiber#alive?` — ファイバーが生存中かどうか
- 状態遷移: NEW → RUNNING → SUSPENDED → DONE
- CRuby Fiber API でスタック管理、GC 連携済み

#### Fiber の制約

- `Fiber#transfer` は未対応
- Enumerator は未対応

### 例外処理
```ruby
raise "error message"   # RuntimeError を発生（バックトレース付き）
raise                   # 空メッセージで RuntimeError を発生

begin
  # body
rescue => e
  # e に RuntimeError 例外オブジェクトが束縛される
  p(e.message)          # 例外メッセージ
  p(e.backtrace)        # バックトレース（Array of String）
ensure
  # 常に実行される（正常時・例外時・return 時）
end
```

- `raise` は Kernel のメソッドとして実装。RuntimeError 例外オブジェクトを生成する。
- 例外オブジェクトは `message`（元の引数）と `backtrace`（呼び出し履歴）を持つ。
- バックトレースは `"file:line:in 'method'"` 形式の文字列配列。
- `rescue` はクラス引数を取らない（全ての raise をキャッチ）。
- `rescue => e` で例外オブジェクトを変数に束縛可能。`=> e` 省略も可。
- `ensure` は常に実行される。ensure 内の `raise` や `return` は元の結果を上書きする。
- `begin/rescue/end` のみ（ensure なし）も可。`begin/ensure/end` のみ（rescue なし）も可。
- CRuby の `rb_raise`（longjmp）ではなく、RESULT の state 伝播で実装。
- バックトレースは `raise` 時点のフレーム linked list をスナップショットして構築。

### 論理演算子
```ruby
a && b    # a が偽なら a、そうでなければ b（短絡評価、値を返す）
a || b    # a が真なら a、そうでなければ b
a and b   # && と同じ（優先度低い）
a or b    # || と同じ（優先度低い）
!a        # a が真なら false、偽なら true
not a     # ! と同じ（優先度低い）
```

## メソッド定義

```ruby
def add(a, b)
  a + b
end

# endless method
def double(x) = x * 2
```

- トップレベルの `def` は Object クラスにメソッドを追加
- クラス内の `def` はそのクラスにメソッドを追加
- 同名のメソッドを再定義すると上書き
- 再帰呼び出し可能

## メソッド呼び出し

レシーバ付きは `node_method_call`、レシーバなし（暗黙 self）は `node_func_call` で処理。

```ruby
# レシーバ付き → node_method_call
obj.method(arg1, arg2)
1 + 2           # 1.+(2) と同じ
a[0]            # a.[](0) と同じ
a[0] = 1        # a.[]=(0, 1) と同じ

# レシーバなし → node_func_call (recv 操作なし、self を暗黙使用)
foo(1, 2)       # self.foo(1, 2) と同じ
p(42)           # self.p(42) と同じ

# 明示的 self → node_func_call (SelfNode 検出で最適化)
self.foo(1, 2)
```

## 演算子

全て対応するメソッドとしてディスパッチされる:

| 演算子 | メソッド名 | 説明 |
|---|---|---|
| `a + b` | `a.+(b)` | 加算 / 文字列連結 / 配列結合 |
| `a - b` | `a.-(b)` | 減算 |
| `a * b` | `a.*(b)` | 乗算 / 文字列繰り返し |
| `a / b` | `a./(b)` | 除算 (Ruby floor division) |
| `a % b` | `a.%(b)` | 剰余 (Ruby modulo) |
| `a ** b` | `a.**(b)` | 累乗 |
| `-a` | `a.-@()` | 単項マイナス |
| `a < b` | `a.<(b)` | 小なり |
| `a <= b` | `a.<=(b)` | 以下 |
| `a > b` | `a.>(b)` | 大なり |
| `a >= b` | `a.>=(b)` | 以上 |
| `a == b` | `a.==(b)` | 等値 |
| `a != b` | `a.!=(b)` | 非等値 |
| `a <=> b` | `a.<=>(b)` | 宇宙船演算子 |
| `a << b` | `a.<<(b)` | 左シフト / Array#push / String 追加 |
| `a >> b` | `a.>>(b)` | 右シフト |
| `a & b` | `a.&(b)` | ビット AND |
| `a \| b` | `a.\|(b)` | ビット OR |
| `a ^ b` | `a.^(b)` | ビット XOR |
| `~a` | `a.~()` | ビット NOT |
| `a[i]` | `a.[](i)` | インデックスアクセス |
| `a[i] = v` | `a.[]=(i, v)` | インデックス代入 |

ユーザ定義クラスでも演算子をメソッドとして定義可能:
```ruby
class Vec
  def +(other)
    Vec.new(@x + other.x, @y + other.y)
  end
end
```

## クラス

```ruby
class Point
  def initialize(x, y)
    @x = x
    @y = y
  end

  def x = @x
  def y = @y
  def inspect = "(#{@x}, #{@y})"
end

p Point.new(1, 2)  #=> (1, 2)
```

- `class Name; ...; end` でクラス定義
- `class Child < Parent; end` で継承
- ユーザ定義クラスは暗黙的に Object を継承
- クラスの再オープン可能（既存クラスにメソッド追加）
- `ClassName.new(args)` でインスタンス生成 (`Class#new` → `initialize` 呼��出し)
- クラスオブジェクト自体も VALUE として扱える（定数参照で取得）
- クラスは Object の定数として格納される

### モジュール

```ruby
module Greetable
  def greet
    p("hello!")
  end
end

class Person
  include Greetable
end
```

- `module Name; ...; end` でモジュール定義
- `include ModName` で mixin（super チェーンにプロキシ挿入）
- Module には `new` がない（Class だけ）
- Class は Module を継承

### method_missing

メソッドが見つからない場合、`method_missing` が定義されていれば呼ばれる:
```ruby
class Proxy
  def method_missing(name, x)
    p(name)
    p(x)
  end
end
Proxy.new.foo(42)  # "foo" と 42 を出力
```
第一引数にメソッド名（文字列）、以降に元の引数が渡される。

## ビルトインクラスとメソッド

### Kernel (module, Object に include)

| メソッド | 引数 | 説明 |
|---|---|---|
| `p` | 1 | `inspect` を出力し、引数を返す |
| `puts` | 0+ | 各引数を `to_s` して改行付き出力 |
| `print` | 0+ | 各引数を `to_s` して出力（改行なし） |
| `raise` | 0-1 | 例外を発生。引数が例外値になる |
| `exit` | 0-1 | プロセス終了 |
| `eval` | 1 | 文字列を評価（外部スコープ不可） |
| `block_given?` | 0 | ブロック付き呼び出しか判定 |
| `loop` | 0 + block | 無限ループ（break で脱出） |
| `require` | 1 | ファイル読み込み |
| `require_relative` | 1 | 相対パスでファイル読み込み |
| `Integer` | 1 | 整数変換 |
| `Float` | 1 | 浮動小数点変換 |
| `Rational` | 1-2 | Rational 生成 |
| `Complex` | 1-2 | Complex 生成 |
| `proc` | 0 + block | Proc 生成 |
| `lambda` | 0 + block | lambda 生成 |
| `__dir__` | 0 | 現在のファイルのディレクトリ |

### Object (全クラスの基底, Kernel を include)

| メソッド | 引数 | 説明 |
|---|---|---|
| `inspect` | 0 | `"#<ClassName:0xaddr>"` |
| `to_s` | 0 | `inspect` を呼ぶ |
| `==` | 1 | オブジェクト同一性 (VALUE の一致) |
| `!=` | 1 | `==` の否定 |
| `===` | 1 | `==` と同じ（case/when で使用） |
| `equal?` | 1 | オブジェクト同一性（VALUE 一致） |
| `!` | 0 | `false` を返す (偽オブジェクトでオーバーライド) |
| `nil?` | 0 | `false` |
| `class` | 0 | クラス名を文字列で返す |
| `is_a?` / `kind_of?` | 1 | クラス/モジュール判定 |
| `instance_of?` | 1 | 厳密クラス判定 |
| `respond_to?` | 1 | メソッドの存在チェック |
| `method` | 1 | Method オブジェクト取得 |
| `send` / `__send__` / `public_send` | 1+ | メソッド動的呼び出し |
| `instance_variable_get` | 1 | ivar 取得 |
| `instance_variable_set` | 2 | ivar 設定 |
| `freeze` | 0 | オブジェクト凍結 |
| `frozen?` | 0 | 凍結状態チェック |
| `dup` | 0 | 浅いコピー |
| `tap` | 0 + block | 自身を返す（ブロックに自身を渡す） |
| `object_id` | 0 | オブジェクト ID |
| `hash` | 0 | ハッシュ値 |

### Module

| メソッド | 引数 | 説明 |
|---|---|---|
| `===` | 1 | `is_a?` 判定 |
| `inspect` | 0 | モジュール名を返す |
| `include` | 1 | モジュールを mixin |
| `const_get` | 1 | 定数取得 |
| `const_set` | 2 | 定数設定 |
| `attr_reader` / `attr_writer` / `attr_accessor` | 可変 | アクセサ定義 |
| `private` / `public` / `protected` / `module_function` | 0 | no-op（定義のみ） |

### Class (Module を継承)

| メソッド | 引数 | 説明 |
|---|---|---|
| `new` | 可変 | インスタンス生成 + `initialize` 呼び出し |

### Integer

| メソッド | 引数 | 説明 |
|---|---|---|
| `inspect`, `to_s` | 0 | `"42"` |
| `+`, `-`, `*`, `/`, `%`, `**` | 1 | 算術演算 (Bignum 対応) |
| `-@` | 0 | 単項マイナス |
| `<`, `<=`, `>`, `>=`, `==`, `!=` | 1 | 比較 |
| `<=>` | 1 | 宇宙船演算子 |
| `<<`, `>>` | 1 | ビットシフト |
| `&`, `\|`, `^` | 1 | ビット演算 |
| `~` | 0 | ビット NOT |
| `to_f` | 0 | Float に変換 |
| `zero?` | 0 | 0 かどうか |
| `abs` | 0 | 絶対値 |
| `even?` | 0 | 偶数かどうか |
| `odd?` | 0 | 奇数かどうか |
| `times` | 0 + block | `\|i\|` で 0..self-1 をループ、self を返す |
| `step` | 2 + block | `\|i\|` でステップ付きループ |

Fixnum 範囲を超えると自動的に Bignum になる。Fixnum/Bignum/Float の混在演算に対応。
除算は Ruby floor division（C の切り捨てではなく負の無限大方向に丸める）。

### Float

| メソッド | 引数 | 説明 |
|---|---|---|
| `inspect`, `to_s` | 0 | `"3.14"` |
| `+`, `-`, `*`, `/`, `%`, `**` | 1 | 算術演算 |
| `-@` | 0 | 単項マイナス |
| `<`, `<=`, `>`, `>=`, `==`, `!=` | 1 | 比較 |
| `to_i` | 0 | 整数に変換 |
| `to_f` | 0 | 自身を返す |
| `abs` | 0 | 絶対値 |
| `zero?` | 0 | 0.0 かどうか |
| `floor`, `ceil`, `round` | 0 | 丸め |

定数: `Float::INFINITY`, `Float::NAN`

### String

内部的には CRuby String を T_DATA で包んだ abruby_string として表現。

| メソッド | 引数 | 説明 |
|---|---|---|
| `inspect` | 0 | `"\"hello\""` (引用符付き) |
| `to_s` | 0 | 自身を返す |
| `to_i` | 0 | 整数に変換 |
| `+` | 1 | 連結 |
| `<<` | 1 | 破壊的追加 (self を返す) |
| `*` | 1 | 繰り返し (`"ab" * 3` → `"ababab"`) |
| `==`, `!=` | 1 | 内容比較 |
| `<`, `<=`, `>`, `>=` | 1 | 辞書順比較 |
| `length`, `size` | 0 | バイト長 |
| `empty?` | 0 | 空かどうか |
| `upcase` | 0 | 大文字化 (ASCII のみ) |
| `downcase` | 0 | 小文字化 (ASCII のみ) |
| `reverse` | 0 | 反転 |
| `include?` | 1 | 部分文字列の存在チェック |
| `start_with?` | 1 | 先頭一致チェック |
| `end_with?` | 1 | 末尾一致チェック |
| `chomp` | 0 | 末尾改行除去 |
| `strip` | 0 | 前後空白除去 |
| `split` | 0-2 | 分割 |
| `tr` | 2 | 文字置換 |
| `=~` | 1 | 正規表現マッチ（位置を返す） |
| `bytes` | 0 | バイト配列 |
| `bytesize` | 0 | バイトサイズ |
| `%` | 1 | フォーマット |
| `sum` | 0-1 | チェックサム |
| `unpack` | 1 | バイナリ展開 |
| `to_sym` / `intern` | 0 | Symbol に変換 |

### Symbol

CRuby の即値 Symbol をそのまま利用。

| メソッド | 引数 | 説明 |
|---|---|---|
| `inspect` | 0 | `":foo"` |
| `to_s` | 0 | `"foo"` |
| `to_sym` | 0 | 自身を返す |
| `==`, `!=` | 1 | 同一性比較 |

Hash キーとして使用可能 (`{a: 1}` は `{:a => 1}` と同等)。

### Array

内部的には CRuby Array を T_DATA で包んだ abruby_array として表現。

| メソッド | 引数 | 説明 |
|---|---|---|
| `inspect`, `to_s` | 0 | `"[1, 2, 3]"` |
| `[]` | 1 | インデックスアクセス (負のインデックス対応) |
| `[]=` | 2 | インデックス代入 |
| `push` | 1 | 末尾に追加、self を返す |
| `<<` | 1 | `push` と同じ |
| `pop` | 0 | 末尾を削除して返す |
| `length`, `size` | 0 | 要素数 |
| `empty?` | 0 | 空かどうか |
| `first` | 0 | 先頭要素 (空なら nil) |
| `last` | 0 | 末尾要素 (空なら nil) |
| `+` | 1 | 配列結合 (新しい配列を返す) |
| `include?` | 1 | 要素の存在チェック |
| `==`, `!=` | 1 | 要素ごとの比較 |
| `each` | 0 + block | 各要素を yield、self を返す |
| `each_with_index` | 0 + block | `\|elem, idx\|` で yield |
| `map` / `collect` | 0 + block | 各要素の yield 結果を新配列に |
| `select` / `filter` | 0 + block | ブロックが真を返す要素を新配列に |
| `reject` | 0 + block | ブロックが偽を返す要素を新配列に |
| `flat_map` / `collect_concat` | 0 + block | map + flatten |
| `inject` / `reduce` | 0-1 + block | 累積演算 |
| `all?` / `any?` / `none?` | 0 + block | 全要素/任意要素/ゼロ要素判定 |
| `reverse` | 0 | 逆順配列 |
| `dup` | 0 | 浅いコピー |
| `flatten` | 0 | 平坦化 |
| `concat` | 1 | 破壊的結合 |
| `join` | 0-1 | 文字列結合 |
| `min` / `max` | 0 | 最小/最大要素 |
| `sort` | 0 | ソート |
| `fill` | 1 | 全要素を値で埋める |
| `clear` | 0 | 全要素削除 |
| `replace` | 1 | 内容を置換 |
| `compact` | 0 | nil を除去 |
| `uniq` / `uniq!` | 0 | 重複除去 |
| `transpose` | 0 | 転置 |
| `zip` | 1+ | 対応要素をペアに |
| `pack` | 1 | バイナリパック |
| `slice` / `slice!` | 1-2 | 部分配列取得/除去 |
| `rotate!` | 0-1 | 回転 |
| `shift` | 0 | 先頭要素を削除して返す |
| `unshift` | 1 | 先頭に追加 |

### Hash

内部的には CRuby Hash を T_DATA で包んだ abruby_hash として表現。
キーは内部的に CRuby 値に変換して格納される (文字列キーの等値比較が正しく動くため)。

| メソッド | 引数 | 説明 |
|---|---|---|
| `inspect`, `to_s` | 0 | `'{"a"=>1}'` |
| `[]` | 1 | キーアクセス (見つからなければ nil) |
| `[]=` | 2 | キー代入 |
| `length`, `size` | 0 | 要素数 |
| `empty?` | 0 | 空かどうか |
| `has_key?`, `key?` | 1 | キーの存在チェック |
| `keys` | 0 | キーの配列を返す |
| `values` | 0 | 値の配列を返す |
| `each` / `each_pair` | 0 + block | `\|k, v\|` で各ペアを yield、self を返す |
| `each_key` | 0 + block | 各キーを yield |
| `each_value` | 0 + block | 各値を yield |
| `merge` | 1 | 結合（新しいハッシュを返す） |
| `delete` | 1 | キー削除 |
| `fetch` | 1-2 | キーアクセス（見つからなければ例外 or デフォルト） |
| `dup` | 0 | 浅いコピー |
| `compare_by_identity` | 0 | identity 比較モード |

### Range

内部的には abruby_range (begin, end, exclude_end) として表現。

| メソッド | 引数 | 説明 |
|---|---|---|
| `inspect`, `to_s` | 0 | `"1..10"` / `"1...10"` |
| `first`, `begin` | 0 | 始端 |
| `last`, `end` | 0 | 終端 |
| `exclude_end?` | 0 | `...` かどうか |
| `size`, `length` | 0 | 要素数 (Integer のみ) |
| `include?` | 1 | 含まれるか (Integer のみ) |
| `to_a` | 0 | 配列に変換 (Integer のみ) |
| `==` | 1 | 等値比較 |
| `each` | 0 + block | `\|i\|` で範囲を走査 (Integer のみ)、self を返す |
| `map` / `collect` | 0 + block | 各要素の yield 結果を新配列に |
| `all?` / `any?` | 0 + block | 全要素/任意要素判定 |
| `inject` / `reduce` | 0-1 + block | 累積演算 |

### Regexp

内部的には CRuby Regexp を T_DATA で包んだ abruby_regexp として表現。

| メソッド | 引数 | 説明 |
|---|---|---|
| `inspect` | 0 | `"/pattern/"` |
| `to_s` | 0 | `"(?-mix:pattern)"` |
| `source` | 0 | パターン文字列 |
| `match?` | 1 | マッチするか (true/false) |
| `===` | 1 | `match?` と同じ（case/when で使用） |
| `match` | 1 | マッチするか (true/nil) |
| `=~` | 1 | マッチ位置 (Integer/nil) |
| `==` | 1 | 等値比較 |

### Rational

内部的には CRuby Rational を T_DATA で包んだ abruby_rational として表現。
`Rational(num, den)` または `3r` リテラルで生成。

| メソッド | 引数 | 説明 |
|---|---|---|
| `inspect` | 0 | `"(1/3)"` |
| `to_s` | 0 | `"1/3"` |
| `+`, `-`, `*`, `/`, `**` | 1 | 算術演算 |
| `-@` | 0 | 単項マイナス |
| `<`, `<=`, `>`, `>=`, `==` | 1 | 比較 |
| `<=>` | 1 | 宇宙船演算子 |
| `numerator` | 0 | 分子 |
| `denominator` | 0 | 分母 |
| `to_f` | 0 | Float に変換 |
| `to_i` | 0 | 整数に変換 |
| `to_r` | 0 | 自身を返す |

### Complex

内部的には CRuby Complex を T_DATA で包んだ abruby_complex として表現。
`Complex(real, imag)` または `2i` リテラルで生成。

| メソッド | 引数 | 説明 |
|---|---|---|
| `inspect` | 0 | `"(1+2i)"` |
| `to_s` | 0 | `"1+2i"` |
| `+`, `-`, `*`, `/`, `**` | 1 | 算術演算 |
| `-@` | 0 | 単項マイナス |
| `==` | 1 | 等値比較 |
| `real` | 0 | 実部 |
| `imaginary` | 0 | 虚部 |
| `abs` | 0 | 絶対値 |
| `conjugate` | 0 | 共役複素数 |
| `rectangular` | 0 | `[real, imag]` 配列 |
| `to_f` | 0 | Float に変換 (虚部が 0 の場合) |
| `to_c` | 0 | 自身を返す |

### TrueClass / FalseClass

| メソッド | 引数 | 説明 |
|---|---|---|
| `inspect`, `to_s` | 0 | `"true"` / `"false"` |

`==`, `!=`, `nil?`, `class` は Object から継承。

### NilClass

| メソッド | 引数 | 説明 |
|---|---|---|
| `inspect` | 0 | `"nil"` |
| `to_s` | 0 | `""` (空文字列) |
| `nil?` | 0 | `true` |
| `[]` | 1 | 常に `nil` を返す |

## 制約

Ruby との相違点の詳細は [todo.md](todo.md) を参照。

主な未サポート機能:
- `->` lambda リテラル構文 / `&:sym` sugar
- for .. in
- デフォルト引数・キーワード引数・可変長引数受け取り (`*args`, `**kwargs`)
- クラス変数 (`@@var`)
- block 内 numbered parameters (`_1`, `_2`) / `redo`
- `alias` / `alias_method`
- ネストしたクラス/モジュール (`class A::B`)
- シングルトンメソッド / 特異クラス
- `prepend` / `extend`
- `Fiber#transfer`
- Enumerator / lazy evaluation
