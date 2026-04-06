# abruby 実装済み機能

## リテラル
- 整数 (Fixnum + Bignum): `42`, `-3`, `100000000000000000000`
- 浮動小数点 (Flonum + heap Float): `1.5`, `3.14`, `1.0e100`
- 文字列: `"hello"`, `"value is #{expr}"`（補間対応）
- Symbol: `:foo`, `:empty?`, `:save!`
- 配列: `[1, 2, 3]`, `[]`
- ハッシュ: `{"a" => 1}`, `{a: 1}`（Symbol キー対応）, `{}`
- Range: `1..10`（inclusive）, `1...10`（exclusive）
- Regexp: `/pattern/`, `/pattern/i`
- ヒアドキュメント: `<<~HEREDOC ... HEREDOC`
- `%w(a b c)` → `["a", "b", "c"]`
- `%i(a b c)` → `[:a, :b, :c]`
- `true`, `false`, `nil`

## 変数
- ローカル変数: `a = 1; a`
- 複合代入: `+=`, `-=`, `*=` 等
- インスタンス変数: `@x = 1; @x`（複合代入対応）
- `self`（トップレベルは main オブジェクト）

## 制御構造
- `if / elsif / else / end`
- `unless / else / end`
- `while / end`
- `until / end`
- 後置 if/unless: `expr if cond`
- 三項演算子: `cond ? a : b`
- `&&` / `||` / `and` / `or`（短絡評価、値を返す）
- `!` / `not`
- `return`（明示的 return、値あり/なし）

## メソッド
- `def name(args); end`
- endless method: `def name(args) = expr`
- 再帰呼び出し
- 全メソッド呼び出しは OOP ディスパッチ（`node_method_call` に統一）
- `method_missing(name, ...)`

## クラス・モジュール
- `class Name; end`
- `class Child < Parent; end`（継承）
- `module Name; end`
- `include ModName`（mixin）
- `Class#new` + `initialize`
- クラスの再オープン
- クラスオブジェクトは VALUE（定数参照で取得）
- 定数参照: `Float::INFINITY` 等 (`::` 構文)
- Class < Module の継承関係

## 演算子（全てメソッドディスパッチ）
- 算術: `+`, `-`, `*`, `/`, `%`, `**`, `-@`
- 比較: `<`, `<=`, `>`, `>=`, `==`, `!=`, `<=>`
- ビット: `&`, `|`, `^`, `~`, `<<`, `>>`
- インデックス: `[]`, `[]=`
- 追加: `<<` (Array#push, String 破壊的追加)

## ビルトインクラス
- **Object**: inspect, to_s, ==, !=, !, nil?, class, p
- **Module**: inspect, include
- **Class**: new, inspect (Module を継承)
- **Integer**: 算術, 比較, **, <=>, <<, >>, &, |, ^, ~, to_s, to_f, zero?, abs
- **Float**: 算術, 比較, **, to_s, to_i, to_f, abs, zero?, floor, ceil, round
- **String**: +, *, 比較, length/size, empty?, upcase, downcase, reverse, include?, to_s, to_i, inspect
- **Array**: [], []=, push, pop, length/size, empty?, first, last, +, include?, inspect
- **Hash**: [], []=, length/size, empty?, has_key?/key?, keys, values, inspect
- **Symbol**: ==, !=, to_s, to_sym, inspect（CRuby 即値を直接利用）
- **Range**: first, last, begin, end, exclude_end?, size/length, include?, to_a, ==, inspect, to_s
- **Regexp**: match?, match, =~, ==, source, inspect, to_s（CRuby Regexp を内部利用）
- **TrueClass / FalseClass / NilClass**: inspect, to_s, nil?

## 定数
- ビルトインクラスは Object の定数
- `Float::INFINITY`, `Float::NAN`
- ユーザ定義クラスは per-instance main_class の定数

## ランタイム
- AbRuby インスタンスごとに独立した VM 状態
- `AbRuby.new` で新しい環境、`AbRuby.eval` は一時インスタンス
- T_DATA 統一構造（全ヒープオブジェクトの先頭に klass）
- `AB_CLASS_OF` は static inline（Fixnum/Bignum/Float は即値チェック、T_DATA は直引き）
- `ab_verify()` によるデバッグアサーション（`--enable-debug`）
- AST pretty print（`--dump`）
- builtin/ にクラスごとのソース分離
