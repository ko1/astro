# abruby TODO: 未実装の Ruby 機能

実装済み機能は [done.md](done.md) を参照。

## 言語機能

### 制御構造
- [ ] `case / when / in`
- [ ] `for .. in`
- [x] `break`（while/until から脱出、値付き対応）
- [ ] `next`
- [x] `begin / rescue / ensure / raise`（例外処理）— RuntimeError 限定、rescue はクラス引数なし
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
- [ ] シングルトンメソッド / 特異クラス
- [ ] `is_a?` / `kind_of?` / `instance_of?`
- [ ] `ancestors`

### 変数・定数
- [x] グローバル変数 (`$var`)
- [ ] クラス変数 (`@@var`)
- [x] 定数代入 (`FOO = 42`)
- [x] 多重代入 (`a, b = 1, 2`)

### リテラル・型
（Symbol, Range, Regexp, ヒアドキュメント, %w, %i は実装済み。done.md 参照）

### 演算子
（基本演算子は実装済み。done.md 参照）

## ビルトインメソッドの差分

### Integer
- [ ] `times`, `upto`, `downto`（ブロック必要）
- [ ] `even?`, `odd?`

### Float
- [ ] `nan?`, `infinite?`, `finite?`
- [ ] `truncate`

### String
- [ ] `[]` / `[]=`（部分文字列）
- [ ] `gsub` / `sub` / `match`（正規表現）
- [ ] `split`, `strip`, `chomp`, `chop`
- [ ] `start_with?`, `end_with?`

### Array
- [ ] `each`, `map`, `select`, `reject`, `reduce`（ブロック必要）
- [ ] `sort`, `flatten`, `compact`, `uniq`
- [ ] `shift`, `unshift`, `join`
- [ ] `delete`, `delete_at`, `count`

### Hash
- [ ] `each`, `map`, `select`（ブロック必要）
- [ ] `merge`, `delete`, `fetch`
- [ ] `to_a`, `default`

### 未実装クラス
- [ ] IO/File
- [ ] Comparable, Enumerable
- [ ] Numeric (Integer/Float の共通親)

## ランタイム・内部実装

- [ ] abruby オブジェクトの free（現在リーク前提）
- [ ] インラインキャッシュ（現在 strcmp 線形探索）
- [ ] メソッド/ivar/定数テーブルの動的拡張
- [ ] スタックオーバーフロー検出
- [ ] ASTro 部分評価 / JIT / AOT
