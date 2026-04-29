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

### 型特化ノード（Integer 演算）

整数算術／比較セレクタの 1-arg 送信は専用ノードに発行する:

```
node_send1_intplus / intminus / inttimes
node_send1_intlt / intgt / intle / intge / inteq
```

ファスト・パスはタグ・ビット 2 個の AND + 算術 + 再タグ付け。型 guard が
外れたときは `swap_dispatcher(n, &kind_node_send1)` で汎用 send1 に戻して
`asom_send` で再ディスパッチ（IC ヒット）。

- パース時発行 (`make_specialized_send1`): `+ - * < > <= >= =` のセレクタを
  最初から特化版で発行。warmup なしで AOT bake が特化版を捕捉する。
- ランタイム発行: `asom_method->prim_kind` を `def_prim_kind` で立てておけば、
  parse-time に取りこぼした送信もスローパスでタイプ・フィードバックして
  `swap_dispatcher` で書き換える経路が残してある（保険）。
- すべての特化版は `@canonical=node_send1` で構造ハッシュを共通化、SD コード
  シャードはスワップ前後どちらでも使える（is_specialized が立った後の
  `swap_dispatcher` は no-op）。

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

### 比較スクリプト `make compare` / `make compare-wall`

二モードの 8 軸比較:

```sh
make compare ITERS=30        # inner-work (system ticks 自己計測)
make compare-wall ITERS=30   # wall-clock (process 起動から exit まで)
```

- **inner-work モード (デフォルト)** — 各エンジンの `BenchmarkHarness`
  (asom 自前 `Bench.som` または SOM-st 標準) が `system ticks` で計測した
  ベンチループ部分だけを抽出。プロセス起動・stdlib parse・JVM bootstrap・
  eager JIT compile を除外できるので、起動コストが大きく違う engine 同士
  (CSOM ~700ms class-load, TruffleSOM ~1.7s JVM+JIT) でも公平に比較できる。
- **wall-clock モード** — `/usr/bin/time -f '%e'` 計測。ユーザ体感に近いが、
  起動コストが支配的でフェアな比較にならない。参考値。

軸:

- **asom interp / aot / pg** — 自身の `Bench.som`
- **SOM++** — C++ bytecode VM、`USE_TAGGING + COPYING + -O3 -flto`
- **TruffleSOM** — Java + GraalVM CE 25 + libgraal JIT
- **PySOM AST / BC** — plain CPython 3.12 (RPython 翻訳なし)
- **CSOM** — plain C bytecode VM、参考値（教育用 naive 実装で peak target ではない）

`make bench-aot BENCH=Sieve ITERS=500` / `make bench-pg ...` で個別計測。
