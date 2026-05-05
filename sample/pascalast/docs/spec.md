# pascalast 言語仕様

`pascalast` は **Pascal のサブセット**インタプリタ。Pascal は静的型付き
の構造化プログラミング言語で、明示的な型宣言・begin/end ブロック・
procedure/function の区別 (副作用あり/値を返す) を特徴とする。
pascalast は ISO 7185 (古典 Pascal) + Free Pascal 風 OO の主要部を実装する。

完全な Pascal 仕様は ISO 7185 / [Free Pascal Reference](https://www.freepascal.org/docs.html)
を参照。本書は pascalast で動く範囲を端的に示す。

## 用語

- **`procedure`**: 値を返さないサブルーチン (戻り値なし)。手続き。
- **`function`**: 値を返すサブルーチン。
- **`var` パラメータ**: 参照渡し (関数内で書き換えると呼出元の変数も変わる)。
- **`begin ... end`**: 文のブロック (他言語の `{ ... }` に相当)。

Pascal は **キーワード・識別子の大文字小文字を区別しない**。`Begin` と
`begin` と `BEGIN` は同じ。本書では小文字で統一。

## プログラムの基本形

```pascal
program hello;

var
  x: integer;
  s: string;

begin
  x := 42;
  s := 'world';
  writeln('hello, ', s, '! x = ', x)
end.
```

- `program <name>;` でプログラム名を宣言。
- `var ... ;` で変数宣言 (型は名前の後ろに `:` で書く)。
- `begin ... end.` (末尾はピリオド) がプログラム本体。
- 文の区切りは `;`。`end` 直前は `;` 不要 (省略可)。

## 型

### 数値・真偽

| 型 | 内部表現 | 別名 |
|---|---|---|
| `integer` | 64-bit 符号付き整数 | `longint` `int64` `word` |
| `boolean` | 真偽 (`true` `false`) | |
| `real` | IEEE-754 double | `double` `single` |
| `char` | 1 文字 | |
| `string` | 文字列 | `AnsiString` 互換の dynamic 文字列 |

`integer → real` の昇格は自動。逆は `trunc(x)` (切り捨て) / `round(x)` (四捨五入) を使う。

### Subrange (部分範囲)

```pascal
type
  digit = 0..9;
var
  d: 1..100;          { 1〜100 の値しか入らない (代入時に範囲チェック) }
```

範囲外を代入すると実行時エラー。

### 列挙型 (enum)

```pascal
type
  color = (red, green, blue);
var
  c: color;
begin
  c := green;
  if c = red then ...
end.
```

### 配列

```pascal
var
  a: array[1..100] of integer;        { 1-origin ベクタ }
  m: array[1..10, 1..10] of integer;  { 2D }
  m2: array[1..10] of array[1..10] of integer;  { 同等 }
  ds: array of integer;                { 動的配列 (サイズ実行時決定) }
```

動的配列は `setlength(ds, n)` でサイズを設定。

### `record`

```pascal
type
  point = record
    x, y: integer;
  end;
var
  p: point;
begin
  p.x := 1;
  p.y := 2;
  with p do begin x := 10; y := 20 end
end.
```

### `variant record` (タグ付き union)

```pascal
type
  shape = record
    case kind: integer of
      1: (radius: real);              { 円 }
      2: (width, height: real);       { 長方形 }
  end;
```

### Set (集合)

```pascal
var
  s: set of 1..100;
begin
  s := [1, 5, 10..20];     { 集合リテラル }
  if 7 in s then ...
end.
```

### `pointer`

```pascal
var
  p: ^integer;        { integer へのポインタ }
begin
  new(p);             { 確保 }
  p^ := 42;           { dereference }
  dispose(p)          { 解放 }
end.
```

## リテラル

```pascal
42  -3  $ff             { 16 進: $ff }
3.14  1e10  -0.5
true  false
'a'                      { char }
'hello'                  { string }
'don''t'                 { ' は '' とエスケープ }
[1, 2, 3..5]             { set リテラル }
nil                      { ポインタの空 }
```

## 演算子

| カテゴリ | 演算子 |
|---|---|
| 算術 | `+ - * / div mod` (`/` は real、`div`/`mod` は integer) |
| 比較 | `< > <= >= = <>` (`=` は等価、`<>` は不等) |
| 論理 | `and or not xor` (短絡なし — Pascal 標準) |
| ビット | `and or not xor shl shr` (integer に対して) |
| 集合 | `+ - * in <= >=` (和・差・積・要素・部分集合) |
| ポインタ | `^` (dereference)、`@` (アドレス取得) |
| 文字列 | `+` (連結)、`[i]` (1-origin で文字取得) |
| 代入 | `:=` |

`/` は常に real を返す。整数除算は `div`、剰余は `mod`。

## 制御構文

### 代入

```pascal
x := 10;
inc(x);                  { x := x + 1 }
dec(x, 5);               { x := x - 5 }
```

### `if`

```pascal
if cond then stmt
if cond then stmt else stmt

if cond then begin
  stmt1;
  stmt2
end else begin
  stmt3
end
```

`if-then-else` の前の文末 `;` は不要 (Pascal 文法の罠)。

### `while` / `repeat`

```pascal
while cond do stmt;

repeat
  stmt
until cond;              { cond が真になったら抜ける (do-while と逆) }
```

### `for`

```pascal
for i := 1 to 10 do stmt;
for i := 10 downto 1 do stmt;
for c in 'hello' do ...   { for-in (コレクション反復) }
```

ループ変数の終端値は **ループ開始時に固定** される (途中で変えてもループ回数は変わらない)。

### `case`

```pascal
case x of
  1, 2:    writeln('small');
  3..10:   writeln('mid');
  100:     writeln('big')
  else     writeln('other')
end;
```

### `goto`

ラベル `<n>:` 経由でジャンプ。`label <n>;` で事前宣言が必要 (Pascal 標準)。
ループ外への脱出くらいに使う。

## procedure / function

```pascal
procedure swap(var a, b: integer);   { var パラメータ = 参照渡し }
var t: integer;
begin
  t := a; a := b; b := t
end;

function fib(n: integer): integer;
begin
  if n < 2 then fib := n
  else fib := fib(n-2) + fib(n-1)
  { 戻り値は関数名 := value で書く (Pascal 標準) }
end;

function gcd(a, b: integer): integer;
begin
  if b = 0 then gcd := a
  else gcd := gcd(b, a mod b)
end;
```

`forward;` 宣言で相互再帰可:

```pascal
function odd_(n: integer): boolean; forward;

function even_(n: integer): boolean;
begin
  if n = 0 then even_ := true
  else even_ := odd_(n - 1)
end;

function odd_(n: integer): boolean;
begin
  if n = 0 then odd_ := false
  else odd_ := even_(n - 1)
end;
```

ネストした procedure/function (内部手続き) も書ける — Pascal の特徴。

## 文字列操作

`AnsiString` 互換の dynamic 文字列:

```pascal
s := 'hello';
length(s)                       { 5 }
s[1]                            { 'h' (1-origin) }
s := s + ', world';
copy(s, 1, 5)                   { 'hello' }
pos('world', s)                 { 8 }
insert('!', s, 6);
delete(s, 6, 1);
setlength(s, 3);                { 'hel' }

inttostr(42)                    { '42' }
strtoint('42')                  { 42 }
```

## 例外処理

Free Pascal 風の `try/except/finally`:

```pascal
try
  risky_code
except
  on e: EDivByZero do
    writeln('div by zero');
  on e: Exception do
    writeln('other: ', e.message)
end;

try
  acquire_resource;
  ...
finally
  release_resource
end;

raise Exception.create('oops');
```

## OOP (Free Pascal 風)

```pascal
type
  TAnimal = class
  protected
    fName: string;
  public
    constructor create(n: string);
    destructor destroy; override;
    function greet: string; virtual;
    property name: string read fName;
  end;

  TDog = class(TAnimal)
  public
    function greet: string; override;
  end;

constructor TAnimal.create(n: string);
begin
  fName := n
end;

destructor TAnimal.destroy;
begin
  inherited                       { 親 destructor 呼出 }
end;

function TAnimal.greet: string;
begin
  greet := 'hi ' + fName
end;

function TDog.greet: string;
begin
  greet := inherited greet + ', woof!'
end;

var
  a: TAnimal;
begin
  a := TDog.create('rex');
  writeln(a.greet);                { → 'hi rex, woof!' (vtable で動的呼出) }

  if a is TDog then writeln('is dog');
  (a as TDog).bark;                 { 安全キャスト }

  a.destroy
end.
```

サポート機能:

- 単一継承 (`class(Parent)`)
- `virtual` / `override` / `abstract` / `inherited`
- コンストラクタ (`create`) / デストラクタ (`destroy`)
- `is` (型チェック) / `as` (型キャスト)
- `class procedure` / `class function` (静的メソッド相当)
- `property fname read getter [write setter]`

メモリ管理は libgc (Boehm GC) backed なので `destroy` を呼ばなくても回収される。

## unit / uses

```pascal
unit MyMath;

interface
function fib(n: integer): integer;

implementation
function fib(n: integer): integer;
begin
  if n < 2 then fib := n else fib := fib(n-2) + fib(n-1)
end;

end.
```

```pascal
program app;
uses MyMath;
begin
  writeln(fib(10))
end.
```

`interface` 部の宣言だけが外部から見える。

## 組み込み (主なもの)

- 数学: `abs sqr sqrt sin cos exp ln pi succ pred`
- 入出力: `write writeln read readln`
- 整数: `inc dec odd ord chr trunc round`
- 文字列: `length copy pos insert delete concat setlength
  inttostr strtoint floattostr lowercase uppercase`
- 集合・配列: `setlength low high length`
- メモリ: `new dispose getmem freemem`
- 制御: `halt exit break continue`

## 例

```pascal
program quicksort;

const N = 10000;
var
  a: array[1..N] of integer;
  i: integer;

procedure qsort(lo, hi: integer);
var
  pivot, l, r, t: integer;
begin
  if lo < hi then begin
    pivot := a[(lo + hi) div 2];
    l := lo; r := hi;
    while l <= r do begin
      while a[l] < pivot do inc(l);
      while a[r] > pivot do dec(r);
      if l <= r then begin
        t := a[l]; a[l] := a[r]; a[r] := t;
        inc(l); dec(r)
      end
    end;
    qsort(lo, r);
    qsort(l, hi)
  end
end;

begin
  for i := 1 to N do a[i] := N - i + 1;
  qsort(1, N);
  for i := 1 to 5 do write(a[i], ' ');
  writeln
end.
```

## 持たない / 制限

- visibility の実行時 enforce (`private` / `protected` の構文受理のみ)
- N 次元 (3D 以上) 配列のフル機能 (2D まで)
- `goto` のフル対応 (構造化制御内のラベルは限定的)
- open array param (`procedure f(a: array of integer)`)
- コンパイラディレクティブ `{$R+}` 等 (構文受理のみ)
- generics (`generic class`/`specialize`)
- inline assembler

詳細: [`done.md`](done.md) / [`todo.md`](todo.md)。
