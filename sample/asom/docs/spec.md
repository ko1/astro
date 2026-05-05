# asom 言語仕様

`asom` は **SOM (Simple Object Machine)** の実装。SOM は教育用に簡略化された
**Smalltalk 方言**で、純粋オブジェクト指向 — **すべてが「オブジェクト」、
すべての操作が「オブジェクトへのメッセージ送信」**という Smalltalk の
基本原理をそのまま持つ。Java 由来の構文ではなく、Smalltalk のコロン区切り
キーワード呼出を使う。

asom は SOM 公式の標準ライブラリと TestSuite (221/221) と
AreWeFastYet (16/16) をすべて通る。仕様の正規ドキュメントは
[SOM の公式ページ](https://som-st.github.io/) を参照。

## 用語

- **オブジェクト**: 状態 (フィールド) と振る舞い (メソッド) を持つ値。
- **クラス**: オブジェクトの型。フィールド・メソッドを定義する。
- **メッセージ送信**: `receiver method` の形でオブジェクトにメソッド呼出を依頼する操作。Smalltalk の唯一の計算原理。
- **ブロック (block)**: `[ ... ]` で書かれる無名関数オブジェクト。`value` メッセージで実行する。
- **シンボル**: `#name` 形式の即値。同じ名前は世界に 1 つ。

## 値の種類

| 種別 | 例 | クラス |
|---|---|---|
| 整数 | `42` `-3` | `Integer` (任意精度) |
| 浮動小数 | `3.14` | `Double` |
| 文字列 | `'hello'` | `String` |
| シンボル | `#run:` | `Symbol` |
| 真偽 | `true` `false` | `True` / `False` |
| nil | `nil` | `Nil` |
| 配列 | `#(1 2 3)` | `Array` |
| ブロック | `[ :x \| x * 2 ]` | `Block` |

`true` `false` `nil` も普通のオブジェクト。`true` に `not` を送ると `false` を返す、というような書き方をする。

## メッセージ送信の三形態

Smalltalk のメソッド呼出は構文的に 3 種類:

### 1. 単項 (unary) — 引数なし

```smalltalk
3 negated         "→ -3"
'abc' size        "→ 3"
true not          "→ false"
```

### 2. 二項 (binary) — 演算子記号 1 つを受け取る

```smalltalk
3 + 4             "→ 7"
3 < 4             "→ true"
'a' , 'b'         "→ 'ab'"
```

`+ - * / < > <= >= = , ~= //` などが二項メッセージ。**演算子優先順位はなく**、左から右に順次評価される。`2 + 3 * 4` は `(2 + 3) * 4 = 20`。

### 3. キーワード (keyword) — `name:` のラベルで引数を取る

```smalltalk
arr at: 1 put: 42                "arr に at:put: メッセージを送る"
1 to: 10 do: [:i | i print]       "Integer の to:do: で 1..10 をループ"
ifTrue:ifFalse: の例 (条件分岐):
  x > 0 ifTrue: [ 'positive' ] ifFalse: [ 'non-positive' ]
```

キーワードは `:` 付きの識別子を「連結したメソッド名」として読む。
`arr at: 1 put: 42` のメソッド名は `at:put:` で、引数 2 つ。

優先度は **単項 > 二項 > キーワード**。`a + b printString` は `a + (b printString)` ではなく `(a + b) printString`(単項が二項より強い ⇒ b printString が先かと思いきや、左結合なので a + b が先) ではなく実際は単項が強い: `a + (b printString)` と評価される。

## クラス定義

ファイル名 `<ClassName>.som` 1 つにつき 1 クラス。

```smalltalk
Point = (
  | x y |                  "インスタンス変数"

  init: ax y: ay = (
    x := ax.
    y := ay
  )

  x = ( ^x )               "ゲッター"
  y = ( ^y )

  + other = (               "演算子オーバーロード"
    ^ Point new init: x + other x y: y + other y
  )

  printOn: stream = (
    stream nextPutAll: '('.
    x printOn: stream.
    stream nextPutAll: ', '.
    y printOn: stream.
    stream nextPutAll: ')'
  )
)
```

- `ClassName = (...)` あるいは `ClassName = SuperClass (...)` (継承)。
- `| f1 f2 |` でインスタンスフィールド宣言。
- `selector = ( body )` でメソッド定義。`selector` の形が三形態 (上述) を決める。
- 本体内の `:=` で代入、`^expr` で早期 return。
- メソッドは `^` で値を返す。`^` がなければ `self` を返す。

クラス側 (class-side) メソッド (Java の static に相当) も定義できる:

```smalltalk
Point = (
  ...
  ----                  "----- 線でクラス側に切り替え"
  zero = ( ^ Point new init: 0 y: 0 )
)
```

## ブロック (block)

第一級の無名関数オブジェクト。

```smalltalk
[ 42 ] value                            "引数 0、value で起動 → 42"
[:x | x * 2] value: 3                   "引数 1、value: → 6"
[:x :y | x + y] value: 3 with: 4        "引数 2、value:with:"
```

ブロック引数の上限は 4 (`value` / `value:` / `value:with:` / `value:with:with:` / `value:with:with:with:`)。

ブロックは閉包: 定義元のスコープのインスタンス変数・ローカル変数を捕獲する。

### 非局所リターン (NLR)

ブロックの中で `^expr` を書くと、**ブロックを定義したメソッドから return** する (ブロックを呼んだ場所からではない)。

```smalltalk
findNegative: arr = (
  arr do: [:e | e < 0 ifTrue: [ ^e ] ].   "見つけた瞬間メソッドから抜ける"
  ^nil
)
```

メソッドが既に終了した後で NLR を発火するとエラー。

## 制御構造はメソッドで書ける

if / while / for に相当する構文はなく、すべてメッセージ送信:

```smalltalk
"if-else"
n > 0 ifTrue: [ 'pos' ] ifFalse: [ 'neg' ]

"if のみ"
n = 0 ifTrue: [ 'zero' ]

"while"
[ i < 10 ] whileTrue: [ i := i + 1 ]

"to:do:"
1 to: 10 do: [:i | i print ]

"timesRepeat:"
3 timesRepeat: [ 'hi' println ]

"collection iteration"
#(1 2 3) do: [:e | e print ]
arr collect: [:e | e * 2 ]
arr select: [:e | e > 0 ]
```

`ifTrue:ifFalse:` は `True` / `False` クラスのメソッドで、引数 2 つの
ブロックの片方だけを `value` する、という実装。`True>>ifTrue:ifFalse: =
( ^aBlock value )`。

## ファイルとプログラムの起動

asom はクラスを読み込んで `run` (引数あれば `run:`) を呼ぶ:

```smalltalk
"Hello.som"
Hello = (
  run = ( 'Hello, World from SOM' println )
)
```

```sh
./asom Hello       # → "Hello, World from SOM"
```

`run:` 版は `Array of Symbol` (コマンドライン引数) を受け取る。

## 標準クラスの主要メソッド (抜粋)

`Smalltalk/` の標準ライブラリで定義されており、SOM 仕様準拠。

### `Integer`

`+ - * / // \\ < > <= >= = ~= negated abs sqrt sin cos
to:do: to:by:do: timesRepeat: between:and: max: min:
asString printString print println`

### `String`

`size length asString , = ~= < >
indexOf: asInteger asSymbol asArray
substringFrom:to: split:`

### `Array`

`size at: at:put: do: collect: select: reject:
inject:into: indexOf: copy printString`

### `Block`

`value value: value:with: value:with:with: value:with:with:with:
whileTrue whileTrue: whileFalse:`

### `Object`

`class hash = == ~= ~~ printOn: printString println
isKindOf: respondsTo: doesNotUnderstand:`

### `System`

`ticks (現在時刻 µs) global: putGlobal: exit:
loadFile: printString:`

## 例: フィボナッチ

```smalltalk
Fib = (
  fib: n = (
    n < 2 ifTrue: [ ^n ].
    ^ (self fib: n - 1) + (self fib: n - 2)
  )

  run = ( (self fib: 30) println )
)
```

```sh
./asom Fib    # → 832040
```

## 持たない / 制限

- メタクラス階層の完全実装 (`metaclass` / `pragma` 等は単純化)
- `Smalltalk-80` 互換のフル機能 — SOM 仕様に従い意図的に小さい
- ファイル I/O / ソケット / GUI
- 動的 `eval`

詳細は SOM 公式ドキュメントと [`done.md`](done.md) / [`todo.md`](todo.md) を参照。
