# koruby 言語仕様

`koruby` は **Ruby のサブセット** インタプリタ。スタンドアロン (CRuby に
依存しない) で動く本格的な Ruby 処理系を目指して実装している — naruby
(整数のみ) や abruby (CRuby 拡張) と違い、独自に値表現・GC・ランタイムを
持ち、optcarrot (NES エミュレータ) を完走する程度のカバレッジを持つ。

CRuby と同等の意味論を狙うが、未実装機能 (Regexp / Fiber / Thread 等) も
ある。完全な Ruby 仕様は [Ruby Language Reference](https://ruby-lang.org/)
を参照。本書は koruby で動く範囲を端的に示す。

## 値の種類

| 型 | 例 | 備考 |
|---|---|---|
| `Integer` (Fixnum/Bignum) | `42` `2**100` | GMP で任意精度に自動昇格 |
| `Float` | `3.14` `1e10` | IEEE-754 double。FLONUM 即値最適化 |
| `String` | `"hello"` `'world'` | mutable |
| `Symbol` | `:foo` | 即値、grep 比較が高速 |
| `true` `false` | (キーワード) | |
| `nil` | (キーワード) | |
| `Array` | `[1, 2, 3]` | mutable |
| `Hash` | `{a: 1, b: 2}` | 挿入順保持 |
| `Range` | `1..10` (両端) `1...10` (右開き) | |
| `Proc` / `Lambda` | `->(x) { x }` `proc { ... }` | |
| `Class` / `Module` | `class Foo; end` | 第一級 |
| `Struct` | `Struct.new(:a, :b)` | |
| `Exception` | `RuntimeError.new("oops")` | |
| `Binding` | `binding` | フレームの lvar / self / cref を捕捉 |
| `Method` / `UnboundMethod` | `obj.method(:foo)` | bind / unbind / curry |
| `Fiber` | `Fiber.new { ... }` | resume / yield / transfer |

**真偽判定**: `false` と `nil` だけが偽。`0` も `""` も `[]` も真 (Ruby 標準)。

## リテラル

```ruby
42  -3  0xff  0b1010  1_000_000      # Integer
3.14  1e10  6.022e23                 # Float
"hello\n"  'no escape'               # String (シングル引用は補間なし)
"#{x + y}"                            # 文字列補間
:name  :"foo bar"                     # Symbol
true  false  nil
[1, 2, 3]                             # Array
{a: 1, b: 2}                          # Hash (Symbol キーの省略形)
{"key" => "val"}                      # Hash (任意キー)
1..10    1...10                       # Range
%w[a b c]                             # = ["a", "b", "c"]
%i[x y z]                             # = [:x, :y, :z]
```

## 変数

```ruby
x = 1                  # ローカル変数 (lower-case)
@x = 1                 # インスタンス変数 (オブジェクトに属する)
@@x = 1                # クラス変数 (クラスに属する) -- 限定対応
$x = 1                 # グローバル変数
X = 1                  # 定数 (大文字始まり)

a, b, c = 1, 2, 3      # 多重代入
a, b = b, a            # スワップ
```

スコープ:
- ローカル変数: `def`/`do...end`/`{}` ブロック単位
- ブロック (`{}` / `do...end`) は外側のローカル変数を捕獲する (closure)

## 演算子

| カテゴリ | 演算子 |
|---|---|
| 算術 | `+ - * / % **` |
| 比較 | `< > <= >= == != <=>` (`<=>` は -1/0/1) |
| 論理 | `&& \|\| ! and or not` (記号と語の優先度が違う点に注意) |
| ビット | `& \| ^ ~ << >>` |
| 代入 | `= += -= *= /= **= &&= \|\|=` |
| 三項 | `? :` |
| 範囲 | `..` `...` |
| 添字 | `a[i]` `a[i] = v` |
| メソッド | `obj.method(args)` `obj&.method` (safe nav) |

`a == b` は `a.==(b)` のメソッド呼出。算術もすべてメソッド (`a + b` =
`a.+(b)`)。

## 制御構文

```ruby
if cond then ... elsif other then ... else ... end
if cond then ... end
unless cond then ... end                 # if not の糖衣
expr if cond                              # 後置 if

while cond do ... end
until cond do ... end
loop { ... }                              # 無限ループ (break で抜ける)

case x
when 1, 2     then "small"
when Integer  then "int"
when /pat/    then "matches"
when 1..10    then "in range"
else               "other"
end

n.times { |i| ... }                       # メソッド呼出 + ブロック
arr.each { |e| ... }
1.upto(10) { |i| ... }

break value      # ループ / ブロックから抜ける
next value       # 次のイテレーションへ
redo             # 同じイテレーションをやり直し
return value     # メソッドから抜ける
```

`if` `case` `begin` も式 — 値を返す。

## メソッド定義

```ruby
def add(a, b)
  a + b
end

def greet(name = "world")            # デフォルト引数
  "hi #{name}"
end

def variadic(*args)                   # 可変長
  args.size
end

def method_with_block(&blk)           # ブロック引数
  blk.call(42)
end

# one-liner def (Ruby 3+)
def square(x) = x * x
```

メソッド本体の最終式が戻り値。`return` 早期復帰可。

## クラス・モジュール

```ruby
class Animal
  def initialize(name)
    @name = name
  end

  attr_reader :name             # ゲッター自動生成
  attr_accessor :age            # ゲッター + セッター

  def greet
    "hi #{@name}"
  end

  def self.create(name)         # クラスメソッド
    new(name)
  end
end

class Dog < Animal              # 継承
  def greet
    super + ", woof!"            # 親メソッド呼出
  end
end

module Greeter                  # ミックスイン用モジュール
  def hello
    "hello!"
  end
end

class Cat < Animal
  include Greeter               # メソッド取り込み
end

c = Cat.new("tama")
c.hello                          # "hello!"  (Greeter から)
c.greet                          # "hi tama" (Animal から)
```

`include` でモジュール由来のメソッドを取り込み、`<` で単一継承。
`super` (引数なし) は呼出元の引数をそのまま親に転送。

## ブロック・Proc・Lambda

ブロックはメソッドに渡す「無名のコードブロック」:

```ruby
[1, 2, 3].each { |x| puts x }
[1, 2, 3].each do |x|
  puts x
end

# yield でブロックを呼ぶ
def repeat(n)
  n.times { yield }
end

repeat(3) { puts "hi" }
```

第一級値としての関数オブジェクト:

```ruby
sqr = ->(x) { x * x }            # Lambda (引数チェック厳格)
sqr.call(5)                       # 25
sqr.(5)                           # 25
sqr[5]                            # 25

p = proc { |x| x + 1 }            # Proc (引数チェック緩い)

# Symbol#to_proc
[1, 2, 3].map(&:to_s)             # ["1", "2", "3"] (=  map { |x| x.to_s })
```

ブロック / Proc は外側の変数を捕獲する (closure)。

## 例外処理

```ruby
begin
  risky_op
rescue ValueError => e
  puts "value error: #{e.message}"
rescue StandardError, RuntimeError => e
  puts "other: #{e}"
rescue
  puts "anything"
else
  puts "no exception"           # 例外が出なかったときだけ
ensure
  cleanup                         # 必ず
end

raise "oops"                      # RuntimeError
raise ArgumentError, "bad input"
raise CustomError.new(...)
```

## ファイル分割・require

```ruby
require_relative "mylib"          # 同じディレクトリの mylib.rb
require "json"                     # 標準ライブラリ
load "config.rb"                   # 都度実行 (キャッシュなし)
```

## 組み込み (主なもの)

### Integer

`+ - * / % ** abs zero? positive? negative? even? odd?
to_s to_f times upto downto step succ pred chr digits
divmod gcd lcm`

### Float

`floor ceil round truncate abs nan? infinite? finite? to_i to_s`

### String

`length size + * == upcase downcase capitalize reverse strip
chars split join sub gsub include? start_with? end_with?
index rindex slice [i] [i, j] [i..j] each_char each_line
to_i to_f to_sym inspect format`

### Symbol

`to_s to_proc == size length`

### Array

`length size << push pop shift unshift first last include?
each map collect select filter reject reduce inject sort sort_by
reverse uniq compact flatten min max sum count zip take drop
join + - * == [i] [i, j] [i..j] []= each_with_index each_with_object`

### Hash

`size [key] [key]= store delete keys values each each_pair
include? key? has_key? value? empty? merge merge!
to_a map select reject any? all? sort_by`

### Range

`first last min max include? cover? size step each map to_a`

### Kernel

`puts print p pp gets sprintf format
require require_relative load
raise rescue
sleep srand rand
abort exit
caller __method__
binding eval (with optional Binding arg)
lambda proc block_given? loop catch throw
Integer Float String Array Hash`

### Binding

```ruby
def m
  a = 1
  b = binding
  b.local_variable_set(:c, 99)
  b.local_variables   # => [:c, :a, :b]   (set-introduced first)
  b.eval("a + c")     # => 100
  b.source_location   # => [<file>, <line>]
end
```

`local_variable_get` / `local_variable_set` / `local_variable_defined?` /
`local_variables` / `receiver` / `eval(src [, file [, line]])` /
`source_location` / `dup` / `clone` をサポート。
`Proc#binding` も実装済み (proc 捕捉 env から Binding 構築)。

### Class / Module

`new ancestors instance_methods include extend
attr_reader attr_writer attr_accessor`

### File / IO (限定対応)

`File.read File.write File.exist? File.open File.join
$stdout.puts $stderr.puts STDOUT STDERR`

## 例

```ruby
# クラス + 継承 + super
class Animal
  attr_reader :name
  def initialize(name) = @name = name
  def greet = "hi #{@name}"
end

class Dog < Animal
  def initialize(name, breed)
    super(name)
    @breed = breed
  end
  def greet = "#{super}, woof! (#{@breed})"
end

puts Dog.new("rex", "lab").greet

# ブロック + each + map
def primes_under(n)
  (2...n).select do |i|
    (2...i).none? { |d| i % d == 0 }
  end
end

puts primes_under(30).inspect

# Struct + 例外
Point = Struct.new(:x, :y) do
  def +(other)
    Point.new(x + other.x, y + other.y)
  end
end

p Point.new(1, 2) + Point.new(10, 20)
```

## 持たない / 制限

- **真の Regexp** — Regexp リテラルは文字列スタブとして扱い、`=~` / `match` / `scan` は no-op (将来的に sample/astrogre と統合予定)
- **Encoding-aware String** — byte sequence のみ。`String#encoding` `force_encoding` `b` は stub、 multi-byte succ や m17n 系は未対応
- **Thread / Mutex / Queue** — single-threaded 前提
- **Refinements** (`refine` / `using`)
- **Ractor**
- **TracePoint / RubyVM**
- **Process / spawn / fork** — `ruby_exe` 子プロセス起動を必要とする spec は skip
- 完全な IO / Float の特殊値 (Infinity / NaN) 細部

詳細: [`done.md`](done.md) / [`todo.md`](todo.md) / [`runtime.md`](runtime.md)。
