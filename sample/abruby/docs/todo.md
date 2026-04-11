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

### インタプリタ改善

- [ ] スーパーインストラクション — 頻出パターン（`while(lvar < const)`, `lvar = lvar + num` 等）を融合ノードに。AOT 無効時のインタプリタ高速化
- [ ] NodeHead スリム化 — `parent`(8), `hash_value`(8), `dispatcher_name`(8), `jit_status`(4), `dispatch_cnt`(4) はホットパスで不要。32B 削減すれば union データが dispatcher と同一キャッシュラインに収まる。コアジェネレータ変更が必要
- [ ] 末尾呼び出し最適化 — `return method_call(...)` パターンを検出しフレーム再利用。再帰のスタック消費削減

### メモリ・GC

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

- [ ] ブロック / yield + インライン化 — optcarrot 必須。specialize でブロック呼び出しを展開できればイテレータのオーバーヘッド除去

## ランタイム・内部実装

- [x] ~~abruby オブジェクトの free（現在リーク前提）~~ → `RUBY_DEFAULT_FREE` で GC sweep 時に解放
- [ ] メソッド/ivar/定数テーブルの動的拡張
- [ ] スタックオーバーフロー検出
