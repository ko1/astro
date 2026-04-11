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
- Rational: `3r`, `Rational(1, 3)`
- Complex: `2i`, `Complex(1, 2)`
- `true`, `false`, `nil`

## 変数
- ローカル変数: `a = 1; a`
- 複合代入: `+=`, `-=`, `*=` 等
- 多重代入: `a, b = 1, 2`（ローカル変数、ivar、gvar 対応）
- インスタンス変数: `@x = 1; @x`（複合代入対応）
- グローバル変数: `$x = 1; $x`（複合代入対応）
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
- `break`（while/until から脱出、値付き対応）
- `case / when / else`（if/elsif に desugar、=== によるマッチング）
- `raise "msg"`（RuntimeError 例外オブジェクトを生成、バックトレース付き）
- `begin / rescue / ensure / end`（rescue はクラス引数なし、`=> e` で例外オブジェクトを束縛）
- バックトレース（`err.backtrace` で `"file:line:in 'method'"` の配列を取得）

## メソッド
- `def name(args); end`
- endless method: `def name(args) = expr`
- `attr_reader`, `attr_writer`, `attr_accessor`
- `super`（bare / 引数付き / 引数なし）
- 再帰呼び出し
- `node_method_call`（明示的レシーバ）/ `node_func_call`（暗黙 self-call、recv 操作なし）
- `method_missing(name, ...)`
- インラインキャッシュ（`method_cache`: klass + method + serial + body + dispatcher）

## ブロック
- ブロックリテラル: `obj.m { |x| ... }` / `obj.m do |x, y| ... end`
- `yield` / `yield args`（`def f; yield; end` 形式）
- `block_given?`（Kernel メソッド、current_frame->prev->block を参照）
- `next` / `next value`（block body から脱出、yield の返り値に）
- `break` / `break value`（block から脱出 → yielding メソッドの call から戻る）
- **非ローカル `return`**（block 内 `return` は block を *定義した* メソッドから脱出）
- outer method のローカル変数への closure アクセス
- `super` が現メソッドの block を暗黙に転送、`super() { ... }` で明示的置き換え
- `node_method_call_with_block` / `node_func_call_with_block` / `node_super_with_block` — block 無し hot path を汚さないための分離ノード
- Block 情報は call サイトの `node_block_literal`（body, params_cnt, param_base）から C スタックの `struct abruby_block` に組み立て、`struct abruby_frame` の block ポインタに格納
- Block 内の `yield` / `super` は block の defining_frame を参照（呼び出し元メソッドの block に到達）
- `abruby_yield` C helper で builtin iterator (`Integer#times`, `Array#each/map/select/reject`, `Hash#each`, `Range#each`) を実装
- RESULT state 拡張: `NORMAL=0, RETURN=1, RAISE=2, BREAK=4, NEXT=8`（bit flag）
- Frame ID 方式の非ローカル return: `CTX::return_target_frame_id` が 0 (wildcard) か `frame.frame_id` 一致で demote、block return のみ defining frame id を target 化

## クラス・モジュール
- `class Name; end`
- `class Child < Parent; end`（継承）
- `module Name; end`
- `include ModName`（mixin）
- `Class#new` + `initialize`
- クラスの再オープン
- クラスオブジェクトは VALUE（定数参照で取得）
- 定数代入: `FOO = 42`
- 定数参照: `Float::INFINITY` 等 (`::` 構文)
- Class < Module の継承関係

## 演算子（全てメソッドディスパッチ）
- 算術: `+`, `-`, `*`, `/`, `%`, `**`, `-@`
- 比較: `<`, `<=`, `>`, `>=`, `==`, `!=`, `<=>`
- ビット: `&`, `|`, `^`, `~`, `<<`, `>>`
- インデックス: `[]`, `[]=`
- 追加: `<<` (Array#push, String 破壊的追加)

## ビルトインクラス
- **Kernel** (module, Object に include): p, raise, eval, Rational(), Complex(), require, require_relative
- **RuntimeError**: message, backtrace, to_s, inspect（例外オブジェクト）
- **Object**: inspect, to_s, ==, !=, ===, !, nil?, class, is_a?, kind_of?, instance_of?
- **Module**: inspect, include, const_get, const_set
- **Class**: new, inspect (Module を継承)
- **Integer**: 算術, 比較, **, <=>, <<, >>, &, |, ^, ~, [], to_s, to_f, zero?, abs, **times**
- **Float**: 算術, 比較, **, to_s, to_i, to_f, abs, zero?, floor, ceil, round
- **String**: +, *, 比較, length/size, empty?, upcase, downcase, reverse, include?, to_s, to_i, inspect
- **Array**: [], []=, push, pop, length/size, empty?, first, last, +, include?, inspect, **each**, **map** (alias collect), **select** (alias filter), **reject**
- **Hash**: [], []=, length/size, empty?, has_key?/key?, keys, values, inspect, **each** (alias each_pair)
- **Symbol**: ==, !=, to_s, to_sym, inspect（CRuby 即値を直接利用）
- **Range**: first, last, begin, end, exclude_end?, size/length, include?, to_a, ==, inspect, to_s, **each**
- **Regexp**: match?, match, =~, ==, source, inspect, to_s（CRuby Regexp を内部利用）
- **Rational**: 算術, 比較, numerator, denominator, to_f, to_i, inspect, to_s
- **Complex**: 算術, ==, real, imaginary, abs, conjugate, rectangular, inspect, to_s
- **TrueClass / FalseClass / NilClass**: inspect, to_s, nil?

## 定数
- ビルトインクラスは Object の定数
- `Float::INFINITY`, `Float::NAN`
- ユーザ定義クラスは per-instance main_class の定数

## ランタイム
- AbRuby インスタンスごとに独立した VM 状態
- `abruby_machine` に `method_serial`（インラインキャッシュ無効化用、インスタンスごと）
- `AbRuby.new` で新しい環境、`AbRuby.eval` は一時インスタンス
- T_DATA 統一構造（全ヒープオブジェクトの先頭に klass）
- `AB_CLASS_OF` は static inline（即値は `AB_CLASS_OF_IMM` に分離）
- `ab_verify()` によるデバッグアサーション（`--enable-debug`）
- AST pretty print（`--dump`）
- builtin/ にクラスごとのソース分離
- `caller_node` フレーム方式（各フレームが呼び出し元を記録、backtrace 生成に使用）
- `PUSH_FRAME` / `POP_FRAME` マクロ（インライン例外で backtrace 位置を記録）
- `no_stack_protector` 属性（DISPATCH / コードストア SD_ 関数）

## 高速化
- **Float 算術 fast path**: `node_flonum_plus/minus/mul/div` と `node_flonum_lt/le/gt/ge` を追加。
  `node_fixnum_*_slow` が Flonum/Float の両オペランドを観測したら `swap_dispatcher` で flonum 系に昇格。
  mandelbrot/nbody 等の Float-heavy ベンチが大幅改善
- **ivar インラインキャッシュ + 直接インデックス**: `abruby_object` は `klass` + `ivar_cnt` + inline 4 slots
  + heap extra。クラスの `ivar_shape` (name → slot_idx) で shape-based slot 割当。
  `node_ivar_get/set` は IC cache `(klass, slot)` のみでガード、直接 `obj->ivars[slot]` アクセス
- **attr_reader / attr_writer の dispatch インライン化**: `abruby_class_add_method` で
  `def x; @x; end` / `def x=(v); @x = v; end` の AST 形を検出し `ABRUBY_METHOD_IVAR_GETTER/SETTER`
  として保存。`method_cache_fill` で slot を先計算、`dispatch_method_frame` の先頭で frame 構築を
  スキップして直接 ivar 読み書き
- **ab_id_table ハイブリッド化 + inline storage**: 小テーブル（capa ≤ 4）は packed linear、
  大テーブルは open-addressing Fibonacci ハッシュ。テーブル構造体内に `inline_storage[4]` を埋め込み、
  小テーブルは別途 alloc 不要
- **Bignum 演算**: `integer_*_calc` を `rb_big_*` 直接呼び出しに変更（rb_funcall バイパス）。factorial が改善
- **String#+ / String#* のバッファ事前確保**: `rb_str_buf_new(total_size)` で再 alloc を回避
- **RESULT state ビットフラグ化**: `RETURN=1, RAISE=2, BREAK=4` に変更。メソッド境界での RETURN catch は
  `r.state &= ~RESULT_RETURN` の 1 命令に
- **frame.klass 削除**: `abruby_method::defining_class` に移し、`dispatch_method_frame` の
  frame 構築コストを 1 store 分減らす
- **EVAL_* を `static inline`**: 生成された evaluator 関数に `inline` キーワードを付け、specializer が
  出力する SD_ ラッパーとの組み合わせで gcc が木全体を cross-node 最適化しやすくする
- **GC mark の最適化**: `abruby_data_mark` が immediate value (nil, Fixnum, Symbol, Flonum, true/false)
  を `rb_gc_mark` に渡さずスキップ。binary_trees の leaf node mark コスト削減
- **Object#== の identity 短絡**: `node_eq` / `node_neq` で `lv == rv` なら method dispatch せず直接返す。
  `node_arith_fallback` は method が `ab_object_eq` に解決された場合も identity でショートカット
