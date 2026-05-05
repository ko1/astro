# astocaml 言語仕様

`astocaml` は **OCaml サブセット**のインタプリタ。OCaml は静的型付きの
関数型言語で、(1) パターンマッチ、(2) 代数的データ型 (バリアント・タプル・
レコード)、(3) 静的型推論 (Hindley-Milner)、(4) ファースト・クラス関数 +
クロージャ、(5) 命令的拡張 (`ref` / 例外 / 配列) を持つ。

astocaml は **HM-lite の型推論** (互換性のためのチェックのみ; ランタイムは
動的) と OCaml 構文の主要部分を実装。`ocaml` toplevel / `ocamlc` バイトコードに
近いセマンティクスで動く。完全な OCaml 仕様は
[OCaml Manual](https://v2.ocaml.org/manual/) を参照。

## 値の種類

| 種別 | 例 | 備考 |
|---|---|---|
| 整数 (`int`) | `42` `-3` | 62-bit fixnum (タグ付け済み) |
| 浮動小数 (`float`) | `3.14` | IEEE-754 double (heap) |
| 文字 (`char`) | `'a'` | |
| 文字列 (`string`) | `"hello"` | immutable |
| バイト列 (`bytes`) | `Bytes.of_string "hi"` | mutable |
| ユニット (`unit`) | `()` | 値が 1 つしかない型 |
| 真偽 (`bool`) | `true` `false` | |
| リスト (`'a list`) | `[1; 2; 3]` `1 :: tail` | immutable |
| タプル | `(1, "ab", 3.0)` | 任意長 |
| レコード | `{ x = 1; y = 2 }` | 名前付きフィールド |
| バリアント | `Foo` `Bar 3` | 代数的データ型 |
| 多相バリアント | `` `Foo `` `` `Bar 3 `` | 事前宣言不要 |
| `'a option` | `None` `Some 42` | |
| `'a ref` | `ref 0` | mutable セル |
| `'a array` | `[\| 1; 2; 3 \|]` | mutable 固定長 |
| `'a Lazy.t` | `lazy expr` | 遅延評価 |
| 関数 (`'a -> 'b`) | `fun x -> x + 1` | 第一級 |
| オブジェクト | `object val x = 0 method m = ... end` | |

`'a` などの型変数は「任意の型」を意味する。

## リテラル

```ocaml
42  -3  0xff  0b1010
3.14  1e10  0.1e-3
'a'  '\n'  '\\'
"hello\n"
true  false
()
```

## 束縛 (let)

```ocaml
let x = 10                        (* トップレベル定義 *)
let y = x + 1 in y * 2            (* let .. in .. — ローカル束縛 *)

let rec fact n = if n = 0 then 1 else n * fact (n - 1)   (* 再帰 *)

let rec even n = n = 0 || odd (n - 1)
and odd n = n = 1 || even (n - 1)                         (* 相互再帰 *)
```

`let rec` は再帰許可。`let .. and ..` で同時束縛 (相互再帰可)。

## 関数

```ocaml
let add x y = x + y                       (* 多引数 *)
let add = fun x y -> x + y                (* 無名関数 *)
let id = fun x -> x

let inc = add 1                            (* 部分適用: int -> int *)
inc 5                                       (* => 6 *)
```

OCaml の関数はすべて 1 引数で、複数引数はカリー化されている (`int -> int -> int` は実際は `int -> (int -> int)`)。

`function` は引数 1 個 + パターンマッチの糖衣:

```ocaml
let classify = function
  | 0 -> "zero"
  | n when n > 0 -> "positive"
  | _ -> "negative"
```

## 演算子

| カテゴリ | 演算子 |
|---|---|
| 整数算術 | `+ - * / mod` |
| 浮動小数算術 | `+. -. *. /.` (型ごとに別演算子) |
| 比較 | `< > <= >= = <> == !=` (`=` は構造的等価、`==` は物理同一) |
| 論理 | `&& \|\| not` |
| 文字列 | `^` (連結) |
| リスト | `::` (cons) |
| 参照 | `:= ! ` (代入 / 参照) |
| レコード | `r.field <- v` (mutable フィールド書込) |
| 配列 | `a.(i)` `a.(i) <- v` |

整数と浮動小数で**演算子が分かれている**のは OCaml の特徴 (暗黙の数値変換が無い):

```ocaml
1 + 2          (* int *)
1.0 +. 2.0     (* float *)
1 + 2.0        (* 型エラー *)
```

すべての演算子は関数値としても使える: `(+) 1 2 = 3`、`List.fold_left (+) 0 [1;2;3] = 6`。

## 制御構文

### `if / then / else`

```ocaml
if cond then e1 else e2
if cond then e1                  (* else 省略時は () が要求される (e1 : unit) *)
```

### 順次実行

```ocaml
e1; e2          (* e1 を捨てて e2 *)
begin e1; e2 end
```

### `try / with` (例外ハンドリング)

```ocaml
try
  do_something ()
with
  | Not_found -> 0
  | Failure msg -> -1
```

`raise e` で例外を投げる:

```ocaml
raise (Failure "bad input")
failwith "bad input"           (* raise (Failure "bad input") の糖衣 *)
```

組み込み例外: `Failure` / `Not_found` / `Invalid_argument` / `Division_by_zero` /
`Match_failure` / `Assert_failure` / `Exit`。

ユーザ例外:

```ocaml
exception My_error of string
raise (My_error "oops")
```

### `lazy` / `Lazy.force`

```ocaml
let p = lazy (expensive ())
Lazy.force p              (* 1 回だけ evaluate して結果を memoize *)
```

## パターンマッチ

OCaml の中核機構。`match expr with` の右辺で値を分解しつつ分岐:

```ocaml
match lst with
| [] -> 0
| [x] -> x
| x :: y :: _ -> x + y
| _ -> -1
```

サポートするパターン:

| パターン | 例 |
|---|---|
| リテラル | `0` `"abc"` |
| ワイルドカード | `_` |
| 変数束縛 | `x` |
| タプル | `(a, b)` |
| リスト | `[]` `x :: xs` `[a; b; c]` |
| バリアント | `Some x` `None` `Foo` `Bar (a, b)` |
| 多相バリアント | `` `Foo `` `` `Bar n `` |
| レコード | `{ x; y }` `{ x = 1; _ }` |
| `as` (別名) | `(x, y) as p` |
| or-pattern | `1 \| 2 \| 3` |
| ガード | `n when n > 0 -> ...` |
| 例外 | `match e with exception E -> ...` |

完備性チェックはランタイムの `Match_failure` で対応 (静的検査は弱い)。

## 代数的データ型

### バリアント (sum type)

```ocaml
type shape =
  | Circle of float
  | Rect of float * float
  | Triangle of { a : float; b : float; c : float }     (* インライン record *)

let area = function
  | Circle r -> 3.14 *. r *. r
  | Rect (w, h) -> w *. h
  | Triangle { a; b; c } -> ...
```

### レコード (product type)

```ocaml
type point = { mutable x : int; y : int }

let p = { x = 1; y = 2 }
let p2 = { p with x = 10 }       (* 関数的更新 *)
p.x                              (* => 1 *)
p.x <- 5                         (* mutable のみ書込可 *)
```

### 多相バリアント (タグ付きユニオン、宣言不要)

```ocaml
let red = `Red                   (* 事前 type 不要 *)
let make_color = function
  | `Red -> 0xff0000
  | `Green -> 0x00ff00
  | `Blue n -> n
```

## 末尾呼出最適化 (TCO)

末尾位置の関数呼出は **C スタックを伸ばさない**。`if` の枝、`match` の右辺、
`let .. in` の本体、`begin .. end` の最終式などはすべて末尾位置。

```ocaml
let rec count n = if n = 0 then () else count (n - 1)
let () = count 1_000_000     (* オーバーフローしない *)
```

## モジュール

```ocaml
module Math = struct
  let pi = 3.14159
  let square x = x *. x
end

Math.square 2.0              (* 4.0 *)
open Math; pi                (* スコープに pi を取り込む *)
```

`module type S = sig ... end` でシグネチャ宣言、`module M : S = struct ... end`
で型注釈付き定義、`module M = functor (X : S) -> struct ... end` で
ファンクタ — 構文は受理するが、ファンクタの実体化は未完。

## クラス・オブジェクト (簡易)

```ocaml
class point ax ay = object
  val mutable x = ax
  val mutable y = ay
  method get_x = x
  method set_x v = x <- v
  method shift dx dy = x <- x + dx; y <- y + dy
end

let p = new point 1 2
p#shift 10 20
p#get_x                          (* => 11 *)
```

`val [mutable] f = ...` でフィールド、`method m args = ...` でメソッド。
`obj#method` でメッセージ送信、`self#field` でフィールド参照
(メソッド内で `field` 直接参照は未対応)。継承 (`inherit`) は未対応。

## 標準ライブラリ (抜粋)

I/O: `print_int` / `print_string` / `print_endline` / `print_char` /
`print_float` / `Printf.printf` / `Printf.sprintf` / `Printf.eprintf`

変換: `string_of_int` / `int_of_string` / `string_of_float` /
`float_of_int` / `int_of_float`

リスト: `List.{length, hd, tl, rev, append, map, filter, fold_left, fold_right, iter, nth, mem}`

文字列・配列・bytes: `String.{length, get, sub, concat}` /
`Array.{make, length, get, set}` / `Bytes.{create, make, length, get, set, to_string, of_string}`

数学: `sqrt` / `sin` / `cos` / `log` / `exp` / `floor` / `ceil`

その他: `compare` / `min` / `max` / `abs` / `ref` / `!` / `:=` /
`Lazy.force` / `Lazy.is_val` / `failwith` / `invalid_arg` / `assert`

## 例

```ocaml
let rec fact n = if n = 0 then 1 else n * fact (n - 1)
let () = print_int (fact 10); print_newline ()    (* => 3628800 *)

(* リストの和、TCO *)
let rec sum acc = function
  | [] -> acc
  | x :: xs -> sum (acc + x) xs
let () = print_int (sum 0 [1; 2; 3; 4; 5])        (* => 15 *)

(* バリアント + パターンマッチ *)
type tree = Leaf | Node of tree * int * tree
let rec insert v = function
  | Leaf -> Node (Leaf, v, Leaf)
  | Node (l, x, r) when v < x -> Node (insert v l, x, r)
  | Node (l, x, r) -> Node (l, x, insert v r)
```

## 持たない / 制限

- カスタム多文字中置演算子 (`(+!)` など)。`+ - * / mod` 等の標準セットは可
- Range パターン (`'a' .. 'z'`)
- クラス継承 (`inherit`) / `initializer` / virtual method
- ファンクタの実体化 (構文受理のみ)
- First-class module / GADT (構文受理のみ)
- Effect handlers
- ppx 拡張

詳細: [`done.md`](done.md) / [`todo.md`](todo.md)。
