# asom 実装済み機能

未実装 / 既知の課題は [todo.md](todo.md) を参照。詳しいランタイム構造は
[runtime.md](runtime.md) を参照。

## 目次

- [SOM 言語仕様](#som-言語仕様) — リテラル / 変数 / メソッド / ブロック / クラス
- [標準ライブラリ](#標準ライブラリ) — bootstrap class と stdlib overlay
- [ASTro 統合](#astro-統合) — 実行モード、AOT/PG
- [テスト](#テスト) — TestSuite / AreWeFastYet ベンチマーク

## SOM 言語仕様

SOM は教育用の Smalltalk dialect。文法仕様 ([SOM/specification/SOM.g4](https://github.com/SOM-st/SOM/blob/main/specification/SOM.g4)) に対する asom の対応:

### リテラル

- 整数: `42`, `-3`, `9223372036854775807`（`2^62` まで、tagged int）
- 浮動小数点: `1.5`, `-3.14`, `2.0e10`
- 文字列: `'hello'`、エスケープ `\t`/`\b`/`\n`/`\r`/`\f`/`\0`/`\\`/`\'` 対応、NUL byte を含む文字列も保持
- シンボル: `#foo`, `#'with spaces'`, `#between:and:`, `#+`, `#~`, `#&`, `#|`
- 配列リテラル: `#(1 2 'three' #four #(5 6))` — ネスト配列、負数、シンボル、混在 OK
- `nil`, `true`, `false`, `self`, `super`

### 変数

- インスタンス変数 (`| x y |` at class top) — 継承クラスから参照可（parse 時に親 fields を prepend して slot 配置）
- クラス側変数 (`----` 後の `| x y |`) — class object に一緒にアロケート
- ブロック / メソッドローカル: `| a b c |` at body top
- ブロック引数: `[ :x :y | ... ]`
- グローバル: 大文字始まりの識別子はクラス、見つからないとき `unknownGlobal:` フォールバック

### メッセージ送信

- Unary: `obj message`
- Binary: `a + b`, `a == b`, `a < b`, `a || b`, `a && b`, `a , b`
- Keyword: `a between: lo and: hi`, `obj at: i put: v`
- ↑ 引数 0–8 まで対応 (`node_send0` … `node_send8`)
- `super` 送信: unary / binary / keyword 全部対応 (`node_super_send0..8`)
- ws 区切りシンボル: `#between:and: withArguments:` は `between:and:` と `withArguments:` の二語

### ブロック

- 0–2 引数まで `Block1` / `Block2` / `Block3` の独自クラスインスタンスとして区別
  - `[42] class = Block1`
  - `[:x | x+1] class = Block2`
- `value`, `value:`, `value:with:`, `value:with:with:`
- `whileTrue:`, `whileFalse:`
- 非局所リターン (`^expr`): home メソッドが live なら setjmp/longjmp で unwind
- escape: home が return 済みのときは `escapedBlock: aBlock` を送信元に dispatch
- lexical scope chain: 親メソッド + 親ブロックの locals を `scope` 段で参照

### クラス

- `Foo = Bar (...)` で superclass 指定（無指定は Object）
- `Foo = nil (...)` は superclass なし (Object 用)
- per-class metaclass、metaclass-of-metaclass チェーンも適切
  - `1 class class superclass = Object class` 通る
- `----` 区切りで class-side / instance-side 分割
- `selector = primitive` 宣言は既存 C primitive を保持

### 制御構造

- `ifTrue:` / `ifFalse:` / `ifTrue:ifFalse:` / `ifFalse:ifTrue:`（True/False 双方）
- `ifNil:` / `ifNotNil:` / `ifNil:ifNotNil:` / `ifNotNil:ifNil:`（Object/Nil 双方）
- `and:` / `or:`、`&&` / `||`（block 引数を `value` 自動 invoke）
- `Integer>>to:do:`, `Integer>>downTo:do:`, `Integer>>timesRepeat:`
- `Integer>>to:by:do:`（stdlib）
- `Block>>whileTrue:`, `whileFalse:`

### リフレクション / メタプログラミング

- `Class>>methods` → ordered Array of Method mirror
- `Method>>signature` → Symbol、`Method>>holder` → Class
- `Class>>name`, `Class>>superclass`, `Class>>fields`, `Class>>asString`
- `Class>>fromString:` (Integer/Double に特化)
- `Object>>class`, `Object>>asString`, `Object>>print`, `Object>>println`
- `Object>>perform:`, `perform:withArguments:`, `perform:inSuperclass:`
- `Object>>instVarAt:`, `instVarAt:put:`, `instVarNamed:`, `instVarNamed:put:`
- `Object>>doesNotUnderstand:arguments:` フォールバック
- `Object>>unknownGlobal:` フォールバック (block 内のグローバル参照含む)

### 例外 / エラー

- `Object>>error:` — メッセージを表示して exit
- `Object>>subclassResponsibility` — 抽象メソッド宣言

## 標準ライブラリ

### Bootstrap classes (C primitive)

asom は SOM 標準ライブラリの主要クラスを **C で実装** したものをインストールする。
詳細は `asom_primitives.c` を参照。

| クラス | 主な primitive |
|--------|----------------|
| Object | `class`, `==`, `~~`, `=`, `~=`, `<>`, `isNil`, `notNil`, `value`, `hashcode`, `asString`, `print`, `println`, `inspect`, `ifNil:`, `ifNotNil:`, `ifNil:ifNotNil:`, `ifNotNil:ifNil:`, `subclassResponsibility`, `error:`, `,`, `perform:`, `perform:withArguments:`, `perform:inSuperclass:`, `instVarAt:`, `instVarAt:put:`, `instVarNamed:`, `instVarNamed:put:` |
| Class | `new`, `new:`, `new:withAll:`, `name`, `superclass`, `methods`, `fields`, `asString`, `fromString:`, `PositiveInfinity` |
| Method | `signature`, `holder` |
| Boolean (True/False) | `ifTrue:`, `ifFalse:`, `ifTrue:ifFalse:`, `ifFalse:ifTrue:`, `not`, `and:`, `or:`, `&`, `\|`, `&&`, `\|\|`, `asString` |
| Integer | `+`, `-`, `*`, `/`, `//`, `%`, `rem:`, `<`, `>`, `<=`, `>=`, `=`, `<>`, `negated`, `abs`, `max:`, `min:`, `asString`, `asInteger`, `asDouble`, `print`, `println`, `to:`, `to:do:`, `downTo:do:`, `timesRepeat:`, `sqrt`, `&`, `\|`, `bitXor:`, `<<`, `>>`, `>>>`, `as32BitSignedValue`, `as32BitUnsignedValue`, `round`, `floor`, `ceiling` |
| Double | `+`, `-`, `*`, `/`, `//`, `%`, `rem:`, `<`, `>`, `<=`, `>=`, `=`, `<>`, `~=`, `negated`, `abs`, `sqrt`, `sin`, `cos`, `log`, `exp`, `round`, `floor`, `ceiling`, `asString`, `asInteger`, `println` |
| String | `print`, `println`, `length`, `size`, `=`, `+`, `,`, `concatenate:`, `asString`, `asInteger`, `asSymbol`, `hashcode`, `at:`, `beginsWith:`, `primSubstringFrom:to:`, `isDigits`, `isLetters`, `isWhiteSpace` |
| Symbol | extends String 全 + `asString`（自身を返す）。VALUE インターン済みで `==` が成立 |
| Array | `at:`, `at:put:`, `length`, `size`, `putAll:`, `do:`, `doIndexes:`, `from:to:do:`, `copy`, `first`, `last`, `isEmpty`, `collect:`, `select:`, `inject:into:`, `contains:`, `indexOf:` (nil on not-found) |
| Block, Block1/2/3 | `value`, `value:`, `value:with:`, `value:with:with:`, `whileTrue:`, `whileFalse:` |
| Nil | `isNil`, `notNil`, `asString`, `println`, `ifNil:`, `ifNotNil:`, `ifNil:ifNotNil:`, `ifNotNil:ifNil:` |
| System | `printNewline`, `printString:`, `print:`, `println:`, `global:`, `global:put:`, `load:`, `exit:`, `ticks`, `time`, `fullGC`, `gcStats`, `totalCompilationTime` |
| Random | `initialize`, `next`, `seed:`（class-side、シングルトン） |

### Stdlib overlay merge

bootstrap class に `SOM/Smalltalk/<Name>.som` を起動時にマージ:

- `.som` の method が `primitive` 宣言 → 既存 C primitive を保持
- `.som` の AST method で、既存に primitive 無し → AST 版を install
- `.som` の AST method で、既存に primitive あり → スキップ（高速 primitive 優先）

これで `Array>>sum`, `Array>>average`, `Array>>join:`, `Block>>whileFalse:`,
`Boolean>>between:and:` 等の Smalltalk-side helpers が利用可能。

## ASTro 統合

### 実行モード（abruby 互換語彙）

```
asom <ClassName> [args...]                   # interp（デフォルト）
asom -c <ClassName> ...                      # AOT-compile + run
asom -p <ClassName> ...                      # run + post-run PG bake
asom -c -p <ClassName> ...                   # AOT before, PG after
asom --plain <ClassName> ...                 # bypass code store entirely
asom --aot-only ...                          # skip PGC index lookup
asom --compiled-only ...                     # warn if interp dispatcher used
asom --pg-threshold=N ...                    # default 100, env ASOM_PG_THRESHOLD
asom --code-store=DIR ...                    # default code_store/, env ASOM_CODE_STORE
asom --preload=A,B,C ...                     # eager-load classes for AOT bake
```

### コードストア
- `astro_cs_compile(body, file)` で `code_store/c/SD_<hash>.c` 生成
- `astro_cs_build(NULL)` で `make -j` → `o/*.o` → `all.so`
- `astro_cs_reload()` で世代別 `all.{N}.so` を `dlopen`
- `astro_cs_load(body, file)` で `n->head.dispatcher` を `SD_<hash>` に swap
- selector の bare-literal vs interned 不整合を `asom_class_lookup` の strcmp fallback で吸収
- `-rdynamic` リンクで SD shared object が `asom_send_slow`, `asom_invoke_ast` 等を resolve

### 型特化ノード（Integer 演算 / Array アクセス）

ホットなセレクタは parse 時に専用ノードに発行され、`asom_send` を経由
しない。すべて `@canonical=node_send1` または `=node_send2` で構造ハッシュ
共通化、SD コードシャードはスワップ前後どちらでも使える（`is_specialized`
が立った後の `swap_dispatcher` は no-op）。

```
# 1-arg send: Integer 演算
node_send1_intplus / intminus / inttimes
node_send1_intlt / intgt / intle / intge / inteq

# 1-arg send: Array
node_send1_arrayat                       ← Array at:

# 2-arg send: Array
node_send2_arrayatput                    ← Array at:put:
```

ファスト・パスはタグ・ビット 1〜2 個のチェック + インライン算術 / 配列
アクセス + 再タグ付け。型 guard が外れたときは
`swap_dispatcher(n, &kind_node_send1)` で汎用 send1/send2 に戻して
`asom_send` で再ディスパッチ（IC ヒット）。

- パース時発行 (`make_specialized_send1`, `make_specialized_send_array`):
  `+ - * < > <= >= =` および `at: / at:put:` のセレクタを最初から特化版で
  発行。warmup なしで AOT bake が特化版を捕捉する。
- ランタイム発行: `asom_method->prim_kind` を `def_prim_kind` で立てて
  おけば、parse-time に取りこぼした送信もスローパスでタイプ・フィードバック
  して `swap_dispatcher` で書き換える経路が残してある（保険）。

### 制御フロー・インライン化

`ifTrue: / ifFalse: / ifTrue:ifFalse: / ifFalse:ifTrue: / whileTrue: /
whileFalse:` のうち、ブロック引数（while 系では受信側ブロックも）が
**0 引数・0 ローカル・本体に nested block 生成を含まない**ものは、パース時
に専用 NODE に書き換える（`node_iftrue / iffalse / iftrue_iffalse /
iffalse_iftrue / whiletrue / whilefalse`）。

ランタイムは `asom_inline_frame_push` で C スタック上に空フレームを
1 個積み（`lexical_parent = c->frame`、locals=NULL）、ブロック本体を
直接 EVAL する。`asom_make_block` も `setjmp` も無し。`whileTrue:` は
ループ全体で 1 つのフレームを使い回すため、反復回数で線形に効果が
スケールする。

ifTrue:/ifFalse: 系は受信値が `val_true / val_false` でない場合の
フォールバックとして元の `node_block` を operand に保持し、`asom_send`
で正規の `ifTrue:` 等を送る（DNU 互換のため、benchmark では発火しない）。

エスケープ防止: 本体に nested block 生成があると、その closure が
スタック・フレームを `lexical_parent` に取り込み、関数戻り後に UAF と
なる。パース時の `subtree_creates_block()` がこれを検出し、当該パターン
は inline せず正規の send1/send2 で処理する。

### `m->no_nlr` で setjmp スキップ

メソッド本体に `node_block` がひとつも無ければ、そのメソッドに対する
non-local return（`^expr` from a nested block）は文法的に発生しえない。
パース時に `subtree_creates_block(body)` の結果を `ASOM_PARSED_METHOD->no_nlr`
にセットし、`asom_invoke` がフラグを見て setjmp 設置を丸ごとスキップする
（unwind 構造体も作らない）。

per-call ~50 ns の節約だが、再帰系の Towers / Queens / List では数百万回
呼ばれるので Towers 1.2×, Queens 1.15×, List 1.28× の追加スピードアップ。

ブロックにも同じ最適化を入れた。`subtree_has_nlr(block_body)` をパース時
に走らせ（`node_block` 子は越境せず — 子ブロックは自分の catcher を持つ；
inline CF ノードは body_block ではなく stmts に潜る）、結果を
`ALLOC_node_block` の `no_nlr` operand 経由で `asom_method->no_nlr` に
焼き、`asom_block_invoke` が escape-catcher の setjmp + unwind chain push
を完全に省略する。Mandelbrot プロファイルで `asom_block_invoke` が
18% → 5.5%、`__sigsetjmp` が消失。

### `and:` / `or:` の inline

`Boolean>>and:` / `Boolean>>or:` の引数が inlinable な block literal の
ときは、`node_and` / `node_or` に書き換える。意味的には
`^ self ifTrue: aBlock ifFalse: false`（`or:` は false/true 入れ替え）と
同じだが、true/false 受信時に `aBlock value` を呼ぶ代わりに stmts を
inline 評価する。

Mandelbrot 内側ループ `[notDone and: [z < 50]] whileTrue: [...]` では
`[z < 50]` が外側 while のイテレーションごとに `asom_make_block` され、
`frame->captured = true` を立てて enclosing frame を pool 外に固定して
いた。inline 後は内側ブロックがそもそも具現化されないので frame が
pool を循環し続け、`frame_alloc → __libc_calloc` 経由のメモリ使用が
プロファイルから消える。Mandelbrot AOT-cached: 2050ms → 1380ms (1.48×)。

### `whileTrue:` / `whileFalse:` の pool 版

既存の `node_whiletrue` / `whilefalse` はスタック割当の inline frame を
使うため、cond / body のサブツリーが closure を作りうる
（`subtree_creates_block` が真）と inline できず、代わりに
`asom_block_invoke` per iter にフォールバックしていた。inline CF 子
ノード（`node_iftrue` / `node_and` / etc.）は稀な non-Boolean
fallback 用に `body_block` を保持するので、bool 系を含む body は
ほぼ「block 生成あり」と判定されてしまう。

pool 版（`node_whiletrue_pool` / `node_whilefalse_pool`）は inline frame
を per-bucket frame pool から heap に確保する。closure escape があれば
`frame->captured = true` で pool 復帰を抑止する — `node_iftrue_pool` /
`node_to_do_pool` と同じ正当性。1 回の `whileTrue:` 呼び出しにつき
pool fetch + return が 1 回で、un-inlined 版の per-iter
`asom_block_invoke` ペアを置き換える。

Mandelbrot の 3 重 nested whileTrue: が全て inline 化される（最内
`[notDone and: [z < 50]] whileTrue: [...]` を含む）。AOT-cached
runtime: 1380ms → 860ms（1.6×、no_nlr-blocks 修正からの累積で 2.4×）。

クロスベンチ高速化（50 iters、AOT-cached、best-of-3、whileTrue_pool 前後）:

| benchmark    | before  | after  | speedup |
|--------------|---------|--------|---------|
| Sieve        | 7742us  | 5581us | 1.39× |
| Bounce       | 38881us | 30191us| 1.29× |
| List         | 138606us| 52524us| **2.64×** |
| Permute      | 83375us | 60083us| 1.39× |
| Storage      | 65721us | 48214us| 1.36× |
| TreeSort     | 93406us | 66488us| 1.41× |
| QuickSort    | 24764us | 8281us | **2.99×** |
| Towers       | 186216us|146689us| 1.27× |
| BubbleSort   | 15936us | 11142us| 1.43× |
| Fannkuch     | 223148us|112833us| 1.98× |
| Queens       | 49863us | 36806us| 1.35× |

### Block / method bump arena

`asom_make_block` も calloc 2 回（`struct asom_method` + `struct asom_block`）
してたので、隣接させた `struct asom_block_record { method; block }` を
slab で束ねて bump-allocation。Sieve outer to:do: の if-true ブロックの
ように毎回 fresh な block を作るパターンで、per-iter 2 calloc が消えて
2 回のポインタ進行になる。

double arena 同様、未回収（GC が必要）。1 回のベンチでは bounded。

### Double bump arena

`asom_double_new` は `calloc` だったが、bump-allocation slab に置き換え。
スラブ 1 個 = 4096 個の `asom_double` (約 96 KB)。スラブ枯渇時にだけ
`malloc` が走り、それ以外は `g_double_arena.next++` で 1 ns。

GC が無いので回収はしない（フレーム同様）。1 回のベンチ内では総量に
上限があるので問題なし。本格的に GC を入れるとき roots に追加。

性能効果: **Mandelbrot 0.794s → 0.556s (1.43× 高速化)**。NBody も
同様の数値計算系で寄与する見込み（手元では未計測）。

### Flonum tagging（Double を VALUE 即値化）

VALUE エンコーディングを 1-bit int タグから 2-bit に拡張し、CRuby 流の
biased-exponent 圧縮で **Double を VALUE word 内に即値で持つ**:

```
...x1   -> SmallInteger  (low bit 1, top 63 bits 値)
...10   -> Flonum        (low 2 bits 10, biased double bits)
...00   -> object pointer
```

代表値 (biased exponent ∈ [897, 1150]、abs ≈ 2^-126 〜 2^127) はロスレス
で 62 bit に詰まる。範囲外（denormal、巨大 exponent）は従来通り
`struct asom_double` を bump arena に boxed allocation。

この変更で Mandelbrot のような **中間 Double が無数に作られて即捨て** の
パターンで:

- `asom_double_new` の bump 進行 + class field write が消える
- heap-double の pointer-deref 経由のフィールド read が消える
- L1 cache 圧が下がる
- 結果値が register に乗ったまま次の演算に流れる

性能効果（5 iters × inner=350、AOT-cached、best-of-3）:

| ベンチ | flonum 前 | flonum 後 | 倍率 |
|---|---|---|---|
| Mandelbrot interp | 724 ms | **52 ms** | **14×** |
| Mandelbrot AOT | 503 ms | **33 ms** | **15×** |

Mandelbrot AOT は Truffle (425 ms) を **12.9× 引き離す** に変化。
他のベンチ（Sieve / Bounce / Storage / TreeSort / Towers /
QuickSort）は ±5% で同等 — 中間 Double を作らないので flonum
の出番がない。Bounce が変わらなかったことで「Ball field の boxed
double はそのまま」と判明、ここを潰すには **shape ベースの field
unbox** が要る（`done.md` の続き、保留）。

実装は `context.h` の VALUE マクロ群と `asom_runtime.{h,c}` /
`asom_primitives.c` の限定的な分岐追加だけ。AST / Node 層は無改造。
SD shard の Merkle hash も変わらず、AOT cache は再 bake で互換。

### `to:do:` / `to:by:do:` / `timesRepeat:` インライン化

`from to: end do: [ :var | body ]` のような Integer 受信の繰り返し系も
パース時に専用 NODE に書き換える。受信側／引数が SmallInteger なら
C-level の `for` ループで反復し、`asom_block_invoke` の per-iter
calloc + setjmp が消える（per-call 1 度のフレーム取得のみ）。

| パターン | 書き換え先 |
|--------|----------|
| `n to: m do: [:i \| ...]`           | `node_to_do` / `node_to_do_pool` |
| `n to: m by: s do: [:i \| ...]`     | `node_to_by_do` / `node_to_by_do_pool` |
| `n timesRepeat: [...]`              | `node_times_repeat` / `node_times_repeat_pool` |

エスケープ可能性で 2 種類に分岐:

- `_pool` 無し版 — 本体に nested block 生成なし → C スタック上のフレーム
  + 必要なら `slot` 1 個 (`locals = &slot`)。最速。
- `_pool` 版 — 本体に nested block 生成あり → pool（heap）から
  フレームを 1 個 pop。escape した closure は `captured` フラグで
  pool 外に固定し、`asom_block_invoke` 同等の安全性。

これにより Sieve のような outer to:do: が ifTrue: ネストを含んでいても
inline 化可能（pool 版を使う）。

### 性能効果（累積、interp / best-of-3）

| benchmark | baseline | + spec+pool | + 制御flow inline | + to:do: inline | 累積倍率 |
|-----------|----------|-------------|-------------------|------------------|---------|
| Sieve     | 0.032s   | 0.012s        | 0.009s          | 0.007s        | **4.6×** |
| Permute   | 0.044s   | 0.033s        | 0.031s          | 0.029s        | 1.52×    |
| Towers    | 0.125s   | 0.075s        | 0.073s          | 0.064s        | 1.95×    |
| Bounce    | 0.037s   | 0.008s        | 0.007s          | 0.007s        | **5.3×** |
| BubbleSort| 0.023s   | 0.007s        | 0.007s          | 0.007s        | **3.3×** |
| Mandelbrot| 1.015s   | 0.797s        | 0.556s †        | 0.493s        | **2.06×**|
| Queens    | 0.039s   | 0.028s        | 0.020s          | 0.017s        | **2.3×** |
| List      | 0.071s   | (n/a)         | 0.035s          | 0.031s        | 2.3×     |
| QuickSort | 0.031s   | (n/a)         | 0.013s          | 0.011s        | 2.8×     |
| TreeSort  | 0.080s   | (n/a)         | 0.046s          | 0.033s        | 2.4×     |

列: baseline / + 制御flow inline / + to:do: + double arena / + block arena

† Mandelbrot の値は double arena 後。to:do: 段階単独では 0.794s。

### フレーム pool

`asom_invoke` / `asom_block_invoke` のフレーム+locals は **slot 数バケット
別 free-list pool** (`g_frame_pool[16]`) で再利用する:

- `frame_alloc(slots)`: pool に空きがあれば pop（O(1)）、無ければ
  `frame + locals[]` をひとつの `calloc` で確保（旧コードは calloc 2 回）。
- `frame_free(frame)`: `captured == false` ならバケットに push。
  `captured == true` なら nested closure に lexical_parent として捕捉
  されているので、pool に戻すと alias する → そのままヒープに leak（旧
  コードと同じ振る舞い）。

`asom_make_block` は `c->frame->captured` と全 lexical 上位フレームの
`captured` を `true` に立てる。これで、closure escape の有無を実行時に
（保守的に）判定して frame 再利用と両立する。

性能効果: AreWeFastYet で Sieve 1.7×、Permute / Towers 1.3×、Queens 1.25×
の高速化。プロファイル上の `_int_malloc` / `__libc_calloc` 使用比率が
大幅減少。

## テスト

### SOM-st/SOM TestSuite — **216 / 221 (97.7%) アサーション pass、23 / 24 ファイル clean**

| ファイル | 結果 |
|----------|------|
| PreliminaryTest | 1/1 ✓ |
| BooleanTest | 16/16 ✓ |
| BlockTest | 13/13 ✓ |
| ArrayTest | 32/32 ✓ |
| GlobalTest | 3/3 ✓ |
| CoercionTest | 1/1 ✓ |
| CompilerReturnTest | 6/6 ✓ |
| SuperTest | 10/10 ✓ |
| ClosureTest | 1/1 ✓ |
| ReflectionTest | 7/7 ✓ |
| SystemTest | 2/2 ✓ |
| ClassLoadingTest | 1/1 ✓ |
| ClassStructureTest | 6/6 ✓ |
| DoesNotUnderstandTest | 3/3 ✓ |
| SpecialSelectorsTest | 1/1 ✓ |
| VectorTest | 28/28 ✓ |
| SetTest | 9/9 ✓ |
| DictionaryTest | 5/5 ✓ |
| HashTest | 1/1 ✓ |
| DoubleTest | 27/27 ✓ |
| SelfBlockTest | 1/1 ✓ |
| StringTest | 17/17 ✓ |
| SymbolTest | 5/5 ✓ |
| **IntegerTest** | **20/25** — 残り 5 失敗は全部 Bignum (>2⁶²) |

### AreWeFastYet ベンチマーク — 16 種が verifyResult 込みで OK

```
Sieve, Permute, Towers, Queens, List, Storage, Bounce,
BubbleSort, QuickSort, TreeSort, Mandelbrot, Fannkuch,
Richards, DeltaBlue, Json, NBody, GraphSearch
```

### 比較スクリプト `make bench`

`test/run_compare.rb` がエンジン横並び比較を回す。`/usr/bin/time -f '%e'`
で各 trial をラップし、**1 回の実行から inner-work と wall-clock の両方
を取得**する（best of 3、メトリクスごとに独立に最小を採る）。最後に
2 つのテーブルを並べて出力:

```sh
make bench ITERS=30
```

- **Inner-work** — 各エンジンの `BenchmarkHarness`（asom 自前 `Bench.som`
  または SOM-st 標準）が `system ticks` で計測したベンチループ部分。
  プロセス起動・stdlib parse・JVM bootstrap・eager JIT compile を
  除外できるので、起動コストが大きく違う engine 同士でも公平に比較できる。
- **Wall-clock** — `/usr/bin/time -f '%e'` 計測。ユーザ体感に近いが
  起動コストが支配的になりがち。参考値。

軸:

- **asom interp / aot / pg** — 自身の `Bench.som`
- **SOM++** — C++ bytecode VM、`USE_TAGGING + COPYING + -O3 -flto`
- **TruffleSOM** — Java + GraalVM CE 25 + libgraal JIT

CSOM と plain-CPython PySOM は外した。CSOM は教育用 reference 実装で
性能比較の対象ではない上、sustained scale だと per-bench 4 分以上かかる。
PySOM は plain CPython 走行だと桁違いに遅く、本来は RPython で translate
した JIT バイナリ (`make som-ast-jit`) を使うべき — pypy ソース (~1.5 GB)
と pypy2 / python2 ランタイムが必要なので、translate された binary を
用意したら column を追加する。

`make bench-aot BENCH=Sieve ITERS=500` / `make bench-pg ...` で個別計測。
