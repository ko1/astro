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
- [ ] **prologue 関数ポインタによるメソッドディスパッチ再設計** — 後述「prologue リファクタリング」を参照

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

## prologue リファクタリング

### 背景と動機

現在の `dispatch_method_frame` は全メソッドタイプ (AST / CFUNC / IVAR_GETTER / IVAR_SETTER) を1つの巨大な関数で処理している。問題:

1. **引数チェックがない** — argc が多すぎても少なすぎてもエラーにならず、多い場合はスタック上の他フレームの領域を踏み潰す危険がある
2. **specialize 時のコード膨張** — `dispatch_method_frame` は `static inline` なので、specialize すると全 call site に全タイプの分岐コードが展開される。ivar アクセスの call site にも CFUNC 分岐のコードが出る
3. **caller/callee の責務が不明確** — frame push/pop、self save/restore、argc の Qnil 埋めが caller と callee に散在

### 設計方針: CRuby 式の prologue 関数ポインタ

CRuby では `vm_call_iseq_setup` / `vm_call_cfunc` / `vm_call_ivar` 等のメソッドタイプ別呼び出し関数を `struct rb_callcache` に記録し、cache hit 時に直接呼び出す (参照: `vm_insnhelper.c`)。同じ方式を abruby に導入する。

```
現状:
  call site → dispatch_method_frame → mtype 分岐 → frame push → body or cfunc → frame pop

提案:
  call site → mc->prologue(c, call_site, mc, argc, arg_index)
              ↑ メソッドタイプごとの専用関数ポインタ
```

### prologue 関数のシグネチャ

```c
// 非 block 版
typedef RESULT (*method_prologue_t)(
    CTX *c, NODE *call_site,
    const struct method_cache *mc,
    unsigned int argc, uint32_t arg_index);

// block 版
typedef RESULT (*method_prologue_blk_t)(
    CTX *c, NODE *call_site,
    const struct method_cache *mc,
    unsigned int argc, uint32_t arg_index,
    const struct abruby_block *blk);
```

EVAL の `(CTX*, NODE*)` 固定シグネチャでは argc 等を渡せないため、prologue は EVAL とは別の関数ポインタにする。`method_cache` に `prologue` と `prologue_blk` の2フィールドを追加し、`method_cache_fill` でメソッドタイプに応じて設定する。

### 6つの prologue 関数

| 関数 | mtype | frame push | block | argc チェック |
|---|---|---|---|---|
| `prologue_iseq` | AST | する | なし | argc != params_cnt → ArgumentError |
| `prologue_cfunc` | CFUNC | する | なし | argc != params_cnt → ArgumentError |
| `prologue_ivar_getter` | IVAR_GETTER | しない | - | argc != 0 → ArgumentError |
| `prologue_ivar_setter` | IVAR_SETTER | しない | - | argc != 1 → ArgumentError |
| `prologue_iseq_with_block` | AST | する | BREAK demote | 同上 |
| `prologue_cfunc_with_block` | CFUNC | する | BREAK demote | 同上 |

各 prologue は、現在の `dispatch_method_frame` / `dispatch_method_frame_with_block` から該当メソッドタイプの処理を抽出したもの。

- **frame push/pop**: iseq/cfunc の prologue 内で行う。ivar はフレームなし (現状と同じ)
- **fp/cref の save/restore**: iseq prologue 内で行う
- **RESULT_RETURN skip-count**: iseq/cfunc prologue 内で処理
- **RESULT_BREAK demote**: _with_block 版のみ
- **self の save/restore**: prologue には含めない。call site 側の責務 (現状と同じ)

### call site の変更

```c
// node_method_call (cache hit):
VALUE save_self = c->self;
c->self = recv_val;
RESULT r = mc->prologue(c, n, mc, params_cnt, arg_index);
c->self = save_self;

// node_func_call (cache hit):
RESULT r = mc->prologue(c, n, mc, params_cnt, arg_index);

// block 付き (cache hit):
RESULT r = mc->prologue_blk(c, n, mc, params_cnt, arg_index, &blk);
```

`dispatch_method_frame` は `mc->prologue(...)` への1行 wrapper になるか、最終的に削除。

### block 付き ivar の扱い

`prologue_blk` は ivar 系で NULL。block 付き ivar 呼び出し (`obj.x { ... }`) は非 block 版の `mc->prologue` にフォールバック。ivar accessor は yield しないので BREAK は起きず安全。

### apply/splat の扱い

`node_func_call_apply` / `node_method_call_apply` は argc が実行時に変わるため、prologue の argc チェックが毎回走る。将来の PG specialize で call site の argc が定数化されれば、prologue ごとインライン展開されてチェックが消える。

### dispatch_method_with_klass (miss パス / super / method_missing)

一時的な `method_cache` を構築して `mc->prologue(...)` を呼ぶ。現状と同じ構造で、`dispatch_method_frame` の代わりに `mc->prologue` を呼ぶだけ。

### PG specialize との連携 (将来)

PG specialize では prologue の関数ポインタが定数になるので、gcc がその prologue 関数をインライン展開する。例:
- `prologue_ivar_getter` がインライン化 → `obj->ivars[slot]` への直接アクセスのみのコードが出る
- `prologue_iseq` がインライン化 → frame push + body dispatcher 呼び出しのコードが出る (mtype 分岐なし)
- method inlining と組み合わせれば body の中身まで展開される

### 実装手順

各ステップで `make test && make debug-test` が通ること。

1. `context.h` に prologue typedef を追加、`method_cache` にフィールド追加 (NULL 初期化)
2. 6つの prologue 関数を `node.def` に書く (現在の dispatch_method_frame から抽出、まだ未使用)
3. `method_cache_fill` で method type に応じて prologue/prologue_blk を設定
4. `dispatch_method_frame` の中身を `return mc->prologue(...)` に置換 ← **ビッグバン**
5. `dispatch_method_frame_with_block` も同様に置換
6. 各 prologue に argc チェックを追加 (ArgumentError)
7. (任意) hot path で `mc->prologue(...)` を直接呼び出し、wrapper 関数を削除

Step 1-3 は非機能変更 (prologue を書いて設定するだけ、まだ呼ばれない)。Step 4 が本番切り替え。

### 発展: handler 方式 (cache check も callee 側)

prologue リファクタリングの先にある、さらに踏み込んだ設計。call site のコードを間接呼び出し1個に削減する。

#### call site を究極まで小さくする

現状の call site は cache check (klass + serial 比較) + hit/miss 分岐がある。これを handler に全部任せる:

```c
// call site のコード (これだけ)
mc->handler(c, n, mc, recv, argc, arg_index)
```

handler は初期状態で `generic_handler`（method lookup → cache fill → dispatch）。初回呼び出し後、handler 自体を prologue_iseq 等に書き換える。prologue 側が cache check も担う:

```c
prologue_iseq(c, call_site, mc, recv, argc, arg_index) {
    klass = AB_CLASS_OF(recv);
    if (mc->klass != klass || mc->serial != abm->method_serial)
        return generic_handler(...);  // cache miss → 再 lookup
    // cache hit: frame push → body → frame pop
}
```

#### 算術演算ノードの統一

この方式の最大の利点は、**算術/比較ノードのバリエーション爆発を解消できる**こと。

現在 `+` 演算のために `node_plus` / `node_fixnum_plus` / `node_fixnum_plus_slow` / `node_fixnum_plus_overflow` / `node_integer_plus` / `node_flonum_plus` / `node_flonum_plus_slow` の 7 ノードが存在し、実行時に `swap_dispatcher` で AST を書き換えて切り替えている。`-`, `*`, `/`, `%`, `<`, `<=`, `>`, `>=`, `==`, `!=` も同様で、node.def の大部分がバリエーションで埋まっている。

handler 方式なら `node_plus` 1つで済む:

```c
NODE_DEF
node_plus(CTX *c, NODE *n, NODE *left, NODE *right, uint32_t arg_index,
          struct method_cache *mc@ref)
{
    VALUE lv = EVAL_ARG(c, left);
    VALUE rv = EVAL_ARG(c, right);
    return mc->handler(c, n, mc, lv, rv, arg_index);
}
```

handler が型ガードと method redefinition チェックを自由に組み合わせる:

```
handler_fixnum_plus:
  if (FIXNUM_P(lv) && FIXNUM_P(rv) && serial == cached)
    → tagged add (overflow check 付き)
  else
    → generic_plus_handler (method lookup → dispatch_method_with_klass)

handler_flonum_plus:
  if (FLONUM_P(lv) && FLONUM_P(rv) && serial == cached)
    → flonum add
  else
    → generic_plus_handler
```

利点:
- **node.def が劇的にシンプルになる** — 算術/比較/等値で ~50 ノード削減、`node_plus` 1つ + handler 群に統一
- **swap_dispatcher による AST 書き換えが不要** — handler の差し替えだけで型特化が完結
- **型ガードの自由度が高い** — `(Fixnum, Float)` 等の混在ケースも handler で表現可能
- **PG specialize で handler が定数化** → gcc が fixnum fast path をインライン展開、型ガードが定数畳み込みで消える
- **メソッド再定義への対応が自然** — serial 不一致で generic に fallback、再 fill で適切な handler に戻る

## ランタイム・内部実装

- [x] ~~abruby オブジェクトの free（現在リーク前提）~~ → `RUBY_DEFAULT_FREE` で GC sweep 時に解放
- [x] メソッド/ivar/定数テーブルの動的拡張 → `ab_id_table` (hybrid hash table) に移行済み
- [ ] スタックオーバーフロー検出
