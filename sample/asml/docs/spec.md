# asml 言語仕様

`asml` は **Standard ML サブセット**のインタプリタ。手書きの再帰下降パーサで
ソースを `expr` IR に落とし、Hindley-Milner で型推論したあと、推論結果を
利用して特殊化された ASTro ノードに lower してツリーウォーカで実行する。
完全な SML 仕様は
[The Definition of Standard ML (Revised)](https://smlfamily.github.io/sml97-defn.pdf) を参照。

## 値の種類

| 種別 | 例 | 内部表現 |
|---|---|---|
| `int` | `42` `~3` | 63-bit fixnum (タグ付き) |
| `real` | `3.14` `1.5e2` | boxed double (`MLOBJ_REAL`) |
| `string` | `"hello"` | immutable, len + chars (`MLOBJ_STRING`) |
| `bool` | `true` / `false` | static singleton |
| `unit` | `()` | static singleton |
| `'a list` | `[]` `1 :: t` `[1,2,3]` | nil singleton + cons cells |
| tuple | `(1, "a", 3)` | `MLOBJ_TUPLE` |
| `'a ref` | `ref 0` | `MLOBJ_REF` (1-slot mutable cell) |
| 関数 | `fn x => e`, `fun f x = e` | `MLOBJ_CLOSURE` |
| variant | `NONE` `SOME 42` `Leaf` `Node x` | `MLOBJ_VARIANT` (name + 0/1 args) |
| `exn` | `Match`, `Div`, `Fail "..."` | variant、専用 type tag |

整数は `~` で単項マイナス (SML 流) と `-` で 2 項マイナスを区別する。

## リテラル

```sml
42  ~3  0
3.14  1.5e2  1e~6
"hello\n"
true  false
()
[]  [1, 2, 3]
(1, "a", true)
```

## 束縛

```sml
val x = 10                         (* トップレベル定義 *)
val (a, b) = (1, 2)                (* タプル分解 (top-level のみ) *)
let val y = x + 1 in y * 2 end     (* let .. in .. end ローカル束縛 *)

fun fact n = if n <= 1 then 1 else n * fact (n - 1)
fun even n = if n = 0 then true else odd (n - 1)
and odd  n = if n = 0 then false else even (n - 1)
```

`fun` は暗黙の `val rec`。`and` で同時束縛 (相互再帰可)。`let` 内では `val`
と `fun` が混在できる。

## 関数

```sml
fun add x y = x + y                        (* curried, 2-arg *)
val inc    = fn x => x + 1                  (* fn ラムダ *)
val plus3  = add 3                          (* 部分適用: int -> int *)
val seven  = plus3 4                        (* => 7 *)
```

複数引数はカリー化 (`int -> int -> int` は `int -> (int -> int)`)。
ランタイムは N 引数クロージャ + 部分適用 + over-application を直接サポート
(`ml_apply` 内の `partial_state` センチネル)。

## パターンマッチ

```sml
case x of
    0          => "zero"
  | n          => "non-zero"

case xs of
    []         => 0
  | h :: t     => 1 + length t

case opt of
    NONE       => "none"
  | SOME v     => Int.toString v
```

サポートしているパターン:

- ワイルドカード `_`
- 変数束縛 `x`
- 整数 / 文字列 / `true` / `false` / `()` / `nil` リテラル、`~int`
- `h :: t` (右結合、ネスト可)
- リストリテラル `[p1, p2, ...]`
- タプル `(p1, ..., pN)`
- 0 引数コンストラクタ `Foo`
- 1 引数コンストラクタ `Foo p`

`fn pat => body` と `case` の各 arm、`fun` の引数、`val` の左辺、
`handle` の各 arm でこれらが使える。

## 例外

```sml
1 div 0 handle Div => 0                  (* 0 *)
List.hd [] handle Empty => ~1            (* ~1 *)

fun safe f = f () handle Fail msg => msg
```

組み込み例外: `Match`, `Div`, `Empty`, `Fail of string`。`raise e` で投げる
(`e : exn`)。`e handle pat => h | ...` で受け取り。例外の type は単一の `exn`。

ユーザ定義例外 (`exception E of T`) は **未サポート** (datatype + raise の
組み合わせで代替)。

## 演算子

| カテゴリ | 演算子 | 型 |
|---|---|---|
| 整数算術 | `+ - * div mod` | `int -> int -> int` |
| 整数否定 | `~` | `int -> int` |
| 実数除算 | `/` | `real -> real -> real` |
| 比較 | `< <= > >= = <>` | `'a -> 'a -> bool` (多相) |
| 論理 | `andalso` `orelse` `not` | `bool -> ...` (短絡) |
| 文字列連結 | `^` | `string -> string -> string` |
| リスト | `::` (右結合), `@` (右結合) | `'a -> 'a list -> 'a list`, `'a list -> 'a list -> 'a list` |
| 参照 | `!` (prefix), `:=` | `'a ref -> 'a`, `'a ref -> 'a -> unit` |
| 演算子の関数値化 | `op +` `op =` `op @` 等 | (関数型として束縛取得) |

**SML 本家との差**: `+ - *` は `int` 限定 (SML は `int`/`real` overloaded)。
`real` の四則は `+ -` を使えず、 `op +` 等で `int` プリミティブを取得することは
できる。ベンチに使う数値計算は基本 `int` で書く想定。

## datatype

```sml
datatype 'a option = NONE | SOME of 'a
datatype shape    = Circle of int | Square of int | Triangle
datatype tree     = Leaf | Node of int
```

- 0 引数 / 1 引数のコンストラクタのみ。
- 1 引数の `of` の右辺は `int` / `real` / `string` / `bool` / `unit` /
  `'a` / `T list` / `T ref` / `T1 * T2 * ...` / 別のユーザ datatype。
- 関数型 (`T -> T'`) を `of` の右辺に書くのは未サポート。
- 型変数 (`'a`, `'b`) を datatype 名の前に書ける (`'a option`, `('a, 'b) pair`)。

各コンストラクタは型推論器の constructor 表に scheme を持つ
(`SOME : forall 'a. 'a -> 'a option` 等)。

## 型推論

Hindley-Milner full、Algorithm W、レベルベースの一般化 (Remy)、強い値制約
(value restriction)。型注釈の構文は **未サポート** で、すべて推論する。
型エラーは行番号つきで報告し、`exit 2` で停止する:

```
$ ./asml -e 'val x = 1 + true'
asml: type error at line 1: cannot unify bool with int
```

## トップレベル

ファイルは `; ` で区切られた一連の **トップレベル宣言** から成る:

- `val x = e ;`
- `val (a, b) = e ;` (タプル分解のみ)
- `fun f p1 ... pN = e [and ...] ;`
- `datatype t = Ctor [of T] | ... ;`
- 裸の `e ;` (`val it = e` と等価)

## ビルトインとモジュール風名前

- `print : 'a -> unit`, `println : 'a -> unit`
- `Int.toString : int -> string`, `Real.toString : real -> string`
- `String.size` / `size : string -> int`
- `List.length : 'a list -> int`, `length` (alias)
- `List.null` / `null`, `List.hd` / `hd`, `List.tl` / `tl`, `List.rev` / `rev`
- `real : int -> real`, `floor : real -> int`
- `ref`, `!`, `:=`

`Module.name` 形式の修飾識別子は字句解析時に **単一の TK_ID** として扱う
(本物の SML のモジュールシステムは持たない)。

## Record

```sml
val p = {x = 10, y = 20}                  (* リテラル *)
val px = #x p                              (* フィールド選択 *)
fun add {x, y} = x + y                     (* パターン (短縮形) *)
fun first {x = a, y = _} = a               (* パターン (明示) *)

datatype shape = Pt of {x : int, y : int} (* datatype 内 record 型 *)
val s = Pt {x = 5, y = 6}
val n = case s of Pt {x, y} => x * 100 + y
```

特徴:
- フィールドは登場順序にかかわらず**アルファベット順**で比較・unify
- 型推論は **行多相 (row polymorphism) なし**: `#x e` の使用時点で `e` の
  型が record 型に確定している必要がある (型注釈構文がないため、function
  引数で `fun f {x, y} = ...` のようにパターンで導入するのが定石)
- `=` / `<>` は構造比較 (フィールド順比較)

## 未サポートの主要機能

- signature / structure / functor (モジュールシステム)
- 型注釈の構文 (`(e : T)`, `fun f (x : int) = ...`)
- `local in end`, `where`, `withtype`, `abstype`
- ユーザ定義中置演算子 (`infix`, `infixr`)
- 文字リテラル (`#"a"`)
- substring / slice 操作
- ユーザ定義 exception 宣言
- record の row polymorphism (`fun area p = #x p * #y p` は曖昧で reject される)
