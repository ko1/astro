# astocaml

ASTro framework 上に構築した OCaml サブセット・インタプリタ。

## 概要

`astocaml` は OCaml の中核機能を網羅したツリーウォーク・インタプリタ。
ASTroGen が生成する dispatcher 経由で `EVAL_node_*` を呼び出す形をとり、`code_store` を経由した AOT/JIT 用パスも将来的に有効化できる
(現状は `OPTIMIZE` で `astro_cs_load` を呼ぶだけのコールド・パスのみ)。

## 現状サマリ

OCaml の Phase 1 + Phase 2 + Phase 3 の主要機能を一通り実装済み:

- 基本型: `int` / `bool` / `unit` / `string` / `char` / `float`
- 拡張型: `'a list` / `'a option` / `('a, 'b)` タプル / レコード / バリアント / `'a ref` / `'a array` / `'a Lazy.t` / `bytes` / オブジェクト / 関数 (`'a -> 'b`)
- 演算子: `+ - * / mod`, `+. -. *. /.`, `< > <= >= = <> == !=`, `&& || not`, `::`, `^`, `:=` / `!`, `<-`, `.( )`
- 制御構文: `if / then / else`, `;`, `begin .. end`, `try / with / raise`, `lazy ... / Lazy.force ...`
- パターンマッチ: 完全再帰的パターン (リテラル / `_` / 変数 / `[]` / `h::t` / タプル / バリアント / レコード / `as` / or-pattern / 多相バリアント), `when` ガード, `match e with | exception E -> ...`
- 束縛: `let`, `let rec ... and ...` (トップレベル / ネスト両方で相互再帰可), `let .. in ..`, パターン束縛
- 関数: `fun x y -> body`, `fun () -> body`, `function | p1 -> e1 | ...`, ラベル `~x` / オプション `?x` (位置引数として簡略化), 多引数クロージャ + 部分適用 + 過適用
- 演算子の関数値: `(+) (-) (*) (/) mod (+.) (-.) (*.) (/.) (<) (>) (<=) (>=) (=) (<>) (^) (::)`
- 例外: `try ... with`, `raise`, 既定 `Failure` / `Not_found` / `Invalid_argument` / `Division_by_zero` / `Match_failure` / `Assert_failure` / `Exit`, ユーザ定義 `exception E of T`
- バリアント: `type t = Foo | Bar of int | Baz of int * string`, 多相バリアント `\`Foo` / `\`Bar n`
- レコード: `type p = { x : int; y : int }`, `{ x = 1; y = 2 }`, `r.x`, `{ r with x = ... }`, パンニング `{ x }`, パターンマッチ
- モジュール: `module M = struct ... end` (ネスト可), `open M`, `module M = N`, `module type S = sig ... end` / `module M : S = ...` / `module M = functor (X : S) -> ...` (構文受理)
- クラス・オブジェクト: `class NAME params = object val [mutable] f = ...; method m args = ... end`, `new NAME args`, `obj#method args`, `self#field` で field 読取り, `self#set_field v` で書込み
- 末尾呼び出し最適化 (TCO) — 1M 段階の再帰、相互再帰、`match` 内の末尾呼び出しが定数スタックで動作
- AOT 特殊化: `--compile` で `astro_cs_compile` → `.so` ロードを起動
- ビルトイン:
  - I/O: `print_int` / `print_string` / `print_endline` / `print_newline` / `print_char` / `print_float` / `Printf.printf` / `Printf.sprintf` / `Printf.eprintf`
  - 変換: `string_of_int` / `int_of_string` / `string_of_float` / `float_of_int` / `int_of_float` / `float_of_string`
  - 例外系: `failwith` / `invalid_arg` / `raise` / `assert`
  - 比較: `compare` / `min` / `max` / `abs`
  - List: `length` / `hd` / `tl` / `List.{length,hd,tl,rev,append,map,filter,fold_left,fold_right,iter,nth,mem}`
  - String: `String.{length,get,sub,concat}`
  - Bytes: `Bytes.{create,make,length,get,set,to_string,of_string}`
  - Array: `Array.{make,length,get,set}`, リテラル `[| 1; 2; 3 |]`, インデックス `a.(i)`
  - Char: `Char.code` / `.chr`
  - 数学: `sqrt` / `sin` / `cos` / `log` / `exp` / `floor` / `ceil`
  - 参照: `ref`
  - Lazy: `Lazy.force` / `Lazy.is_val`

未対応 (詳細は [`docs/todo.md`](docs/todo.md)):

- 多文字カスタム中置演算子 (`(+!) a b = ...` などの `+!` のレキサ対応)
- Range pattern (`'a' .. 'z'`)
- メソッド body 内での bare field 参照 (`self#field` 経由のみ; 直接の `field` は global lookup される)
- functor の本格的な実体化 (現状は構文受理のみ)
- クラス継承 (`inherit`), `initializer`, virtual method
- First-class module / GADT (構文受理のみ)
- Effect handlers
- ppx 拡張

## ビルド・実行

```sh
make             # interpreter ビルド
make test        # test/*.ml を *.expected と比較
make bench       # bench/*.ml を計測
```

REPL:

```sh
./astocaml
```

スクリプト:

```sh
./astocaml -q test/04_recursion.ml
```

## ファイル構成

| ファイル | 役割 |
|--|--|
| `node.def` | AST ノード定義 (~50 種) と `EVAL` ロジック (ASTroGen 入力) |
| `context.h` | `VALUE` 表現, `CTX`, `oobj` (cons/string/closure/tuple/ref/float/variant/record/array), 例外ハンドラスタック |
| `node.h` | NodeHead 宣言, `EVAL` インライン, `HOPT/HORG` |
| `node.c` | ランタイム allocator + `INIT` (生成ファイルを include) |
| `main.c` | レキサ + 再帰下降パーサ, ランタイム (apply / env / globals / display / pattern compiler), builtins, REPL |
| `Makefile` | astocaml ビルド + `make test` + `make bench` |
| `docs/todo.md` | 未実装機能一覧と将来課題 |
| `docs/done.md` | 実装済み機能一覧 |
| `docs/runtime.md` | 実装の解説 (関数まわり中心) |
| `docs/perf.md` | 性能改善ログ (成功/失敗) |
| `test/` | 23 のテスト (ML + .expected) |
| `bench/` | 6 のベンチマーク (ack/fib/nqueens/sieve/tak + method_call) |

## 値表現

```
VALUE = int64_t
  bit 0 = 1         → fixnum (62-bit signed integer)
  bit 0 = 0         → 8-byte aligned heap pointer or singleton
  singletons        → unit / true / false / nil  (静的に確保)
  heap objects      → cons / string / closure / prim / tuple / ref / float / variant / record / array
```

整数は immediate (タグ付き) で、その他は全てヒープ確保 (`malloc`、終了まで保持)。

## 型チェック

OCaml と同様の (簡易な) Hindley-Milner 型推論をパース後・評価前に走らせ、
型エラーを早期に検出する。デフォルトで有効。

```ocaml
let _ = "a" + 1
```

```
$ ./astocaml prog.ml
astocaml: type error: (+): left operand must be int, got string
astocaml: Type_error((+): expected int)
```

無効化:

```sh
./astocaml --no-check prog.ml    # static check のみ off; runtime check は残る
```

カバー範囲:

- 算術 (`+ - * / mod` で int を要求, `+. -. *. /.` で float)
- 論理 (`&& || not` で bool)
- 比較 (`< > <= >= = <>` の両辺の型は一致)
- 文字列連結 (`^` で string)
- リスト (`::`, `[]`, `hd`, `tl`)
- 参照 (`ref`, `!`, `:=`)
- `if cond` で bool, `then` と `else` の型一致
- `let / let rec`、関数適用 (`f arg` で f は関数, arg と引数型一致)
- `try` の body と handler の型一致

未カバー (`TY_ANY` で素通り、ランタイムでチェック):

- グローバル参照 (`gref`) — global の型情報を持っていないため
- バリアント / レコード / オブジェクト / モジュール
- タプル の構造的検査 (`(a, b)` を作るときは検査せずに作る)
- パターンマッチの arm
- let-polymorphism (`let id = fun x -> x` した後の `id 1` と `id "a"` の両立)

ランタイムでも arith / bool / string / if 条件などは型チェックされ、
失敗時は `Type_error` 例外を発生させる (`try / with Type_error _ -> ...` で捕捉可)。

## テスト

`test/` 内 35 ファイル:

| ファイル | テスト範囲 |
|--|--|
| 00_arithmetic | 整数演算と優先順位 |
| 01_booleans | 真偽値、比較、短絡 |
| 02_let | `let / let-in / shadowing` |
| 03_functions | 関数定義 / 多引数 / 無名関数 |
| 04_recursion | fib / fact / gcd / power / ack 等 |
| 05_lists | リスト構築・走査 |
| 06_higher_order | map / fold_left / fold_right / filter / compose |
| 07_closures | クロージャの環境キャプチャ |
| 08_match | パターンマッチの種類 |
| 09_strings | 文字列、結合、`string_of_int` |
| 10_classics | FizzBuzz, 素数, Tower of Hanoi |
| 11_sorting | 挿入ソート, クイックソート |
| 12_tail_rec | 末尾再帰 (TCO は無いが 10000 回程度なら動作) |
| 13_99problems | 99 OCaml problems の移植 |
| 14_tuples | タプル、destructuring、ネストタプル |
| 15_refs | `ref / ! / :=` と imperative pattern (counter) |
| 16_floats | 浮動小数点、`+. -. *. /.`、math 関数、Leibniz |
| 17_mutual_rec | トップレベル `and` 相互再帰、`function` キーワード |
| 18_match_full | ネストパターン、`when` ガード、`as` パターン |
| 19_exceptions | `try / with / raise / failwith / assert / invalid_arg / Division_by_zero` |
| 20_variants | option, tree, either。バリアントマッチ |
| 21_records | レコードリテラル、`r.f`, `{r with ...}`, レコードパターン、punning |
| 22_stdlib | List.*, String.*, Array.*, compare/min/max/abs |
| 23_modules | module / nested module / open / Lazy / Printf / Bytes |
| 24_classes | class / object / method / new / `obj#m` / mutable val |
| 25_tco | 1M 段階の末尾再帰、相互再帰、match 内の tail 位置 |
| 26_misc | or-pattern, match-exception, infix def, polymorphic variant, `==`, `.5` float |
| 27_inheritance | クラス継承 (`inherit`) と `initializer` |
| 28_containers | `Hashtbl` / `Stack` / `Queue` / `Buffer` |
| 29_list_more | `List.sort/.assoc/.partition/.combine/.split/.find/.exists/.for_all/.flatten/.init/.iter2/.map2` |
| 30_type_check | 型チェックを通る正しいプログラム集 |
| 31_type_errors | 型エラーが静的・動的に検出されるかの回帰テスト |
| 32_let_poly | let-polymorphism (`let id x = x` を `int / string / bool` で同時利用) |
| 33_functor | functor (`module Make (X : ORD) = struct ... end` + `module IntM = Make (IntOrd)`) |
| 34_more_stdlib | Random / Sys / Format / succ/pred / bool conversions |

## ベンチマーク

`bench/` 内 6 ファイル (持続実行で約 1 秒以上のスケール):

| ベンチ | 入力 | 結果 | 時間 | 概要 |
|--|--|--|--|--|
| ack | ack(3, 9) | 4093 | 0.62 s | Ackermann; 深い再帰 |
| fib | fib(35) | 9227465 | 0.57 s | 古典 Fibonacci |
| nqueens | 10-queens × 3 | 724 | 1.12 s | バックトラッキング (相互再帰) |
| sieve | sum of primes ≤ 8000 × 8 | 3738566 | 0.97 s | エラトステネス |
| tak | tak(24, 16, 8) × 5 | 9 | 0.46 s | Takeuchi 関数 |
| method_call | counter#incr 10M | 10000000 | 0.99 s | OO 重い method send |

主な高速化要因:
- **gref インラインキャッシュ** (全ベンチ 3-5× 加速)
- **closure leaf alloca** (frame の malloc を排除; fib で更に 2.7× 加速)
- **method send IC** (`obj#m` の dispatch をキャッシュ; method-call で 1.3-1.6× 加速)
- **AOT specialize** (`-c` で .so にコンパイル、各 NODE の dispatcher を SD_* に patch; ack/tak 3×, fib 2× 加速)
- **`node_appN` closure-leaf fast path** — `oc_apply` の type chain と partial-app 確認を caller 側で in-line skip; fib(40) で更に 1.9× 加速
- TCO トランポリン、fixnum 比較 fast-path、ASTroGen always_inline

`./astocaml -c` (AOT 込み):

| ベンチ | 時間 |
|--|--|
| ack | 0.15 s |
| fib | 0.19 s |
| nqueens | 0.83 s |
| sieve | 0.90 s |
| tak | 0.09 s |

公式 OCaml 4.14.1 との比較:

| bench | astoc | astoc(-c) | ocaml(top) | ocamlc(BC) | ocamlopt |
|--|--|--|--|--|--|
| ack | 0.40 | **0.15** | 0.22 | 0.13 | 0.012 |
| fib | 0.51 | **0.19** | 0.26 | **0.21** | 0.043 |
| nqueens | 1.14 | 0.83 | 0.22 | 0.18 | 0.019 |
| sieve | 0.96 | 0.90 | 0.27 | 0.25 | 0.025 |
| tak | 0.31 | **0.09** | 0.23 | **0.17** | 0.016 |

`fib(40)` で per-call: **astocaml -c 6.4 ns**, ocamlc 7.5 ns, ocamlopt 1.4 ns。

fib / tak で **astocaml AOT が ocamlc bytecode を上回る**。ack も BC とほぼ同等。nqueens / sieve は list 処理が AOT 最適化対象外なのでまだ 3-4× の差。
ocamlopt (native compilation) との差は 4-9× まで縮まったが、依然として真の native code 生成との差は残る。

メモリ使用量はインタプリタの malloc-leak 戦略によるもの (将来 Boehm GC で解消予定)。

## 出典

- `test/13_99problems.ml` は 99 OCaml Problems
  ([ocaml.org/exercises](https://ocaml.org/exercises)) の解答群を本サブセットに
  書き直したもの。
- ベンチマークは Computer Language Benchmarks Game 系の古典問題
  (fib / tak / ack / nqueens / sieve) に基づく。
