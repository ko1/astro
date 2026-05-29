# ChocoPy 実装リファレンス（別実装向け）

本書は **ChocoPy を一から実装する**ための自己完結した仕様。原典は
`chocopy.org` の *ChocoPy v2.2: Language Manual and Reference*（Padhye & Sen,
Hilfinger / UC Berkeley）。AnPy はこの仕様の一実装で、別の言語・別の方式で
再実装しても [testing.md](testing.md) の同じテストで検証できる。

ChocoPy は **静的型付き Python 3.6 サブセット**。重要な性質:

> 妥当な ChocoPy プログラムはほぼ全て妥当な Python 3.6 プログラムであり、
> エラーなく実行されるなら Python 3.6 と同じ観測可能セマンティクスを持つ（§A の
> 例外を除く）。

この性質が差分テストの根拠になる（[testing.md](testing.md)）。

Python より厳格な主な点:
- **定義が文より前**: プログラム/関数本体は「変数・関数・クラス定義」を全て先に置き、
  その後に文。
- 変数・属性・引数・戻り値に**型注釈が必須**。各変数は生涯ひとつの型。
- 比較は**非結合**（`a < b < c` は構文エラー）。
- `print` の引数は `int`/`bool`/`str` のみ。
- `/`（float 除算）なし、`//`（整数除算）のみ。dict / 第一級関数 / 内省なし。

---

## 1. 字句構造（§3）

### 1.1 行・インデント

物理行の終端は LF / CRLF / CR のいずれでもよい。入力末尾も終端。
**論理行** = 空白・コメント以外のトークンを1つ以上含む物理行。論理行の終端で
**NEWLINE** を出す。空行（空白・コメントのみ）は NEWLINE を出さない。
コメントは `#` から行末まで（文字列リテラル内を除く）。

**インデント**（INDENT/DEDENT、Python と同じ）:
- タブは左から、合計が 8 の倍数になるよう 1〜8 個の空白に置換。先頭の空白数が
  その行のインデントレベル。
- スタックを使う。最初に `0` を push（二度と pop しない）。各論理行の先頭で
  レベルをスタック頂と比較: 等しければ何もしない。大きければ push して **INDENT** を
  1 個出す。小さければ、それがスタック中のいずれかの値に一致するまで pop し、pop
  ごとに **DEDENT** を 1 個出す（一致しなければエラー）。入力末尾で、0 より大きい
  残り全てに対し DEDENT を出す。
- `( )` と `[ ]` の内側では改行は無意味（暗黙の行継続。Python 準拠）。この間は
  インデント処理をしない。

### 1.2 識別子・キーワード

識別子 = `[A-Za-z_][A-Za-z0-9_]*`（最長一致。`classic` は識別子で `class` を含むだけ）。

キーワード（Python と同一の予約語。一部は ChocoPy では未使用で構文エラーになるだけ）:
`False None True and as assert async await break class continue def del elif else
except finally for from global if import in is lambda nonlocal not or pass raise
return try while with yield`。

### 1.3 リテラル

- **整数**: 1 桁以上の `0-9`。先頭 0 は単独 `0` のときのみ可（先行ゼロ禁止）。10 進。
  最大値は `2147483647`（= 2³¹−1）。超えると字句エラー。
- **文字列**: `"` で囲む ASCII（10進 32〜126）の並び。エスケープは `\"` `\\` `\t` `\n`
  の 4 種のみ（他は不正）。`\"` は二重引用符、それ以外は対応する文字。値は
  デリミタ間の文字列にエスケープを適用したもの。
  - **IDSTRING** = 内容が識別子構文の文字列リテラル（型注釈でクラス名として使う）。
    それ以外の文字列は **STRING**。実行時の値としては両者とも文字列。
- **真偽**: キーワード `True` / `False`。

### 1.4 演算子・区切り（distinct トークン）

`+ - * // % < > <= >= == != = ( ) [ ] , : . ->`

---

## 2. 構文（§4）

```
program     ::= [ vardef | funcdef | classdef ]* stmt*
classdef    ::= class ID ( ID ) : NEWLINE INDENT classbody DEDENT
classbody   ::= pass NEWLINE | [ vardef | funcdef ]+
funcdef     ::= def ID ( [ typedvar [, typedvar]* ]? ) [ -> type ]? : NEWLINE INDENT funcbody DEDENT
funcbody    ::= [ globaldecl | nonlocaldecl | vardef | funcdef ]* stmt+
typedvar    ::= ID : type
type        ::= ID | IDSTRING | [ type ]
globaldecl  ::= global ID NEWLINE
nonlocaldecl::= nonlocal ID NEWLINE
vardef      ::= typedvar = literal NEWLINE
stmt        ::= simple_stmt NEWLINE
              | if expr : block [ elif expr : block ]* [ else : block ]?
              | while expr : block
              | for ID in expr : block
simple_stmt ::= pass | expr | return [ expr ]? | [ target = ]+ expr
block       ::= NEWLINE INDENT stmt+ DEDENT
literal     ::= None | True | False | INTEGER | IDSTRING | STRING
expr        ::= cexpr
              | not expr
              | expr [ and | or ] expr
              | expr if expr else expr
cexpr       ::= ID | literal | [ [ expr [, expr]* ]? ] | ( expr )
              | member_expr | index_expr
              | member_expr ( [ expr [, expr]* ]? )
              | ID ( [ expr [, expr]* ]? )
              | cexpr binop cexpr
              | - cexpr
binop       ::= + | - | * | // | % | == | != | <= | >= | < | > | is
member_expr ::= cexpr . ID
index_expr  ::= cexpr [ expr ]
target      ::= ID | member_expr | index_expr
```

**expr / cexpr 分割の意味**: `and`/`or`/`not`/三項は `expr` レベル。算術・比較・
呼び出し・添字・属性・単項マイナスは `cexpr` レベル。よって `True == not False` は
構文エラー（`not` は比較の被演算子になれない）。論理演算子の被演算子に論理式を
書くには括弧が要る。ただし**リスト要素・呼び出し引数・添字・括弧・三項の枝**は
完全な `expr`。

### 2.1 優先順位（§4.1、低→高）

| 優先 | 演算子 | 結合 |
|---|---|---|
| 1 | `... if ... else ...` | 右 |
| 2 | `or` | 左 |
| 3 | `and` | 左 |
| 4 | `not` | — |
| 5 | `== != < > <= >= is` | **非結合**（連鎖不可） |
| 6 | `+ -`（2項） | 左 |
| 7 | `* // %` | 左 |
| 8 | `-`（単項） | — |
| 9 | `.` `[]` | 左 |

---

## 3. 型システム（§2.4, §5）

### 3.1 型

- 基本クラス型: `object`（根）, `int`, `bool`, `str`（いずれも `object` の子）。
- ユーザクラス型: 単一継承の木。親は既出のユーザクラスか `object`（`int`/`bool`/`str`
  は親に不可）。
- リスト型 `[T]`（任意の `T`）。再帰可（`[[int]]`）。
- 特殊型: `<None>`（`None` の型）, `<Empty>`（`[]` の型）。プログラム中に書けない。

### 3.2 適合 `≤`（conformance）

- `A ≤ A`
- `C` が `P` のサブクラス（直接/間接）なら `C ≤ P`
- 推移律
- `[T] ≤ object`, `<None> ≤ object`, `<Empty> ≤ object`
- `<None> ≤ <None>`, `<Empty> ≤ <Empty>`
- リスト型同士は **等しいときのみ**関連（`[int]` と `[object]` は無関係）

### 3.3 代入互換 `≤a`

`T1 ≤a T2` は次のいずれか:
1. `T1 ≤ T2`
2. `T1 = <None>` かつ `T2 ∉ {int, bool, str}`
3. `T2 = [T]` かつ `T1 = <Empty>`
4. `T2 = [T]` かつ `T1 = [<None>]` かつ `<None> ≤a T`（**異なるリスト型が互換になる唯一の例**）

### 3.4 join `⊔`（最小上界、`≤a` 順序）

`C = A ⊔ B` ⟺ `A ≤a C` ∧ `B ≤a C` ∧（任意の `D` で `A≤aD ∧ B≤aD ⇒ C≤aD`）。
実装: `A ≤a B` なら `B`、`B ≤a A` なら `A`、さもなくば型階層木での最小共通祖先
（クラス型同士）または `object`。

### 3.5 型環境

`O`（局所: 変数→型 / 関数→シグネチャ）, `M`（クラス→属性/メソッド型）,
`C`（現在のクラス名 or ⊥）, `R`（現在の関数の戻り型 or ⊥）。関数シグネチャは
`{T1×…×Tn → T0; x1,…,xn; v1:T1',…}`（仮引数と局所/ネスト関数の型を含む）。

事前定義:
```
O(len)   = {object → int; arg}
O(print) = {object → <None>; arg}
O(input) = {→ str}
M(object,__init__) = M(int,__init__) = M(bool,__init__) = M(str,__init__) = {object → <None>; self}
```

### 3.6 型検査規則（§5.2、要点）

判定形 `O;M;C;R ⊢ e : T`。

- 変数: `O(id)=T`（関数型でない）⇒ `id : T`。関数名を値として読むのは禁止。
- リテラル: `False`/`True`:bool, INTEGER:int, STRING/IDSTRING:str, `None`:`<None>`。
- 算術 `e1 op e2`（`op ∈ + - * // %`）: 両者 int ⇒ int。単項 `-e`: int ⇒ int。
- 数値比較 `< <= > >=`: 両者 int ⇒ bool。
- 等価 `== !=`: 両者が同じ型 `∈{int,bool,str}` ⇒ bool。
- `is`: 両者とも `∉{int,bool,str}` ⇒ bool。
- 論理 `and`/`or`/`not`: bool ⇒ bool。
- 三項 `e1 if e0 else e2`: `e0`:bool、結果 `T1 ⊔ T2`。
- 文字列: `+`（str×str→str）, `[]`（str×int→str）, `==`/`!=`（str×str→bool）。
- リスト表示 `[e1..en]`(n≥1): 要素型 `T1⊔…⊔Tn` の `[T]`。空 `[]`: `<Empty>`。
- リスト演算: `+`（`[T1]×[T2] → [T1⊔T2]`）, `[]`（`[T]×int → T`）,
  要素代入 `e1[e2]=e3`（`e1:[T]`, `e2:int`, `e3:T3`, `T3 ≤a T`）。
- 属性 `e0.id`: `e0:T0`, `M(T0,id)=T` ⇒ `T`。属性代入も `≤a` で同様。
- オブジェクト生成 `T()`（`T` はクラス）: 型 `T`。
- 関数適用 `f(e1..en)`: `O(f)={T1×…×Tn→T0;…}`、各 `ei` の型が `≤a Ti` ⇒ `T0`。
- メソッド `e1.f(e2..en)`: `M(typeof(e1),f)={T1×…×Tn→T0;…}`、`e1 ≤a T1`、
  以降 `≤a` ⇒ `T0`（**動的ディスパッチ**だが型は静的型で検査）。
- 代入文 `id=e1` / `id:T=e1`: `O(id)=T`, `e1:T1`, `T1 ≤a T`。
- 複数代入 `e1=e2=…=en=e0`: 各 `ei=e0` に分解して検査。ただし `e0` の型が
  `[<None>]` であってはならない（健全性のための制限）。
- `return e`: `e:T`, `T ≤a R`。`return`（値なし）: `<None> ≤a R`。
- `if`/`elif`/`while` の条件: bool。`for id in e`: `e:str` なら `str ≤a O(id)`、
  `e:[T]` なら `T ≤a O(id)`。
- 関数定義: 仮引数・局所・ネスト関数の型で `O` を拡張して本体を検査。`->` が
  あれば戻り型 `T0`、無ければ `<None>`。メソッド定義は加えて第1仮引数の型 =
  定義クラス。
- クラス定義: 本体を `C=クラス名`, `R=⊥` の環境で検査。

実装が**追加で**強制すべき構造規則（健全性に必要）:
- 戻り型が `int`/`bool`/`str` の関数/メソッドは**全実行経路に値付き return** が必要。
- 暗黙継承された read-only 変数（囲みスコープから可視だが宣言していない名前）への
  代入は禁止。関数/クラス名と同名の変数は宣言不可。クラス名はシャドウ不可。
- 属性は（自/継承問わず）再定義不可。メソッドは同一クラス内で再定義不可。
  継承メソッドのオーバーライドは戻り型と第1引数以外の引数型が**厳密一致**のときのみ可。
- `__init__` は戻り型なし（`<None>`）。継承の木構造（循環なし）。

---

## 4. 操作的意味論（§6、実装に必要な観測挙動）

### 4.1 値

- `int(i)`, `bool(True|False)`, `str(n, chars)`（不変, 長さ n）。
- リスト `[l1..ln]`（可変, 固定長, 要素は位置で参照）。
- ユーザオブジェクト `X(a1=…, …)`（属性/メソッドスロットを持つ）。参照セマンティクス。
- `None`。
- 関数（第一級でないが意味論上は値。捕捉環境 `Ef` を持つ closure）。

### 4.2 評価順序・主要規則

- **算術**: `//` `%` は **0 除算で実行時エラー**。Python と同じく**床除算・床剰余**
  （`-7 // 2 = -4`, `-7 % 2 = 1`）。
- **比較/等価**: int/bool は値比較、str は内容比較。
- **`is`**: 両者 `None` なら真、同一オブジェクトなら真、他は偽。
- **論理**: 短絡。`and`: 左が False なら False を返し右を評価しない。`or`: 左が
  True なら True。
- **文字列**: `s[i]` は長さ1の新 str（範囲外は実行時エラー）。`+` は連結（新 str）。
- **リスト**: `[e1..en]` は各要素を左→右に評価し新リスト。`l[i]` は範囲外で
  実行時エラー。`+` は新リスト（長さは和）。`l[i]=e` も範囲外でエラー。
- **代入文 `id=e`**: 右辺を評価して `id` の格納場所を更新。
- **属性代入 `e1.id=e2`**: **右辺 `e2` を先に**、次に `e1` を評価して更新。
- **添字代入 `e1[e2]=e3`**: **右辺 `e3` を先に**、次に `e1`, `e2`。
- **複数代入 `e1=e2=…=en=e0`**: `e0` を**ちょうど1回**評価し、その値を `e1..en` に
  **左→右**に代入（属性/添字ターゲットの部分式もこの順で評価）。
- **`if`/`elif`/`else`**: 条件を順に評価し最初に真の枝のみ実行。`elif` は
  `else: if …` への脱糖と等価。
- **`while`**: 条件が真の間、本体を反復（本体が return したら伝播して終了）。
- **`for id in e`** は次へ脱糖（`itr`/`idx` は新規一時変数。ループ変数 `id` は
  事前宣言が必要で、ループは新スコープを作らない）:
  ```
  itr = e
  idx = 0
  while idx < len(itr):
      id = itr[idx]
      <body>
      idx = idx + 1
  ```
- **`return`**: 値を設定して即座に巻き戻す。値省略時・関数末尾到達時は `None` を返す。
- **関数呼び出し**: 引数を左→右に評価 → 仮引数/局所/ネスト関数に新規格納場所を確保
  → 局所はリテラル初期値、ネスト関数は closure 値で初期化 → 本体を評価。closure は
  定義時の環境を捕捉（ただし `global` 宣言した名前はグローバルへ差し替え）。
- **メソッド呼び出し**: レシーバを評価 → その**動的型**のメソッドスロットを引く →
  レシーバを第1引数にして残りを渡す。メソッドの closure はグローバル環境を捕捉
  （生成時環境は捕捉しない）。
- **オブジェクト生成 `T()`**: 属性/メソッドのスロットを確保 → 属性をリテラル既定値で
  初期化（**グローバル環境**で評価）・メソッドを束縛 → `__init__` を動的ディスパッチで
  起動（引数はレシーバのみ）。`object.__init__` の本体は `pass`。

### 4.3 グローバル変数の初期化

プログラム開始時、グローバル変数定義の**リテラル初期値**を評価して格納し、
関数・クラスをグローバルに束縛してから、トップレベル文を先頭から実行する。

### 4.4 組み込み関数（§2.8.6）

- `print(x)`: `x` が `int`/`bool`/`str` なら印字して改行（`bool` は `True`/`False`、
  `int` は10進、`str` はそのまま）、戻り値 `None`。それ以外は**実行時エラー**。
- `input()`: 標準入力から1行（**末尾改行を含む**）を str で返す。EOF では空文字列。
- `len(x)`: `x` が `str`/リストなら長さ（int）。それ以外は**実行時エラー**。

### 4.5 実行時エラー（§6.4）

静的検査で多くが排除されるため、実行時に起こりうるのは次の5種。発生時はメッセージを
出してプログラムを中断する:
1. `print`/`len` への不正引数
2. ゼロ除算
3. 添字範囲外（文字列選択・リスト要素アクセス）
4. `None` への操作（メソッドディスパッチ・属性アクセス・リスト操作）
5. メモリ不足（オブジェクト生成時）

整数オーバーフローは**未定義動作**（仕様は 32bit 範囲内のみ規定）。

---

## 5. Python との非互換（§A）

- Python 3 は未定義クラスへの前方参照を許さない（`x:A = None` を `class A` 定義前に
  書くと NameError）。ChocoPy は許す。両対応にするには引用形 `x:"A" = None`。
- 整数オーバーフローは ChocoPy では UB、Python は多倍長。32bit を外れる値では非互換。

> 実装・テスト上の含意: 差分テストでは前方参照は引用形を使い、整数は 32bit 範囲に
> 収め、`print` には `int`/`bool`/`str` だけを渡す（[testing.md](testing.md)）。

---

## 6. 実装チェックリスト

1. **字句解析**: 物理行→論理行、インデントスタックで INDENT/DEDENT、`()`/`[]` 内の
   行継続、コメント/空行、文字列/整数リテラル（範囲・エスケープ）、最長一致。
2. **構文解析**: §2 の文法。優先順位（特に expr/cexpr 分割と非結合比較）。定義は文より前。
3. **意味解析（型検査）**: §3 の関係（`≤` `≤a` `⊔`）と §3.6 の規則。クラス階層を確定
   してから式の型推論と文の検査。エラーは実行前に拒否。
4. **実行**: §4 の値・評価順序・脱糖・ディスパッチ・実行時エラー。
5. **観測一致**: `print` の出力（改行・True/False・10進）が Python と一致すること。

AnPy の構成（lexer.c / parse.c / check.c / node.def + node.c / value.c）は一例。
別実装は言語・データ構造・実行方式を自由に選んでよく、[testing.md](testing.md) の
差分テストに通れば適合とみなす。
