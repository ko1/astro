# abruby TODO: Ruby との相違点

abruby が現在サポートしていない Ruby 機能の一覧。

## 言語機能

### 制御構造
- [ ] `elsif`
- [ ] `case / when / in`
- [ ] `for .. in`
- [ ] `until`
- [ ] `break` / `next` / `return`（明示的 return）
- [ ] `begin / rescue / ensure / raise`（例外処理）
- [ ] `&&` / `||` / `and` / `or` / `not`（論理演算子）
- [ ] 後置 if/unless/while (`expr if cond`)
- [ ] 三項演算子 (`cond ? a : b`)
- [ ] `defined?`

### ブロック・Proc・lambda
- [ ] ブロック渡し (`method { |x| ... }` / `method do |x| ... end`)
- [ ] `yield`
- [ ] `Proc.new` / `proc` / `lambda` / `->`
- [ ] `&block` 引数
- [ ] イテレータ (`each`, `map`, `select`, `reduce` 等)

### メソッド
- [ ] `super`
- [ ] デフォルト引数 (`def f(a, b = 1)`)
- [ ] キーワード引数 (`def f(a:, b: 1)`)
- [ ] 可変長引数 (`def f(*args)`)
- [ ] `**kwargs`
- [ ] `attr_reader` / `attr_writer` / `attr_accessor`
- [ ] `public` / `private` / `protected`（アクセス制御）
- [ ] クラスメソッド (`def self.foo`)
- [ ] `alias` / `alias_method`
- [ ] `respond_to?`

### クラス・モジュール
- [ ] `super` 呼び出し
- [ ] `prepend`
- [ ] `extend`
- [ ] ネストしたクラス/モジュール (`class A::B`)
- [ ] `Comparable` / `Enumerable` 等の標準 mixin
- [ ] シングルトンメソッド / 特異クラス
- [ ] `class_eval` / `module_eval`
- [ ] `define_method`
- [ ] `is_a?` / `kind_of?` / `instance_of?`
- [ ] `ancestors`

### 変数・定数
- [ ] グローバル変数 (`$var`)
- [ ] クラス変数 (`@@var`)
- [ ] 定数代入 (`FOO = 42`、トップレベルおよびクラス内）
- [ ] `::`（ネストした定数パス、`A::B::C`）
- [ ] 多重代入 (`a, b = 1, 2`)

### リテラル・型
- [ ] Symbol (`:foo`)
- [ ] Range (`1..10`, `1...10`)
- [ ] Regexp (`/pattern/`)
- [ ] Rational (`1/3r`)
- [ ] Complex (`1+2i`)
- [ ] ヒアドキュメント
- [ ] `%w()`, `%i()` 等のリテラル

### 演算子
- [ ] `<=>` (宇宙船演算子)
- [ ] `<<` (追加演算子 / ビットシフト)
- [ ] `>>` (ビットシフト)
- [ ] `&`, `|`, `^` (ビット演算)
- [ ] `~` (ビット反転)
- [ ] `[]` / `[]=` のユーザ定義（abruby 側で演算子として定義可能だが、パーサ未対応の場面がある可能性）

## ビルトインクラス・メソッドの差分

### Integer
- [ ] `times`, `upto`, `downto`（ブロック必要）
- [ ] `to_r`, `to_c`
- [ ] `even?`, `odd?`
- [ ] `gcd`, `lcm`
- [ ] `digits`
- [ ] `<<`, `>>`, `&`, `|`, `^`, `~`

### Float
- [ ] `nan?`, `infinite?`, `finite?`
- [ ] `truncate`
- [ ] `to_r`

### String
- [ ] `[]` / `[]=`（部分文字列アクセス）
- [ ] `gsub` / `sub` / `match`（正規表現）
- [ ] `split`, `strip`, `chomp`, `chop`
- [ ] `chars`, `bytes`, `lines`
- [ ] `start_with?`, `end_with?`
- [ ] `replace`, `freeze`
- [ ] `<<` (破壊的追加)
- [ ] `encode` / エンコーディング対応

### Array
- [ ] `each`, `map`, `select`, `reject`, `reduce`（ブロック必要）
- [ ] `sort`, `sort_by`
- [ ] `flatten`, `compact`, `uniq`
- [ ] `shift`, `unshift`
- [ ] `join`
- [ ] `zip`, `product`
- [ ] `-` (差集合), `&` (積集合), `|` (和集合)
- [ ] `delete`, `delete_at`
- [ ] `count`
- [ ] `<<` (push の別名)
- [ ] `[]` の Range 対応 (`a[1..3]`)

### Hash
- [ ] `each`, `map`, `select`（ブロック必要）
- [ ] `merge`, `update`
- [ ] `delete`
- [ ] `fetch`
- [ ] `each_key`, `each_value`
- [ ] `to_a`
- [ ] `default` / `default=`

### 未実装クラス
- [ ] Symbol
- [ ] Range
- [ ] Regexp / MatchData
- [ ] IO / File
- [ ] Comparable / Enumerable（モジュール）
- [ ] Kernel（メソッド: `puts`, `print`, `gets`, `require`, `raise` 等）
- [ ] Numeric (Integer/Float の共通親)

## ランタイム・内部実装の差分

### GC
- [ ] abruby オブジェクトの free（現在リーク前提）
- [ ] GC 安全性の完全な検証（大量 eval 時のクラッシュ報告あり）

### メソッドディスパッチ
- [ ] インラインキャッシュ（現在は毎回 strcmp で線形探索）
- [ ] メソッドテーブルの動的拡張（現在固定 64 エントリ）

### インスタンス変数
- [ ] 動的拡張（現在固定 32 エントリ）
- [ ] ivar 名のインデックス化（現在 strcmp 線形探索）

### 定数
- [ ] 動的拡張（現在固定 64 エントリ）

### VALUE スタック
- [ ] 動的拡張（現在固定 10000 スロット）
- [ ] スタックオーバーフロー検出

### ASTro 最適化
- [ ] ASTro 部分評価（特化ディスパッチャ生成）
- [ ] JIT コンパイル
- [ ] AOT コンパイル
- [ ] インラインキャッシュ + 部分評価の組み合わせ
