# ascheme_precise 言語仕様

`ascheme_precise` は **R5RS Scheme** のサブセット。 R5RS は Scheme の標準
(1998) で、 数値タワー / 末尾呼出 / 第一級継続 / 多値 / 高階関数 / マクロ
を含む小ぶりな Lisp。 ascheme_precise は chibi-scheme の `r5rs-tests.scm`
179 件を 100% パスする範囲を実装している (= default GC mode)。

R5RS 仕様自体は短く読める ([R5RS](https://schemers.org/Documents/Standards/R5RS/))。
本書では「ascheme で何が動くか」を端的に示す。

注: `sample/ascheme_precise` は `sample/ascheme` (= libgc 版) を fork して
GC を precise framework に置き換えた版。 言語仕様 / R5RS 互換性は同じ。
GC まわりの差分は [`runtime.md`](./runtime.md) §7、 perf 評価は
[`perf.md`](./perf.md) を参照。

## 値の種類

数値タワー + 同型データ。

| 種別 | 例 | 備考 |
|---|---|---|
| 整数 (fixnum) | `42` `-3` | 即値、64bit 範囲 |
| 整数 (bignum) | `(expt 2 100)` | GMP で任意精度 |
| 有理数 | `1/2` `3/4` | exact、約分済み |
| 実数 (flonum) | `3.14` `1e10` | IEEE-754 double |
| 複素数 | `3+4i` | 直交 / 極形式 |
| 真偽値 | `#t` `#f` | |
| シンボル | `'foo` | |
| 文字 | `#\a` `#\space` `#\newline` | |
| 文字列 | `"hello"` | mutable |
| ペア / リスト | `(1 2 3)` `(a . b)` | |
| ベクタ | `#(1 2 3)` | mutable、固定長 |
| 手続き | `(lambda (x) ...)` | |
| ポート | (input/output) | open-*-file が返す |
| 継続 | `call/cc` の引数 | first-class |
| 約束 | `(delay expr)` | force で評価 |

`#f` だけが偽。それ以外 (`0` `'()` `""` を含む) は真。

## リテラル

```scheme
42  -3.14  1/3  3+4i           ; 数
'symbol  '(1 2 3)  '#(a b)     ; quote
"hello\n"  #\a  #\newline      ; 文字列・文字
#t  #f  '()                    ; 真偽・空リスト
```

## 基本構文

### 定義

```scheme
(define x 10)
(define (square x) (* x x))      ; (define name expr) または (define (name args...) body...)
```

### `lambda`

```scheme
(lambda (x y) (+ x y))           ; 固定引数
(lambda args ...)                ; 可変長 (args は引数のリスト)
(lambda (x . rest) ...)          ; dotted: x = 第一引数, rest = 残りのリスト
```

### `set!` (代入)

```scheme
(set! x (+ x 1))
```

### 局所束縛

```scheme
(let ((x 1) (y 2)) (+ x y))         ; 並列束縛
(let* ((x 1) (y (+ x 1))) y)        ; 逐次束縛
(letrec ((fact (lambda (n) ...))) ...) ; 相互再帰可能
(let loop ((i 0) (acc 0))           ; 名前付き let — 末尾再帰のループ糖衣
  (if (= i 10) acc (loop (+ i 1) (+ acc i))))
```

### 制御

```scheme
(if cond then else)
(if cond then)                    ; else 省略時は未定義値
(cond ((c1) e1) ((c2) e2) (else e3))
(case key ((1 2) "a") ((3 4) "b") (else "c"))
(when cond e1 e2 ...)             ; cond が真なら順次実行
(unless cond e1 e2 ...)
(and e1 e2 ...)                   ; 短絡; 最後の値か #f
(or  e1 e2 ...)                   ; 短絡; 最初の真値か #f
(begin e1 e2 ...)                 ; 順次評価
```

### `do` ループ

```scheme
(do ((i 0 (+ i 1)) (s 0 (+ s i)))   ; (var init step)
    ((= i 10) s))                    ; (test result)
```

## 末尾呼出 (TCO)

R5RS 必須。**末尾位置の関数呼出は C スタックを伸ばさない**。たとえば:

```scheme
(define (loop n) (if (= n 0) 'done (loop (- n 1))))
(loop 1000000000)              ; 無限再帰でもスタックオーバーフローしない
```

`if` の枝、`cond`/`case` の右辺、`let`/`begin` の最終式、`and`/`or` の
末項などはすべて末尾位置として扱われる。

## 第一級継続 — `call/cc`

`call/cc` (call-with-current-continuation) は、現在の「これからの計算」を
1 引数の手続きとして取り出す:

```scheme
(+ 1 (call/cc (lambda (k) (+ 2 (k 10)))))    ; => 11
```

ascheme では **脱出継続 (one-shot, downward)** のみサポート。一度しか
呼べないし、外側から内側へ復帰することはできない。代表的用法は早期脱出と例外:

```scheme
(define (find pred lst)
  (call/cc (lambda (return)
    (for-each (lambda (x) (if (pred x) (return x))) lst)
    #f)))
```

## 多値

```scheme
(values 1 2 3)                              ; 3 つの値を返す
(call-with-values
  (lambda () (values 1 2))
  (lambda (a b) (+ a b)))                   ; => 3
```

## 約束 (lazy)

```scheme
(define p (delay (begin (display "calc!") 42)))
(force p)        ; "calc!" を 1 回だけ表示し 42 を返す
(force p)        ; 2 回目以降は memoize 済み — 表示なしで 42
```

## 数値演算

R5RS の標準手続きを概ね網羅:

```
+ - * / quotient remainder modulo
= < > <= >= zero? positive? negative? odd? even?
abs min max gcd lcm expt sqrt exp log sin cos tan ...
floor ceiling truncate round
exact->inexact inexact->exact
number->string string->number
exact? inexact? integer? rational? real? complex? number?
```

混合演算は数値タワーに沿って自動昇格 (`(+ 1/2 0.5)` → `1.0`)。

## リスト・ペア

```
cons car cdr  caar cadr cdar cddr  caaar ... cdddr
pair? null? list? list length list-ref list-tail reverse
append map for-each filter
member memq memv assq assv assoc
```

`cons` で作るペアの `cdr` がペア or `'()` のとき、それを「リスト」と呼ぶ。
`'()` は「空リスト」で、リストの終端。

## 文字列・文字・ベクタ

```
string-length string-ref string-set! substring string-append
string->list list->string string=? string<? ...
char->integer integer->char char-alphabetic? ...
make-vector vector-length vector-ref vector-set! vector->list ...
```

## I/O

```
display write newline read read-char peek-char eof-object?
open-input-file open-output-file close-input-port close-output-port
with-input-from-file with-output-to-file
current-input-port current-output-port
```

`display` は人間向け (文字列をクオートしない)、`write` は読み戻し可能形式
(クオート付き)。

## 高階・関数

```
apply procedure? compose
map for-each filter fold fold-right reduce
```

## マクロ

R5RS の `define-syntax` + `syntax-rules` をサポート (健全マクロ)。

```scheme
(define-syntax swap!
  (syntax-rules ()
    ((_ a b) (let ((tmp a)) (set! a b) (set! b tmp)))))
```

(`syntax-case` や explicit-renaming は未対応。)

## 例外

R5RS には組み込み例外なし。ascheme では `error` で打ち切る:

```scheme
(error "bad input:" x)         ; メッセージ + 値群
```

`call/cc` を使えばユーザレベルで try/catch 風の機構が組める。

## 例

```scheme
;; 階乗
(define (fact n)
  (if (= n 0) 1 (* n (fact (- n 1)))))
(fact 50)
;; => 30414093201713378043612608166064768844377641568960512000000000000

;; 高階の sum
(define (sum lst)
  (if (null? lst) 0 (+ (car lst) (sum (cdr lst)))))
(sum (map (lambda (x) (* x x)) '(1 2 3 4 5)))    ; => 55

;; 名前付き let で末尾再帰ループ
(let loop ((i 0) (acc 0))
  (if (= i 100) acc (loop (+ i 1) (+ acc i))))   ; => 4950
```

## サポートしない

- `dynamic-wind` の完全動作 (継続の入退室で起動するハンドラ)
- 完全な継続 (上向き / 多重起動) — one-shot 脱出継続のみ
- `eval` / `interaction-environment` / `scheme-report-environment`
- `syntax-case` / `define-record-type` (R5RS 範囲外)
