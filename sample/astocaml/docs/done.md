# 実装済み機能

## 言語

### 値の表現

- 1-bit タグ付き `int64_t` (`bit 0 = 1` で 62-bit fixnum, それ以外は 8-byte aligned ヒープポインタ)
- シングルトン: `unit` / `true` / `false` / `[]` を静的 `oobj` のアドレスで表現
- ヒープオブジェクト型: `cons` / `string` / `closure` / `prim` / `tuple` / `ref` / `float` (boxed) / `variant` / `record` / `array` / `lazy` / `bytes` / `object`

### 制御構造

- `if-then-else` (`else` 省略時 `()`)
- `;` (sequencing) — 左結合
- `begin..end` / `( e )` (ブロック)
- `try ... with` 例外捕捉
- `match e with | exception E -> handler | p -> body` (例外 arm — `try` を伴う形に desugar)
- 末尾再帰最適化 (TCO) — トランポリンによる定数スタック実行 (1M 段階の再帰可)

### 束縛

- `let x = e in body`, `let rec f = e in body`, `let f x y = body`
- `let rec f = ... and g = ... in body` (ネスト相互再帰、二段階パース)
- トップレベル `let rec ... and ...`
- パターン束縛 `let (a, b) = e in body` (top + nested), `let { x; y } = ...`
- `let _ = e` (副作用)
- `let open M in body` (ローカル open — runtime alias)
- 中置演算子の関数定義: 単文字 `(+)`、多文字 `(+!)` `(<*>)` 等
- 演算子としての関数値: `(+) (-) (*) (/) mod (+.) (-.) (*.) (/.) (<) (>) (<=) (>=) (=) (<>) (^) (::)`、加えてユーザ定義の任意 `(<op>)`

### 関数

- `fun x y z -> body`, `fun () -> body`
- `function | p1 -> e1 | ...`
- 多引数クロージャ + 部分適用 + 過適用
- ラベル付き引数 `~x` / `~x:value`、オプション引数 `?x` (位置引数として簡略化処理)
- 末尾位置の関数呼び出しは tail-app に書き換わり、`oc_apply` のトランポリンに乗る (1M 段の再帰でもスタック使用量一定)
- カスタム中置演算子: `+!` `<*>` 等。先頭文字に応じた演算子優先 (parse_add / parse_mul / parse_concat / parse_cmp)

### パターンマッチ

完全な再帰的 pattern struct を持つ。各 arm は単一フレーム push に統合される `node_match_arm` ノードに desugar される。

サポートしているパターン:

- ワイルドカード `_`
- 変数束縛 `x`
- リテラル: 整数 / `true` / `false` / `()` / `[]` / 文字列 / 文字
- `h :: t` (ネスト可)
- タプル `(p1, p2, ..., pN)` (ネスト可)
- バリアント `Foo` / `Foo p` (引数あり; 多相バリアント `\`Foo` / `\`Bar n` も可)
- レコード `{ f1 = p1; f2 = p2 }`, パンニング `{ f1; f2 }`
- as-pattern `p as x` (両方束縛)
- or-pattern `p1 | p2 | ...` (match arm レベル)
- range pattern `'a' .. 'z'`, `0 .. 9`
- `when` ガード — マッチしてもガードが false なら次の arm にフォールスルー
- `| exception E -> handler` (`try` 統合)

### 演算子

- 整数: `+ - * / mod`, 単項 `-`
- 浮動小数点 (boxed double): `+. -. *. /.`, 単項 `-.`
- 論理: `&& || not` (短絡)
- 比較 (多相, fixnum インラインファストパス): `< > <= >= = <> == !=`
- リスト/文字列: `::` (右結合), `^` (左結合, 文字列連結)
- 参照: `!` (deref, prefix), `:=` (assignment), `<-` (record/object フィールド代入)
- リスト/配列: `e.(i)` (配列インデックス), `r.f` (レコードフィールド), `obj#m args` (メソッド送信)

### 例外

- `try ... with ...` (`setjmp`/`longjmp` ベース; ハンドラスタック深 256)
- `raise e`, `failwith s`, `invalid_arg s`, `assert e`
- 既定例外: `Failure` / `Not_found` / `Invalid_argument` / `Division_by_zero` / `Match_failure` / `Assert_failure` / `Exit`
- ユーザ定義 `exception E of T`

### 型

- 型宣言 (`type t = ...`) は構文だけ受理し本体スキップ
- バリアント `type t = Foo | Bar of int | Baz of int * string`
- 多相型変数 `'a` (構文だけ受理。ランタイム動的)
- レコード `type p = { x : int; y : int }` 完全サポート
- 多相バリアント `\`Foo` / `\`Bar 42`
- 型アノテーション (`(e : ty)`, `let f x : ty = ...`) は構文受理のみ

### モジュール / オブジェクト

- `module M = struct ... end` (ネスト可)
- 内部参照は modular-aware gref (`gref_q`) で `M.foo` と `foo` の両方を試行
- `module M = N` (alias)
- `open M` で `M.x` を `x` で呼べるよう alias
- `let open M in body` (ローカル open、runtime alias 経由)
- `module type S = ...` / `module M : S = ...` (シグネチャは構文受理のみ)
- `module M = functor (X : S) -> ...` (functor 構文受理のみ; 機能は未実装)
- `class NAME params = object ... end`:
  - `val [mutable] f = expr` 宣言
  - `method m args = body` 宣言
  - `inherit ParentClass args` (基本的な単一継承; 親のフィールド/メソッドを取り込み, 上書き)
  - `initializer body` (オブジェクト構築直後に self バインドで実行)
  - `new NAME args` でインスタンス化 (0-arg は `new NAME ()`)
  - `obj#method args` でメソッド送信
  - `obj#field` / `obj#set_field v` (auto getter/setter fallback)
  - method body 内で `field_name` (bare) は class fields をスキャンして `self#field` に解決 (pre-scan によるフォワード参照可)
  - `field_name <- value` (bare) は `node_field_assign` に変換

### Lazy

- `lazy expr` で thunk 生成
- `Lazy.force x` で評価 + キャッシュ
- `Lazy.is_val x` で評価済みかチェック

### リテラル

- `123` (int)
- `1.5` / `.5` / `1e10` / `1.5e-3` (浮動小数点)
- `'a'` / `'\n'` (char)
- `"hello\n"` / `"\""` (string with escape)
- `()`, `true`, `false`, `[]`
- `[1; 2; 3]`, `[| 1; 2; 3 |]` (list / array literal)

### Stdlib (組み込み)

- I/O: `print_int / print_string / print_endline / print_newline / print_char / print_float`
- 変換: `string_of_int / int_of_string / string_of_float / float_of_int / int_of_float / float_of_string`
- 比較: `compare / min / max / abs`
- 例外: `failwith / invalid_arg / raise / assert`
- 参照: `ref`
- Lazy: `Lazy.force / .is_val`
- printf: `Printf.printf / .eprintf / .sprintf / printf` (`%d %s %f %c %b %% %x %o`、 `%5d` `%.3f` 等の幅・精度指定)
- List: `length / hd / tl` (ローカル) と `List.{length, hd, tl, rev, append, map, filter, fold_left, fold_right, iter, nth, mem, assoc, assoc_opt, mem_assoc, exists, for_all, find, find_opt, init, flatten, concat, combine, split, partition, sort, iter2, map2}`
- String: `String.{length, get, sub, concat, make, contains, index, uppercase_ascii, lowercase_ascii}`
- Bytes: `Bytes.{create, make, length, get, set, to_string, of_string, blit}`
- Array: `Array.{make, length, get, set}`, リテラル `[| 1; 2; 3 |]`, インデックス `a.(i)`
- Char: `Char.code / .chr` (chars are ints)
- 数学: `sqrt / sin / cos / log / exp / floor / ceil` (gcc builtin)
- Hashtbl: `Hashtbl.{create, add, find, mem, remove, length, iter, replace}`
- Stack: `Stack.{create, push, pop, top, is_empty, length}`
- Queue: `Queue.{create, add, push, pop, is_empty, length}`
- Buffer: `Buffer.{create, add_string, add_char, contents, length, clear}`

## 型システム (HM-lite)

- パース後・評価前に型推論器を走らせ、型エラーを早期検出 (デフォルトで ON; `--no-check` で off)
- 型: `int`, `bool`, `unit`, `string`, `float`, `char`, `'a` (var), `'a -> 'b`, `'a list`, `'a ref`, `'a array`, `'a * 'b * ...` (tuple), `?` (any)
- 単一化 (`ty_unify`) ベース。occurs check も入っている
- 各 AST ノードに対し `infer(node, env, level) -> ty` を再帰実行
- カバー範囲:
  - 算術 (`+ - * / mod` int 要求, `+. -. *. /.` float)
  - 論理 (`&& || not` bool)
  - 比較 (両辺型一致を要求)
  - 文字列連結 (`^` string)
  - リスト (`::`, `[]`, `hd`, `tl`)
  - 参照 (`ref`, `!`, `:=`)
  - if condition は bool、then/else 型一致
  - 関数適用 — 関数型と引数型を unify
  - let / let rec / fun
  - try/with の body と handler 型一致
- 未カバー (`TY_ANY` で素通り):
  - グローバル参照 (gref) — global の型情報を持っていない
  - バリアント / レコード / オブジェクト / モジュール / lazy
  - パターンマッチ arm (`node_match_arm`)
  - タプル / 配列 のリテラル
  - let-polymorphism — generalize/instantiate 未実装
- ランタイム型チェック (補完): `node_add` 等が冒頭で `OC_IS_INT(av) & OC_IS_INT(bv)` を確認、失敗で `Type_error` 例外
- エラー報告: stderr に "type error: ..." と展開済み型を出力。複数のエラーをまとめて出して継続

## 型システム — let-polymorphism + global tracking + functor (追加)

- **let-polymorphism**: `let id x = x` の後、`id 42` / `id "hello"` / `id true` が全て型チェックを通る
  - `ty_scheme` (forall vars . type) を追加
  - `ty_generalize(t, level)` で level より高い free vars を quantify
  - `ty_instantiate(scheme, level)` で quants を fresh vars に置換
  - `node_let` / `node_letrec` の rhs で level+1, 結果を generalize して env に push
  - `node_letrec_n` (相互再帰) も対応
- **トップレベル binding 型を保存**: `gtype_define(name, scheme)` でグローバル型を記録、`gref` で `gtype_lookup` 経由で instantiate
  - 関数を渡って `compose inc inc` のような型推論が安定化
- **タプルリテラル型**: `node_tuple_n` を `OC_CALL_ARGS` から復元して `TY_TUPLE` を返す
- **`node_let_pat` / `node_match_arm` / `node_letrec_n`**: 各 pattern var に fresh ty_var を割り当てて env に push、body を推論

## Functor (真の実装)

- `module F (X : S) = struct ... end` (sugar) と `module F = functor (X : S) -> struct ... end` 両方をサポート
- `g_functors[]` 表に (name, param, body src 範囲) を保存、`module N = F (M)` 時に:
  - 引数 M の globals (`M.x`) を `<param>.x` でエイリアス
  - module prefix を N に push
  - lex を body 開始位置に巻き戻して `parse_program_until(c, TK_END)` で再パース
  - prefix を pop, lex を元に戻す
- 標準的な `Map.Make` 風パターンが動く (test/33_functor.ml)

## stdlib 追加 (Random / Sys / Format)

- `Random.{int, float, init, self_init}` — シード可能 LCG
- `Sys.{argv, getenv, time, command}` / `exit`
- `Format.{printf, sprintf, eprintf}` (Printf エイリアス; box は省略)
- `succ` / `pred` / `bool_of_string` / `string_of_bool`

## 性能改善

### Inline cache for gref

- `node_gref` / `node_gref_q` に `struct gref_cache *cache @ref` 操作を追加
- `c->globals_serial` を毎 `oc_global_define` で bump
- ホットパス: `cache->serial == c->globals_serial` なら cached value を即返す (2 load + 1 cmp)
- 冷ホットパス: `oc_global_ref` で線形探索後にキャッシュ更新
- **効果**: ベンチ全般で 3-5× 高速化 (fib 3.0s → 0.6s 等)
- 実装には ASTroGen の `@ref` 対応が必要 — `astocaml_gen.rb` を作って `Operand` を継承し、`hash_call` で `@ref` を skip させる (ascheme と同パターン)

### 末尾呼び出し最適化 (TCO)

- パース後の post-pass `mark_tail_calls()` が tail-position の `node_app{0..4}` を `node_tail_app{0..4}` に書き換える (dispatcher を swap、struct layout は同一)
- `node_tail_app_K` は `c->tail_call_pending=1` をセットして dummy 値を返す
- `oc_apply` のループが pending を検出して新しい fn / argv で再エントリー (C スタックは伸びない)
- 1M 段階の再帰、相互再帰、`match` の中の末尾呼び出しなどがすべて定数スタックで動作

### 比較演算の fixnum インラインパス

- `node_lt/le/gt/ge` は冒頭で `OC_IS_INT(a) & OC_IS_INT(b)` を確認、両方 fixnum なら直接 `OC_INT_VAL(av) <op> OC_INT_VAL(bv)` で比較
- それ以外は多相 `oc_compare` にフォールスルー

### コードストア (AOT)

- `--compile` (または `-c`) オプションで各トップレベル式を `astro_cs_compile` → `astro_cs_build` → `astro_cs_reload` → `astro_cs_load`
- `code_store/c/SD_<hash>.c` に specialized C コードを書き出し、`code_store/o/SD_*.o` → `code_store/all.so` を作る
- `dlopen` で `.so` をロードし、各 NODE の dispatcher を SD_<hash> に置換

### ASTroGen の always_inline

- 各 `EVAL_node_*` は `static inline __attribute__((always_inline))` 付きで生成される
- 1 つの dispatcher (`DISPATCH_node_*`) からしか呼ばれないので強制 inline でも icache 圧迫は限定的
- `setjmp` を含む `node_try` は `oc_run_try` ヘルパに切り出し (always_inline と setjmp は両立しない gcc 制約)

### Pattern compiler の単一フレーム化

- `node_match_arm` で arm 内の全変数を 1 つのフレームに集約
- パーサ・ランタイムの depth 整合 (arity == 0 の場合は frame push をスキップ) で機能正しさを担保

### モジュール対応 gref

- `node_gref_q` で qualified / bare の 2 つの名前を runtime に持ち、いずれか先にヒットしたものを返す
- ネスト module 内の bare 参照を `M.x` 経由でも `x` 経由でも解決
