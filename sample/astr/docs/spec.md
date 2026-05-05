# astr 言語仕様

`astr` は **R のサブセット**インタプリタ。R は統計計算用の動的型付き言語で、
**ベクタが第一級** (スカラもすべて長さ 1 のベクタとして扱う) という特徴を
持つ。astr は GNU R の中核機能のうち、教育・ベンチマーク用途で頻出する
範囲を実装する。

完全な R 仕様は [R Language Definition](https://cran.r-project.org/doc/manuals/r-release/R-lang.html)
を参照。本書は astr で動く範囲を端的に示す。

## 値の種類

| 種別 | 例 | 備考 |
|---|---|---|
| 整数 (fixnum) | `42L` `-3` | 63-bit 即値 (`L` サフィックスは構文受理だが内部的に同じ) |
| 浮動小数 | `3.14` `1e10` | IEEE-754 double (heap) |
| 真偽 | `TRUE` `FALSE` | 内部的に整数 1 / 0 |
| 文字列 | `"hello"` `'world'` | |
| 数値ベクタ | `c(1, 2, 3)` `1:10` | 同型要素の固定長配列 |
| 整数ベクタ | (整数のみの c() 結果) | |
| 文字列ベクタ | `c("a", "b", "c")` | |
| リスト (list) | `list(1, "a", c(1, 2))` | 異種要素可 |
| `NA` | `NA` | 欠損値 |
| `NULL` | `NULL` | 空値 |

R では純粋なスカラは無く、`x <- 1` の `1` も実際は長さ 1 のベクタ。astr
ではパフォーマンスのため fixnum はベクタ化せず保持し、ベクタ操作要求時に
昇格する。

## リテラル

```r
42L             # 整数
3.14   1e10     # 浮動小数 (numeric)
TRUE   FALSE    # 真偽
"hello"  'hi'   # 文字列 (どちらの引用符も可)
NA              # 欠損値
NULL            # 空値
```

## 代入

`<-` と `=` のどちらも代入として動く (R の慣習では `<-` を推奨):

```r
x <- 1
y = 2
```

ベクタ要素への代入は `v[i] <- value` で:

```r
v <- c(1, 2, 3)
v[2] <- 100
print(v)        # 1 100 3
```

## 演算子

| カテゴリ | 演算子 |
|---|---|
| 算術 | `+ - * / %% %/% ^` (`%%` は剰余、`%/%` は整数除算、`^` は冪) |
| 比較 | `< > <= >= == !=` |
| 論理 | `& | !` (要素ごと)、`&& ||` (短絡; スカラ向け) |
| 範囲 | `1:10` (1..10 の整数ベクタ) |
| 添字 | `v[i]` |

ベクタ演算は要素ごとに自動適用 (broadcasting):

```r
c(1, 2, 3) + 10           # c(11, 12, 13)
c(1, 2, 3) * c(4, 5, 6)   # c(4, 10, 18)
```

## 関数定義

```r
fib <- function(n) {
  if (n < 2) n
  else       fib(n - 1) + fib(n - 2)
}
fib(20)    # 6765
```

- `function(args) body` で関数オブジェクトを作り、`<-` で名前に束縛する。
- 関数本体の最終式が戻り値 (`return(x)` も使える)。
- 仮引数のデフォルト値: `function(n = 10) n * 2`。
- `...` (可変長引数) は astr では構文受理のみ — 全機能サポートは未対応。

## 制御構造

### `if / else`

```r
if (cond) expr1 else expr2
if (cond) expr1                # else 省略時は NULL
```

`if` も式 — 値を返す。

### `while`

```r
while (cond) {
  body
}
```

### `for`

```r
for (i in 1:n) {
  print(i)
}

for (v in c(1, 2, 3)) { ... }
```

R の `for` は **イテラブル (ベクタ・リスト) を順に取り出す**。`1:n` は
整数ベクタ `c(1, 2, ..., n)` の糖衣表現。

### `break` / `next`

`break` でループから抜ける、`next` で次のイテレーションへ (他言語の `continue`)。

## ベクタ操作

### 生成

```r
c(1, 2, 3)                       # 数値ベクタ
c("a", "b", "c")                 # 文字列ベクタ
1:10                             # 1, 2, ..., 10
```

### アクセス

```r
v <- c(10, 20, 30, 40)
v[1]                  # 10  (R は 1-origin)
v[4]                  # 40
length(v)             # 4
```

(`v[1:3]`、`v[v > 0]` 等の複数要素 / 論理添字は未対応。)

### 主な組み込み関数

```r
length(v)        # 長さ
sum(v)           # 合計
c(a, b, c)       # 連結 (concatenate)
paste(a, b)      # 文字列を空白区切りで連結
paste0(a, b)     # 区切りなし連結
nchar(s)         # 文字列の長さ
substr(s, i, j)  # 部分文字列 (1-origin、両端含む)
```

## 数学関数

```r
floor(x)  ceiling(x)  round(x)
sqrt(x)   abs(x)
log(x)    exp(x)
sin(x)    cos(x)    tan(x)
```

## 型変換・判定

```r
as.integer(x)    # 整数化
as.numeric(x)    # 浮動小数化
is.numeric(x)    # 数値か?
is.character(x)  # 文字列か?
```

## 出力

```r
print(x)         # 値を「[1] ...」スタイルで表示 (R 慣習)
cat("a", "b")    # 値をそのまま連結出力 (改行なし)
```

## 例

```r
# フィボナッチ
fib <- function(n) {
  if (n < 2) n
  else       fib(n - 1) + fib(n - 2)
}
print(fib(30))           # [1] 832040

# ベクタの和 (要素ごと演算 → sum で集計)
v <- 1:100
print(sum(v))            # [1] 5050

# 平均
mean_x <- function(v) sum(v) / length(v)
print(mean_x(c(2, 4, 6, 8)))   # [1] 5
```

## 持たない / 制限

- レキシカルスコープ (トップレベル以外のクロージャ捕獲は限定的)
- `apply` / `sapply` / `lapply` / `mapply` の関数族
- 整数 (`L`) と double の厳密な型区別 (内部的には fixnum 統一)
- 複数要素の添字 (`v[1:3]`、論理添字 `v[v > 0]`)
- リスト要素アクセス `v[[i]]` `v$name`
- `tryCatch` / シグナル処理
- 正規表現 (`grepl` / `gsub` 等)
- データフレーム、S3 / S4 / R6 オブジェクトシステム
- 環境オブジェクト (`environment` / `globalenv`)

詳細: [`done.md`](done.md) / [`todo.md`](todo.md)。
