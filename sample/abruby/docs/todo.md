# abruby TODO: 未実装の Ruby 機能

実装済み機能は [done.md](done.md) を参照。

## optcarrot 対応

benchmark/optcarrot/bin/optcarrot-bench を動かすために必要な機能。
優先度順にリストアップ。

### 致命的（これがないと何も動かない）

- [x] ブロック / yield（block 基盤、`{ ... }` / `do...end`, `yield`, `block_given?`, `next`, `break`, 非ローカル `return`, closure, super block forwarding）
- [x] Proc / lambda / `&block` パラメータ（`Proc.new`, `proc`, `lambda`, `Proc#call`, `Proc#[]`, `Proc#arity`, `Proc#lambda?`, `def f(&blk)`, `f(&proc)` — `->` 記法と `&:symbol` は未対応）
- [x] Fiber（`Fiber.new`, `Fiber#resume`, `Fiber.yield`, `Fiber#alive?` — CRuby Fiber API でスタック管理）
- [x] `case / when`（if/elsif チェーンに desugar、=== メソッド対応）
- [x] `attr_reader` / `attr_writer` / `attr_accessor`
- [ ] デフォルト引数 (`def f(a, b = 1)`)

### 重要（主要な処理パスで使用）

- [x] `Integer#times`
- [x] `Integer#[]`（ビットインデックス `num[bit]`）
- [x] Array: `each`, `each_with_index`, `map`/`collect`, `select`/`filter`, `reject`, `flat_map`/`collect_concat`, `fill`, `flatten`, `clear`, `replace`, `concat`, `join`, `min`, `max`, `sort`, `inject`/`reduce`, `uniq`, `compact`, `transpose`, `zip`, `pack`, `all?`, `any?`, `none?`
- [x] Hash: `each`/`each_pair`, `each_key`, `each_value`, `fetch`, `merge`, `delete`, `dup`, `compare_by_identity`
- [x] String: `split`, `strip`, `chomp`, `bytes`, `unpack`, `bytesize`, `start_with?`, `end_with?`, `tr`, `=~`, `%`, `sum`, `to_sym`/`intern`
- [ ] String: `gsub`, `sub`, `match`, `scan`, `[]`, `[]=`
- [x] `super`（bare super で引数転送、super(args) で明示的引数、super() で引数なし）
- [x] `private` / `public` / `protected` / `module_function`（定義のみ、アクセス制御は no-op）
- [x] `until` ループ（基本形は実装済み。`begin...end until` は未対応）

### 中程度（特定機能で必要）

- [x] `eval`（ローカル変数は外部スコープ不可）
- [x] `Struct.new`（最小限、attr_accessor 付きクラス生成）
- [x] `method(:name)`（Method オブジェクト、`call`/`[]` で呼び出し可能）
- [x] `const_get` / `const_set`（動的定数参照）
- [x] `is_a?` / `kind_of?` / `instance_of?`
- [ ] `defined?`
- [ ] クラス変数 (`@@var`)
- [ ] 可変長引数 受け取り (`def f(*args)`, `def f(**kwargs)`) — 呼び出し側 splat は実装済
- [ ] `&:symbol`（ブロック引数のシンボル記法）

### 標準ライブラリ

- [x] File I/O（join, binread, dirname, basename, extname, expand_path, exist?, readable?, read — CRuby File への facade）
- [ ] Zlib（ROM 解凍）
- [ ] Marshal（シリアライゼーション）

## その他の未実装機能

### 制御構造
- [ ] `for .. in`
- [x] `next`（block body から、値付き/値なし両対応）

### ブロック・Proc・lambda
- [x] `Proc.new` / `proc` / `lambda` — block の heap escape、closure 環境保持
- [x] `&block` 引数 / `&proc_var` 転送
- [ ] `->` lambda リテラル構文
- [ ] `&:symbol` sugar
- [ ] block 内 default / *args / **kwargs パラメータ
- [ ] `_1`, `_2` 番号付きパラメータ
- [ ] `redo`

### メソッド
- [ ] キーワード引数 (`def f(a:, b: 1)`)
- [ ] クラスメソッド (`def self.foo`)
- [ ] `alias` / `alias_method`
- [x] `respond_to?`

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
- [x] `even?`, `odd?`, `step`
- [ ] `upto`, `downto`
- [ ] `bit_length`, `between?`

#### Float
- [ ] `nan?`, `infinite?`, `finite?`
- [ ] `truncate`

#### String
- [x] `start_with?`, `end_with?`, `tr`, `%`
- [ ] `[]=`（部分文字列代入）
- [ ] `sub`, `gsub`, `match`, `scan`

#### Array
- [x] `sort`, `compact`, `uniq`, `uniq!`, `shift`, `unshift`, `join`, `inject`/`reduce`
- [x] `transpose`, `zip`, `rotate!`, `flat_map`/`collect_concat`, `pack`
- [x] `min`, `max`, `fill`, `clear`, `replace`, `concat`, `slice`, `slice!`
- [x] `all?`, `any?`, `none?`, `each_with_index`
- [ ] `delete`, `delete_at`, `count`
- [ ] `each_slice`

#### Hash
- [x] `each_key`, `each_value`, `merge`, `delete`, `fetch`, `dup`, `compare_by_identity`
- [ ] `to_a`, `default`

### 未実装クラス
- [x] File（最小限 facade: join, binread, dirname, basename, extname, expand_path, exist?, readable?, read）
- [x] GC（thin facade: disable, enable, start）
- [x] Process（clock_gettime）
- [ ] IO（File の基底）
- [ ] Comparable, Enumerable
- [ ] Numeric (Integer/Float の共通親)

## 高速化

### 実測状況（2026-04-11 時点, `abruby+compiled` vs `ruby`）

- **勝っているベンチ**: while 0.08×, collatz 0.10×, method_call 0.18×, fib 0.21×, gcd 0.21×, nested_loop 0.19×, dispatch 0.30×, ack 0.30×, tak 0.26×, ivar 0.23×, sieve 0.47×, hash 0.70×, object 0.84×
- **同等**: nbody 1.11×
- **負けているベンチ**: mandelbrot 1.93× (Float 特化済だが method dispatch 含む残存コスト),
  binary_trees 1.97× (T_DATA 大量確保が主因), factorial 1.18× (Bignum 乗算), string 2.79× (`s += "x"` による allocation churn)

### 残る課題

### メソッドディスパッチ

- [x] メソッド名インターン化 — 全メソッド名をパース時に intern し、ID ポインタ比較に置換。実装済み
- [x] インラインキャッシュ — `node_method_call` に `struct method_cache` を `@ref` で埋め込み、ヒット時に `abruby_class_find_method` を完全スキップ。グローバルシリアルでメソッド定義・include 時に無効化
- [x] メソッドテーブルのハッシュ化 — `ab_id_table` を hybrid 実装に変更（小テーブルは packed linear、大テーブルは open-addressing Fibonacci hash）。IC ミス時のメソッド探索・builtin クラスのメソッド参照を高速化

### AOT / JIT（ASTro 部分評価）

- [ ] AOT ベンチマーク測定 — `make bench` で plain vs compiled の性能差を把握。compiled テストは通っているので基盤は動作済み
- [ ] ループ選択的 JIT — `dispatch_cnt` 閾値超えノードをバックグラウンドで gcc コンパイル → dlopen → swap_dispatcher
- [ ] specialize でのメソッドインライン化 — 現在 `node_def` は `@noinline` で specialize をブロック。型フィードバックと組み合わせてメソッドボディを展開できれば、method lookup + frame push/pop が消える
- [ ] specialize でのローカル変数レジスタ化 — gcc LICM はポインタエイリアスで `c->fp[i]` をホイストできない。specialize レベルで C ローカル変数にマッピングすれば回避可能
- [ ] ガード削除 — ループ内の型安定変数に対し、ループ入口で1回だけ型チェックし、ボディ内の FIXNUM_P チェックを除去（Truffle/Graal の speculation + deopt パターン）

### CTX フィールドの frame 移行

- [ ] `c->self`, `c->fp` 等を `abruby_frame` に移動し、CTX から廃止する。frame push/pop で自動的に save/restore されるため、`node_method_call` の手動 save/restore が不要になりコードが簡潔になる。`c->current_frame->self` / `c->current_frame->fp` と間接参照が1段増えるが、gcc が基本ブロック内でレジスタに保持できれば実質コストは小さい。specialize でのローカル変数レジスタ化と組み合わせれば LICM 問題も回避可能

### インタプリタ改善

- [ ] スーパーインストラクション — 頻出パターン（`while(lvar < const)`, `lvar = lvar + num` 等）を融合ノードに。AOT 無効時のインタプリタ高速化
- [ ] NodeHead スリム化 — `parent`(8), `hash_value`(8), `dispatcher_name`(8), `jit_status`(4), `dispatch_cnt`(4) はホットパスで不要。32B 削減すれば union データが dispatcher と同一キャッシュラインに収まる。コアジェネレータ変更が必要
- [ ] 末尾呼び出し最適化 — `return method_call(...)` パターンを検出しフレーム再利用。再帰のスタック消費削減

### メモリ・GC

- [ ] abruby_object の ivar inline slots をクラスごとに可変化 — 現在は全オブジェクト固定 4 slots (`ABRUBY_OBJECT_INLINE_IVARS`)。`klass->ivar_shape.cnt` を見て alloc 時にちょうどいいサイズを確保し、`extra_ivars` を不要にする。flexible array member で `ivars[]` をクラスの shape に合わせれば、ivar 0 個のクラス (binary_trees) は 0 bytes、ivar 7 個のクラス (nbody Body) は 56 bytes inline で heap alloc なし。クラス再オープンで shape が伸びた場合のみ realloc
- [ ] ノードのアリーナアロケータ — 個別 malloc → バンプポインタ。同一スコープのノードが隣接しキャッシュ局所性向上
- [ ] VALUE スタック遅延 GC マーク — 現在 10,000 スロット全体をマーク。`fp + frame_size` までに限定

### オブジェクトシステム

- [x] ivar インラインキャッシュ — `node_ivar_get/set` に `struct ivar_cache *@ref` を埋め込み、
  `(klass, slot)` ガードで `obj->ivars.entries[slot]` を直接参照。ivar/nbody で大幅に効く
- [ ] シェイプベース ivar アクセス — ivar の名前線形探索を固定オフセットに。CRuby のオブジェクトシェイプと同様の手法。
  現在の ivar IC はキャッシュエントリが object 毎の entry slot を示すが、
  shape なら class 単位で共有できる。binary_trees 等の大量オブジェクト生成で更なる高速化余地
- [ ] case/when ジャンプテーブル — 現在 if/elsif チェーンに desugar。整数リテラル when はジャンプテーブル化

### 機能追加（最適化の前提条件）

- [ ] ブロック / yield + インライン化 — optcarrot 必須。specialize でブロック呼び出しを展開できればイテレータのオーバーヘッド除去。**method inlining の基盤が先に必要**。20260412 に iterator ごとに融合ノードを手書きする方式を試したが、N イテレータで N ノードが必要になり scale せず、かつフレーム共有が `binding` 等で破綻するため差し戻し (詳細: `docs/report/20260412_phase1_block_speedup.md`)。正しい方向は PE + inlining で (1) cfunc iterator または (2) AST で書き直した iterator を inline 展開

## ランタイム・内部実装

- [x] ~~abruby オブジェクトの free（現在リーク前提）~~ → `RUBY_DEFAULT_FREE` で GC sweep 時に解放
- [x] メソッド/ivar/定数テーブルの動的拡張 → `ab_id_table` (hybrid hash table) に移行済み
- [ ] スタックオーバーフロー検出
