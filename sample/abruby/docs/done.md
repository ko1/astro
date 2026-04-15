# abruby 実装済み機能

未実装機能 / 既知のバグは [todo.md](todo.md) を参照。

## 目次

- 言語機能: [リテラル](#リテラル) / [変数](#変数) / [制御構造](#制御構造) / [メソッド](#メソッド) / [ブロック](#ブロック) / [Proc / lambda](#proc--lambda) / [Fiber](#fiber) / [クラス・モジュール](#クラスモジュール) / [演算子](#演算子全てメソッドディスパッチ) / [定数](#定数)
- ライブラリ: [ビルトインクラス](#ビルトインクラス)
- 実行系: [ランタイム](#ランタイム) / [高速化](#高速化)

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
- 多重代入: `a, b = 1, 2`（ローカル変数、ivar、gvar、配列インデックス、属性 対応）
- インスタンス変数: `@x = 1; @x`（複合代入対応、shape-based slot、4 slots inline + heap overflow）
- グローバル変数: `$x = 1; $x`（複合代入対応、ab_id_table で動的管理）
- `||=` / `&&=` 演算子（ローカル変数、ivar、gvar、配列インデックス、属性ターゲット対応）
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
- デフォルト引数 (`def f(a, b = 1)`)
- splat 引数 — 呼び出し側 (`foo(*args)`, `foo(a, *b, c)`, `obj.m(*x, y)`) と
  受け取り側 (`def f(*args)`) の両方。呼び出し側はパース時に
  `[a] + b + [c]` 形式の Array 式に lower し、`node_func_call_apply` /
  `node_method_call_apply` が実行時に Array を c->fp に unpack してディスパッチ。
  最大 `ABRUBY_APPLY_MAX_ARGS=32` 要素。`**kwargs` は未対応
- クラスメソッド `def self.foo`（`singleton_table_class()` 経由でクラス自身の
  methods テーブルに登録）
- `attr_reader`, `attr_writer`, `attr_accessor`
- `super`（bare / 引数付き / 引数なし）
- 再帰呼び出し
- `node_method_call`（明示的レシーバ）/ `node_func_call`（暗黙 self-call、recv 操作なし）
- `method_missing(name, ...)`
- `method(:name)` — Method オブジェクト (`call` / `[]` で呼び出し可能)
- `respond_to?`
- `eval`（ローカル変数は外部スコープ不可）
- インラインキャッシュ（`method_cache`: klass + method + serial + ivar_slot + body + dispatcher、グローバル `method_serial` でメソッド定義 / include 時に無効化）
- メソッド名インターン化（全メソッド名をパース時に intern、ID ポインタ比較）

## ブロック
- ブロックリテラル: `obj.m { |x| ... }` / `obj.m do |x, y| ... end`
- `yield` / `yield args`（`def f; yield; end` 形式）
- `block_given?`（Kernel メソッド）
- `next` / `next value`（block body から脱出、yield の返り値に）
- `break` / `break value`（block から脱出 → yielding メソッドの call から戻る）
- **非ローカル `return`**（block 内 `return` は block を *定義した* メソッドから脱出）
- outer method のローカル変数への closure アクセス
- `super` が現メソッドの block を暗黙に転送、`super() { ... }` で明示的置き換え
- `node_method_call_with_block` / `node_func_call_with_block` / `node_super_with_block` — block 無し hot path を汚さないための分離ノード
- Block 情報は call サイトの `node_block_literal`（body, params_cnt, param_base）から C スタックの `struct abruby_block` に組み立て、`struct abruby_frame` の block ポインタに格納
- Block 内の `yield` / `super` は `abruby_context_frame(c)` ヘルパー経由で lexical enclosing frame を参照
- `abruby_yield` C helper で builtin iterator (`Integer#times`, `Array#each/map/select/reject`, `Hash#each`, `Range#each`) を実装
- RESULT state 拡張: `NORMAL=0, RETURN=1, RAISE=2, BREAK=4, NEXT=8`（bit flag）
- RESULT skip-count 方式の非ローカル return: `RESULT.state` の上位ビット（`RESULT_SKIP_SHIFT` 以降）に「何回 method boundary を skip するか」を埋め、method 境界で skip==0 なら catch、そうでなければ decrement して propagate。frame push 時にカウンタを増やす必要がなく、非ブロック経路の `return` は追加コスト 0

## Proc / lambda
- `Proc.new { ... }` / `proc { ... }` — ブロックを Proc オブジェクトに変換
- `lambda { ... }` — lambda を生成（return が lambda 自身からの脱出に）
- `Proc#call` / `Proc#[]` / `Proc#yield` — Proc の実行
- `Proc#arity` — パラメータ数
- `Proc#lambda?` — lambda かどうか
- `Proc#to_proc` — 自身を返す
- `def f(&blk)` — ブロックを Proc として受け取る（`&block` パラメータ）
- `f(&my_proc)` — Proc をブロックとして渡す
- closure: 定義スコープのローカル変数を読み書き可能
- ブロックの heap escape: `abruby_block_to_proc()` がスタック上の `abruby_block` を heap の `abruby_proc` に変換

## Fiber
- `Fiber.new { |arg| ... }` — ファイバー生成（CRuby Fiber API でスタック管理）
- `Fiber#resume(args)` — ファイバー開始/再開
- `Fiber.yield(value)` — ファイバー中断、resumer に値を返す
- `Fiber#alive?` — ファイバーが生存中かどうか
- 状態管理: NEW → RUNNING → SUSPENDED → DONE
- closure: 定義スコープのローカル変数アクセス可能
- ファイバー内でのメソッド呼び出し、例外処理
- GC 連携: CRuby Fiber 経由の machine stack scan、GC stress テスト済み

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
- `private` / `public` / `protected` / `module_function`（定義のみ、アクセス制御は no-op）

## 演算子（全てメソッドディスパッチ）
- 算術: `+`, `-`, `*`, `/`, `%`, `**`, `-@`
- 比較: `<`, `<=`, `>`, `>=`, `==`, `!=`, `<=>`
- ビット: `&`, `|`, `^`, `~`, `<<`, `>>`
- インデックス: `[]`, `[]=`
- 追加: `<<` (Array#push, String 破壊的追加)

## ビルトインクラス
- **Kernel** (module, Object に include): p, puts, print, raise, exit, eval, block_given?, loop, require, require_relative, Integer(), Float(), Rational(), Complex(), proc, lambda, __dir__
- **RuntimeError**: message, backtrace, to_s, inspect（例外オブジェクト）
- **Object**: inspect, to_s, ==, !=, ===, !, nil?, class, is_a?, kind_of?, instance_of?, respond_to?, instance_variable_get, instance_variable_set, method, freeze, frozen?, tap, object_id, equal?, dup, hash, send, __send__, public_send
- **Module**: ===, inspect, include, const_get, const_set, private, public, protected, module_function, attr_reader, attr_writer, attr_accessor
- **Class**: new (Module を継承)
- **Integer**: 算術(+,-,*,/,%,**,-@), 比較(<,<=,>,>=,==,!=,<=>), ビット(<<,>>,&,|,^,~,[]), to_s, inspect, to_f, zero?, abs, even?, odd?, times, step
- **Float**: 算術(+,-,*,/,%,**,-@), 比較(<,<=,>,>=,==,!=), to_s, inspect, to_i, to_f, abs, zero?, floor, ceil, round
- **String**: +, <<, *, 比較(==,!=,<,<=,>,>=), length/size, empty?, upcase, downcase, reverse, include?, start_with?, end_with?, chomp, strip, split, tr, =~, bytes, %, sum, bytesize, unpack, to_s, inspect, to_i, to_sym/intern
- **Symbol**: ==, !=, to_s, to_sym, inspect（CRuby 即値を直接利用）
- **Array**: [], []=, push, <<, pop, shift, unshift, length/size, empty?, first, last, +, *, include?, inspect/to_s, each, each_with_index, map/collect, select/filter, reject, flat_map/collect_concat, reverse, dup, flatten, concat, join, min, max, sort, fill, clear, replace, transpose, zip, pack, inject/reduce, uniq, uniq!, compact, slice, slice!, ==, !=, all?, any?, none?, rotate!
- **Hash**: [], []=, length/size, empty?, has_key?/key?, keys, values, inspect/to_s, each/each_pair, each_key, each_value, merge, delete, fetch, dup, compare_by_identity
- **Range**: first/begin, last/end, exclude_end?, size/length, include?, ===, to_a, ==, inspect, to_s, each, map/collect, all?, any?, inject/reduce
- **Regexp**: match?, ===, match (※ bool 返却、MatchData 未実装), ==, =~, source, inspect, to_s（CRuby Regexp を内部利用）
- **Rational**: 算術(+,-,*,/,**,-@), 比較(<,<=,>,>=,==,<=>), numerator, denominator, to_f, to_i, to_r, inspect, to_s
- **Complex**: 算術(+,-,*,/,**,-@), ==, real, imaginary, abs, conjugate, rectangular, to_f, to_c, inspect, to_s
- **TrueClass / FalseClass / NilClass**: inspect, to_s, nil? (NilClass のみ `[]` も対応)
- **Proc**: call, lambda?, arity, to_proc
- **Fiber**: resume, yield (class method), alive?
- **GC**: disable, enable, start（CRuby GC への thin facade）
- **Struct**: Struct.new（最小限、attr_accessor 付きクラス生成）
- **File**: join, binread, dirname, basename, extname, expand_path, exist?, readable?, read（CRuby File への facade）
- **Process**: clock_gettime（CLOCK_MONOTONIC 固定）

## 定数
- ビルトインクラスは Object の定数
- `Float::INFINITY`, `Float::NAN`
- ユーザ定義クラスは per-instance main_class の定数

## ランタイム
- AbRuby インスタンスごとに独立した VM 状態
- `abruby_machine` に `method_serial`（インラインキャッシュ無効化用、インスタンスごと）
- `AbRuby.new` で新しい環境、`AbRuby.eval` は一時インスタンス
- T_DATA 統一構造（全ヒープオブジェクトの先頭に klass + obj_type）
- `AB_CLASS_OF` は static inline（即値は `AB_CLASS_OF_IMM` に分離）
- `ab_verify()` によるデバッグアサーション（`--enable-debug`）
- AST pretty print（`--dump`）
- builtin/ にクラスごとのソース分離
- `caller_node` フレーム方式（各フレームが呼び出し元を記録、backtrace 生成に使用）
- `PUSH_FRAME` / `POP_FRAME` マクロ（インライン例外で backtrace 位置を記録）
- `no_stack_protector` 属性（DISPATCH / コードストア SD_ 関数）
- `abruby_cref` による lexical 定数スコープチェーン

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
- **NodeHead スリム化**: `parent`(8), `jit_status`(4), `dispatch_cnt`(4) を除去し、フィールドを cold/warm/hot ゾーンに再配置。72B→56B (NODE: 144B→128B)。dispatcher と union データが常に同一キャッシュラインに収まる。ASTroGen に `ASTRO_NODEHEAD_PARENT` 等の条件付きガードを追加
- **メソッドテーブルのハッシュ化**: `ab_id_table` を hybrid 実装に変更（小テーブルは packed linear、
  大テーブルは open-addressing Fibonacci hash）。テーブル構造体内に `inline_storage[4]` を埋め込み、
  小テーブルは別途 alloc 不要。IC ミス時のメソッド探索・builtin クラスのメソッド参照を高速化
- **prologue 関数ポインタ**: argc 特化 4 種 (`prologue_ast_simple_0/1/2/n`) +
  `prologue_ast_complex` + `prologue_cfunc` + `prologue_ivar_getter/setter`。すべて `always_inline`
- **GC mark の最適化**: `abruby_data_mark` が immediate value (nil, Fixnum, Symbol, Flonum, true/false)
  を `rb_gc_mark` に渡さずスキップ。binary_trees の leaf node mark コスト削減
- **Object#== の identity 短絡**: `node_eq` / `node_neq` で `lv == rv` なら method dispatch せず直接返す。
  `node_arith_fallback` は method が `ab_object_eq` に解決された場合も identity でショートカット
- **call node の mtype 特化 (Phase 1)**: `method_cache_fill` が mtype (AST simple / cfunc / ivar) を
  確定させた後、`node_call0/1/2` を `node_call0/1/2_{ast,cfunc,ivar_{get,set}}` に `swap_dispatcher`。
  特化版 dispatcher は `prologue_ast_simple_N` / `prologue_cfunc` / `prologue_ivar_{getter,setter}` を
  名前で直接呼び出し、prologue は `always_inline` で展開されるため `mc->prologue` 間接呼び出しが消える。
  **設計判断**: 特化は `NODE_DEF` による別 kind として定義 (kind が増えるが、各 EVAL 関数が子 dispatcher
  をパラメータとして受け取る形になるので ASTroGen の specialize 機構から子ノード (recv の node_self 等)
  の SD_ 関数ポインタを代入して inline できる。plain function にすると
  `recv->head.dispatcher(c, recv)` が runtime 読み取りとなり specialize 不可になる)。
- **CTX フィールドの frame 移行**: `c->self`, `c->fp` を `abruby_frame` に移動し CTX から廃止。
  frame push/pop で自動的に save/restore されるため、`node_method_call` の手動 save/restore が不要に。
  以前 (2026-04-14) は `saved_self`/`saved_fp`/`saved_cref` 追加方式で frame +24B のリグレッションが
  出ていたが、最終的には frame サイズを抑えた形 (`{prev, method, caller_node, block, self, fp, entry}`)
  で導入し性能維持。`cref` は `entry` 経由で参照するため frame には保持しない
  cache miss 時は `specialized_call_miss` が `mc->demote_cnt` を bump し、generic dispatcher を
  直接呼ぶ (recv/args の double EVAL を回避)。`demote_cnt >= ABRUBY_CALL_POLY_THRESHOLD` (=2) で
  megamorphic 判定、以後 specialize しない (bm_dispatch のような 3-class 循環で swap 往復を防止)。
  plain モードで fib -14%, method_call -15%, ack -11%。compiled モードでは SD_ が generic の
  `EVAL_node_call1` をハードコードで呼ぶため runtime swap は効かない — Phase 2 (JIT による SD_ 再生成)
  の課題
