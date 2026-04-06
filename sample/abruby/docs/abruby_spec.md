# abruby 言語仕様

abruby (a bit Ruby) は Ruby のサブセット言語。CRuby の VALUE をそのまま利用する。

## リテラル

- 整数: `42`, `-3` (Fixnum 範囲のみ)
- 文字列: `"hello"`, `"value is #{expr}"` (文字列補間対応)
- 配列: `[1, 2, 3]`, `[]`
- ハッシュ: `{"a" => 1}`, `{a: 1}`, `{}`
- `true`, `false`, `nil`

Symbol リテラルは文字列として扱われる (`{a: 1}` は `{"a" => 1}` と同等)。

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

### self
メソッド内で現在のレシーバを参照:
```ruby
class Foo
  def me; self; end
end
```
トップレベルでは `nil`。

## 制御構造

### if / unless
```ruby
if cond
  expr1
else
  expr2
end

unless cond
  expr1
else
  expr2
end
```
`false` と `nil` が偽、それ以外は真 (CRuby の `RTEST` と同じ)。
else 節は省略可能（省略時は `nil`）。

### while
```ruby
while cond
  body
end
```
返り値は常に `nil`。

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
| `a / b` | `a./(b)` | 除算 |
| `a % b` | `a.%(b)` | 剰余 |
| `-a` | `a.-@()` | 単項マイナス |
| `a < b` | `a.<(b)` | 小なり |
| `a <= b` | `a.<=(b)` | 以下 |
| `a > b` | `a.>(b)` | 大なり |
| `a >= b` | `a.>=(b)` | 以上 |
| `a == b` | `a.==(b)` | 等値 |
| `a != b` | `a.!=(b)` | 非等値 |

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
- ユーザ定義クラスは暗黙的に Object を継承
- クラスの再オープン可能（既存クラスにメソッド追加）
- `ClassName.new(args)` でインスタンス生成 (`Class#new` → `initialize` 呼び出し)
- クラスオブジェクト自体も VALUE として扱える（定数参照で取得）

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

### Object (全クラスの基底)

| メソッド | 引数 | 説明 |
|---|---|---|
| `inspect` | 0 | `"#<ClassName:0xaddr>"` |
| `to_s` | 0 | `inspect` を呼ぶ |
| `==` | 1 | オブジェクト同一性 (VALUE の一致) |
| `!=` | 1 | `==` の否定 |
| `nil?` | 0 | `false` |
| `class` | 0 | クラス名を文字列で返す |
| `p` | 1 | `inspect` を出力し、引数を返す |

### Class

| メソッド | 引数 | 説明 |
|---|---|---|
| `new` | 可変 | インスタンス生成 + `initialize` 呼び出し |
| `inspect` | 0 | クラス名を返す |

### Integer

| メソッド | 引数 | 説明 |
|---|---|---|
| `inspect`, `to_s` | 0 | `"42"` |
| `+`, `-`, `*`, `/`, `%` | 1 | 算術演算 |
| `-@` | 0 | 単項マイナス |
| `<`, `<=`, `>`, `>=`, `==`, `!=` | 1 | 比較 |
| `zero?` | 0 | 0 かどうか |
| `abs` | 0 | 絶対値 |

### String

内部的には CRuby String を T_DATA で包んだ abruby_string として表現。

| メソッド | 引数 | 説明 |
|---|---|---|
| `inspect` | 0 | `"\"hello\""` (引用符付き) |
| `to_s` | 0 | 自身を返す |
| `to_i` | 0 | 整数に変換 |
| `+` | 1 | 連結 |
| `*` | 1 | 繰り返し (`"ab" * 3` → `"ababab"`) |
| `==`, `!=` | 1 | 内容比較 |
| `<`, `<=`, `>`, `>=` | 1 | 辞書順比較 |
| `length`, `size` | 0 | バイト長 |
| `empty?` | 0 | 空かどうか |
| `upcase` | 0 | 大文字化 (ASCII のみ) |
| `downcase` | 0 | 小文字化 (ASCII のみ) |
| `reverse` | 0 | 反転 |
| `include?` | 1 | 部分文字列の存在チェック |

### Array

内部的には CRuby Array を T_DATA で包んだ abruby_array として表現。

| メソッド | 引数 | 説明 |
|---|---|---|
| `inspect`, `to_s` | 0 | `"[1, 2, 3]"` |
| `[]` | 1 | インデックスアクセス (負のインデックス対応) |
| `[]=` | 2 | インデックス代入 |
| `push` | 1 | 末尾に追加、self を返す |
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

- 継承は未サポート (ユーザ定義クラスは暗黙的に Object を継承するのみ)
- モジュール未サポート
- ブロック・Proc・lambda 未サポート
- 例外処理 (begin/rescue/ensure) 未サポート
- Symbol 未サポート (文字列として扱われる)
- Bignum 未サポート (Fixnum 範囲のみ)
- Float 未サポート
- 正規表現 未サポート
- require / load 未サポート
- CRuby のメソッドは呼ばない (AbRuby 内で定義したメソッドのみ)
- CRuby の C レベル関数は利用可能 (IO 関連等)
