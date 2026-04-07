# abruby TODO: 未実装の Ruby 機能

実装済み機能は [done.md](done.md) を参照。

## optcarrot 対応

benchmark/optcarrot/bin/optcarrot-bench を動かすために必要な機能。
優先度順にリストアップ。

### 致命的（これがないと何も動かない）

- [ ] ブロック / yield / Proc（全イテレータの基盤）
- [x] `case / when`（if/elsif チェーンに desugar、=== メソッド対応）
- [x] `attr_reader` / `attr_writer` / `attr_accessor`
- [ ] デフォルト引数 (`def f(a, b = 1)`)

### 重要（主要な処理パスで使用）

- [ ] `Integer#times`（ブロック依存）
- [x] `Integer#[]`（ビットインデックス `num[bit]`）
- [ ] Array: `each`, `map`, `select`, `reject`, `fill`, `flatten`, `clear`, `replace`, `concat`（ブロック依存多数）
- [ ] Hash: `each`, `fetch`, `merge`, `delete`, `compare_by_identity`
- [ ] String: `gsub`, `split`, `strip`, `chomp`, `bytes`, `pack`, `unpack`, `bytesize`, `[]`
- [x] `super`（bare super で引数転送、super(args) で明示的引数、super() で引数なし）
- [ ] `private` / `public` / `protected`
- [ ] `module_function`
- [x] `until` ループ（基本形は実装済み。`begin...end until` は未対応）

### 中程度（特定機能で必要）

- [x] `eval`（ローカル変数は外部スコープ不可）
- [ ] `Struct.new`
- [ ] `method(:name)`（メソッドオブジェクト）
- [x] `const_get` / `const_set`（動的定数参照）
- [x] `is_a?` / `kind_of?` / `instance_of?`
- [ ] `defined?`
- [ ] クラス変数 (`@@var`)
- [ ] 可変長引数 (`*args`, `**kwargs`)
- [ ] `&:symbol`（ブロック引数のシンボル記法）

### 標準ライブラリ

- [ ] Zlib（ROM 解凍）
- [ ] File I/O（ROM 読み込み）
- [ ] Marshal（シリアライゼーション）

## その他の未実装機能

### 制御構造
- [ ] `for .. in`
- [ ] `next`

### ブロック・Proc・lambda
- [ ] `Proc.new` / `proc` / `lambda` / `->`
- [ ] `&block` 引数

### メソッド
- [ ] キーワード引数 (`def f(a:, b: 1)`)
- [ ] クラスメソッド (`def self.foo`)
- [ ] `alias` / `alias_method`
- [ ] `respond_to?`

### クラス・モジュール
- [ ] `prepend`
- [ ] `extend`
- [ ] ネストしたクラス/モジュール (`class A::B`)
- [ ] シングルトンメソッド / 特異クラス
- [ ] `ancestors`

### 変数・定数
- [ ] クラス変数 (`@@var`)

### ビルトインメソッドの差分

#### Integer
- [ ] `upto`, `downto`（ブロック必要）
- [ ] `step`（ブロック必要）
- [ ] `even?`, `odd?`
- [ ] `bit_length`, `between?`

#### Float
- [ ] `nan?`, `infinite?`, `finite?`
- [ ] `truncate`

#### String
- [ ] `[]=`（部分文字列代入）
- [ ] `sub`, `match`（正規表現）
- [ ] `start_with?`, `end_with?`
- [ ] `tr`, `scan`
- [ ] `%` フォーマット

#### Array
- [ ] `sort`, `compact`, `uniq`
- [ ] `shift`, `unshift`, `join`
- [ ] `delete`, `delete_at`, `count`
- [ ] `each_slice`, `transpose`, `rotate`, `zip`
- [ ] `inject` / `reduce`

#### Hash
- [ ] `to_a`, `default`
- [ ] `each_key`, `each_value`

### 未実装クラス
- [ ] IO/File
- [ ] Comparable, Enumerable
- [ ] Numeric (Integer/Float の共通親)

## ランタイム・内部実装

- [x] ~~abruby オブジェクトの free（現在リーク前提）~~ → `RUBY_DEFAULT_FREE` で GC sweep 時に解放
- [ ] インラインキャッシュ（現在 strcmp 線形探索）
- [ ] メソッド/ivar/定数テーブルの動的拡張
- [ ] スタックオーバーフロー検出
- [ ] ASTro 部分評価 / JIT / AOT
