# aforth 言語仕様

`aforth` は Forth のサブセット。ここで言う **Forth** は、データを積み下ろし
する「データスタック」を中心に、**右から左に並んだ語 (word) を順番に実行する**
言語族のこと。たとえば `1 2 +` は「1 を積む → 2 を積む → `+` がスタック上の
2 つを加算して 3 を残す」と読む。

aforth は ANS Forth に近い CORE 系の語を採るが、`EXIT` / `DOES>` / locals /
浮動小数 / immediate word は持たない (詳細は末尾)。

## 値とスタック

- **セル (cell)**: 値の単位。aforth では符号付き 64bit 整数 (`int64_t`)。
- **データスタック**: 計算中の値が積まれる LIFO。常に「上 (top of stack, TOS)」が直近の値。
- **リターンスタック**: 制御フロー用の補助スタック (`>R` / `R>` / `R@` で操作)。
- **真偽値**: 整数で表現。**真は `-1` (全ビット 1)、偽は `0`** (Forth の慣習)。

スタック効果はコメント表記で `( before -- after )` と書く。例:
`+` は `( a b -- a+b )`。

## リテラル

10 進整数のみ。`-3` のような負号も整数リテラルとして読まれる。

```forth
1 2 + .       \ 出力: 3
```

(`'a'` のような文字リテラル、文字列、浮動小数はなし。文字列は `." ..."` で
直接出力するだけ — 文字列値そのものは扱わない。)

## 算術・比較・ビット演算

| カテゴリ | 語 |
|---|---|
| 算術 | `+ - * / MOD NEGATE ABS 1+ 1- 2* 2/` |
| 比較 | `= <> < > <= >= 0= 0< 0>` (戻り値: 真は `-1`、偽は `0`) |
| ビット | `AND OR XOR INVERT LSHIFT RSHIFT` |

```forth
3 4 +              \ スタック: 7
10 3 MOD           \ スタック: 1
5 6 <              \ スタック: -1  (真)
```

## スタック操作

| 語 | 効果 |
|---|---|
| `DUP` | TOS を複製: `( a -- a a )` |
| `DROP` | TOS を捨てる: `( a -- )` |
| `SWAP` | 上 2 つを入れ替え: `( a b -- b a )` |
| `OVER` | 2 番目を複製して上に: `( a b -- a b a )` |
| `ROT` | 3 番目を上に: `( a b c -- b c a )` |
| `NIP` | 2 番目を捨てる: `( a b -- b )` |
| `TUCK` | TOS を 2 番目の下に: `( a b -- b a b )` |
| `2DUP` | 上 2 つを複製: `( a b -- a b a b )` |
| `2DROP` | 上 2 つを捨てる |
| `?DUP` | TOS が 0 でなければ DUP |
| `DEPTH` | 現在のデータスタックの深さを積む |

## リターンスタック操作

| 語 | 効果 |
|---|---|
| `>R` | データスタック → リターンスタックへ移動 |
| `R>` | リターンスタック → データスタックへ移動 |
| `R@` | リターンスタックの TOS をデータスタックに複製 |

リターンスタックは制御フロー (`DO`/`LOOP` 内のループカウンタ等) でも
使われるので、word 境界をまたいだ操作は破綻に注意。

## 制御フロー

word 定義の中で使う。トップレベルでも書ける。

### `IF ... ELSE ... THEN`

TOS を取り出して真偽判定。`ELSE` 節は省略可。

```forth
: classify ( n -- )
  DUP 0 < IF
    ." negative"
  ELSE
    ." non-negative"
  THEN
  DROP ;
```

### 無限ループ系

```forth
: loop1 BEGIN ... condition UNTIL ;        \ condition が真になるまで
: loop2 BEGIN ... AGAIN ;                   \ 無限 (LEAVE で抜ける)
: loop3 BEGIN cond WHILE ... REPEAT ;       \ cond が偽になるまで
```

### カウンタループ `DO ... LOOP`

```forth
10 0 DO  I .  LOOP        \ 0 1 2 ... 9 を出力
```

- 開始値・終端値をデータスタックから取って (`limit start DO`)、
  カウンタを 1 増やしながら本体を実行。終端値に達したら抜ける。
- `I` で内側のカウンタ、`J` でその外側のカウンタを参照。
- `+LOOP` は刻み幅をスタックから取る (`step +LOOP`)。
- `LEAVE` でループから即座に抜ける。

## 語の定義

```forth
: SQUARE ( n -- n*n )  DUP * ;
5 SQUARE .              \ 25
```

- `: NAME ... ;` で新しい word を定義。中括弧不要、空白で区切る。
- 自己再帰は `RECURSE` を使う (定義中の自分の名前は使えない):

```forth
: FIB ( n -- fib_n )
  DUP 2 < IF EXIT-FAKE THEN
  DUP 1- RECURSE  SWAP 2 - RECURSE  + ;
```

(注: `EXIT` は未対応 — 上は説明用の擬似例。実際は `IF ... ELSE ... THEN` で書く。)

## 変数・定数・ALLOT

メモリは「セル (8 byte) を単位」とした生領域に確保する。

```forth
VARIABLE COUNT          \ COUNT は 1 セル分の領域 + そのアドレスを返す word
0 COUNT !               \ アドレスに 0 を格納
COUNT @ 1+ COUNT !      \ インクリメント
COUNT ?                 \ 値を出力 (= @ . )
```

| 語 | 効果 |
|---|---|
| `VARIABLE NAME` | 1 セル領域を確保 |
| `<n> CONSTANT NAME` | コンパイル時定数 (`NAME` 実行で `n` を積む) |
| `CREATE NAME` | 領域を確保し、開始アドレスを返す word を作る |
| `<n> ALLOT` | 直近の `CREATE` 領域を `n` byte 拡張 |
| `@` | アドレスから 1 セル読む |
| `!` | アドレスに 1 セル書き込む |
| `+!` | アドレスの値に加算 |
| `CELLS` | `n CELLS` = `n * 8` (セルサイズ倍) |
| `CELL+` | アドレス + 8 |

## 入出力

| 語 | 効果 |
|---|---|
| `.` | TOS を整数として出力 |
| `EMIT` | TOS を ASCII 1 文字として出力 |
| `CR` | 改行 |
| `SPACE` | 空白 1 つ |
| `BL` | 空白文字コード (32) を積む |
| `." string"` | 直書きの文字列をそのまま出力 (空白で終端) |

入力 (`KEY` / `ACCEPT` 等) は無し。

## コメント

- `\` から行末まで
- `( ... )` で囲まれた範囲

`(` と `)` の間にも空白が必要 (Forth の慣習通り)。

## 例

```forth
\ 階乗
: FACT ( n -- n! )
  DUP 1 <= IF DROP 1 EXIT-FAKE THEN
  DUP 1- RECURSE * ;

\ Collatz
: COLLATZ ( n -- count )
  0 SWAP
  BEGIN  DUP 1 >  WHILE
    DUP 2 MOD 0= IF 2 / ELSE 3 * 1+ THEN
    SWAP 1+ SWAP
  REPEAT
  DROP ;
```

## 持たない機能

- `EXIT` (word の途中復帰)
- `DOES>` (ユーザ定義 word ファクトリ)
- locals (`{ a b -- }` 等)
- immediate word / `IMMEDIATE` / コンパイル時拡張
- 浮動小数 (`F+` 等)
- スレッド / ファイル I/O
- マルチタスク

詳細リスト: [`done.md`](done.md) / [`todo.md`](todo.md)。
