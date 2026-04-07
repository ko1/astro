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
未初期化のインスタンス変数は `nil` を返す。最大 32 個。

### グローバル変数
```ruby
$x = 1
$x       #=> 1
$x += 1  #=> 2
```
VM インスタンスごとに独立。未初期化は `nil`。最大 64 個。

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

### 例外処理
```ruby
raise "error message"   # RuntimeError を発生
raise                   # 空メッセージで RuntimeError を発生

begin
  # body
rescue => e
  # e に例外メッセージ（文字列）が束縛される
  p(e)
ensure
  # 常に実行される（正常時・例外時・return 時）
end
```

- `raise` は Kernel のメソッドとして実装。引数の VALUE がそのまま例外値になる。
- `rescue` はクラス引数を取らない（全ての raise をキャッチ）。
- `rescue => e` で例外メッセージを変数に束縛可能。`=> e` 省略も可。
- `ensure` は常に実行される。ensure 内の `raise` や `return` は元の結果を上書きする。
- `begin/rescue/end` のみ（ensure なし）も可。`begin/ensure/end` のみ（rescue なし）も可。
- CRuby の `rb_raise`（longjmp）ではなく、RESULT の state 伝播で実装。

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

全てのメソッド呼び出しは OOP メソッドディスパッチ経由 (`node_method_call` に統一)。

```ruby
# レシーバ付き
obj.method(arg1, arg2)
1 + 2           # 1.+(2) と同じ
a[0]            # a.[](0) と同じ
a[0] = 1        # a.[]=(0, 1) と同じ

# レシーバなし (self に対する呼び出し)
foo(1, 2)       # self.foo(1, 2) と同じ
p(42)           # self.p(42) と同じ
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
| `raise` | 0-1 | 例外を発生。引数が例外値になる |

### Object (全クラスの基底, Kernel を include)

| メソッド | 引数 | 説明 |
|---|---|---|
| `inspect` | 0 | `"#<ClassName:0xaddr>"` |
| `to_s` | 0 | `inspect` を呼ぶ |
| `==` | 1 | オブジェクト同一性 (VALUE の一致) |
| `!=` | 1 | `==` の否定 |
| `!` | 0 | `false` を返す (偽オブジェクトでオーバーライド) |
| `nil?` | 0 | `false` |
| `class` | 0 | クラス名を文字列で返す |

### Class / Module

| メソッド | 引数 | 説明 |
|---|---|---|
| `new` | 可変 | インスタンス生成 + `initialize` 呼び出し (Class のみ) |
| `inspect` | 0 | クラス名を返す |
| `include` | 1 | モジュールを mixin (Module) |

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

### Regexp

内部的には CRuby Regexp を T_DATA で包んだ abruby_regexp として表現。

| メソッド | 引数 | 説明 |
|---|---|---|
| `inspect` | 0 | `"/pattern/"` |
| `to_s` | 0 | `"(?-mix:pattern)"` |
| `source` | 0 | パターン文字列 |
| `match?` | 1 | マッチするか (true/false) |
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

## 制約

Ruby との相違点の詳細は [todo.md](todo.md) を参照。

主な未サポート機能:
- ブロック・Proc・lambda・イテレータ
- next
- super 呼び出し
- case / when / in
- for .. in
- require / load
- アクセス制御 (public/private/protected)
- デフォルト引数・キーワード引数・可変長引数
- クラス変数 (`@@var`)
