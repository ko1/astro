# AnPy 言語仕様（実装範囲）

AnPy は [ChocoPy](https://chocopy.org/) v2.2（静的型付き Python 3.6 サブセット）の
実装。本書は実装したセマンティクスをまとめる。実装済み/未実装は
[done.md](done.md) / [todo.md](todo.md)。

## プログラム構造

`program ::= [vardef | funcdef | classdef]* stmt*` — トップレベルは**定義が先、文が後**。
関数本体も `[global/nonlocal/vardef/funcdef]* stmt+`。Python より厳格。

## 型（§2.4）

- 基本: `int`, `bool`, `str`, `object`（`int`/`bool`/`str` は `object` のサブクラス）
- リスト: 任意の型 `T` に対し `[T]`。リスト型同士は等しい場合のみ関連（`[int]` と
  `[object]` は無関係）。`[T] <= object`。
- 特殊: `<None>`（`None` の型）, `<Empty>`（`[]` の型）。書けない型。
- ユーザクラス: 単一継承の木（根は `object`）。

関係:
- **適合 `<=`**: 反射的・推移的。`C <= P` は `C` が `P` のサブクラス。`[T]/<None>/<Empty> <= object`。
- **代入互換 `<=a`**: `T1<=T2`、または `T1=<None>` かつ `T2∉{int,bool,str}`、または
  `T2=[T]` かつ `T1=<Empty>`、または `T2=[T]` かつ `T1=[<None>]` で `<None><=a T`。
- **join `⊔`**: `<=a` 順序での最小上界。

## 値（§2.5）

- `int`: 符号付き 32bit。オーバーフローは未定義動作（実装は機械語幅で計算; 範囲内なら Python と一致）。
- `bool`: `True`/`False`。
- `str`: 不変。`len(s)` / `s[i]`（長さ1の新 str）/ `s1+s2`。
- list: 可変・固定長。`len` / `l[i]` / `l1+l2`（要素型 `T1⊔T2`）/ `l[i]=e`。
- オブジェクト: 参照。`is` で同一性。`None` は `object`/任意クラス/任意リスト型に代入可。

## 式（§2.6）と優先順位（§4.1, 低→高）

`if-else`(右) < `or` < `and` < `not` < 比較（`== != < <= > >=` と `is`、**非結合**）
< `+ -` < `* // %` < 単項 `-` < `. []`。

- 算術 `+ - * // %` は `int`×`int`→`int`（`//`,`%` は Python の床除算・床剰余）。`+` は
  `str+str`/`list+list` も可。
- 比較 `< <= > >=` は `int`、`== !=` は同型の `int`/`bool`/`str`。`is` は `int`/`bool`/`str`
  以外。論理は `bool`、短絡評価。
- `e1 if e0 else e2`、リスト表示 `[e,...]`、添字 `e[i]`、属性 `e.id`、呼び出し `f(...)`、
  メソッド `e.m(...)`（動的ディスパッチ）。

## 文（§2.8）

式文（値は捨てる）、`if/elif/else`、`while`、`for id in expr`（list/str を走査; ループ変数は
事前に宣言）、`pass`、`return [expr]`、複数代入 `t1 = t2 = ... = e`（右辺を1回評価し左から代入）。

## 組み込み（§2.8.6）

`print(object)->None`（引数は `int`/`bool`/`str` のみ; それ以外は実行時エラー）、
`input()->str`、`len(object)->int`（`str`/list のみ）。

## 実行時エラー（§6.4）

`print`/`len` の不正引数、ゼロ除算、添字範囲外、`None` への操作（メソッド/属性/list 操作）、
メモリ不足。発生時はメッセージを出して当該実行を中断する。
