# baruby 言語仕様

実装ノートは [runtime.md](runtime.md)、未対応項目は [todo.md](todo.md)、
ベンチ・性能は [perf.md](perf.md) を参照。
**このドキュメントはユーザから見える言語の意味論のみを記述する**
(「baruby はどういう Ruby サブセットか」)。
parser の desugar や VALUE エンコーディングは runtime.md 側。

baruby = "**barely a ruby**"。naruby (= "not a ruby", int64 only) と
abruby (= "a bit ruby", CRuby 拡張) のあいだに位置する小型サブセットで、
**統一 GC 基盤 (`docs/gc_design.md`) のテストベッド**として作られた。
OO 機能 (class / module / method / instance var / block) は意図的に
入れていない — メソッド呼び出し風の構文はすべてパーサが parse-time に
専用ノードへ desugar する (詳細は runtime.md)。

> 「Ruby で書いたつもりが baruby だった」という驚きを最小化するため、
> 文法は Ruby 互換 (Prism でパースする) だが、評価できる値・式は下記の
> 表だけ。範囲外は parser が `unsupported node: PM_<...>` で拒否する。

## 値

3 種類:

| 型 | リテラル | 値表現 |
|---|---|---|
| **Integer** | `42`, `-7`, `1_000_000` | 符号付き 63 bit (LSB-tag) |
| **String** | `"hello"` | mutable byte 列 (UTF-8 想定だが取り扱いはバイト) |
| **Array** | `[1, 2, 3]` | 任意要素の可変長配列 |

加えて真偽値:

| 値 | 意味 |
|---|---|
| `false` | falsy 値 (Integer 0 / 空配列 / 空文字列とは区別) |
| `nil` | falsy 値、`false` とは別シングルトン (`nil != false`) |
| `true` | 真の真偽値 (Integer 1 とは別シングルトン) |
| 上記以外すべて | truthy (整数 0 も `[]` も `""` も真) |

`p (1 == 1)` は `true` と表示される (整数 `1` ではない)。
`nil.to_s == ""`、`false.to_s == "false"` (Ruby 互換)。

**未対応の値型**: 浮動小数 / Hash / Symbol / Range / Regexp / Proc /
Class / Object。

整数オーバーフローは未定義 (C の signed shift と同じ挙動)。bignum
自動拡張・range check はなし。範囲は概ね `±2^62` (LSB tag 1 bit を
使うため naruby より 1bit 狭い)。

## リテラル

```ruby
42           # Integer
-7           # Integer (PM_INTEGER_NODE は符号を含む)
1_000_000    # Integer (アンダースコア区切り OK)

"hello"      # String — eval 毎に fresh alloc される (intern なし)
"abc\n"      # 通常のエスケープ展開は prism 任せ

[1, 2, 3]    # Array — eval 毎に fresh alloc
[]           # 空 Array
[a, [b, c]]  # ネスト OK
```

`true` / `false` / `nil` キーワード対応。比較式 (`1 == 1` 等) も
`true` / `false` を返す。

## ローカル変数 / 代入

```ruby
x = 1
x += 2
a, b = ...    # 多重代入は未対応
```

スコープは `def` 単位 + トップレベル。**インスタンス変数 / グローバル変数 /
クラス変数はすべて未対応**。

## 演算子

### 算術 (Integer のみ)

`+`, `-`, `*`, `/`, `%`。被演算子はどちらも Integer でなければ
ランタイムで "type mismatch in +" 等を出力する。

### 文字列

`+` (concat → 新しい String を返す)、`*` (Integer 倍した新 String)、
`<<` (mutating append、self を返す)。`==` / `!=` は **値比較**
(`"abc" == "abc"` は `true`)。`<` / `<=` / `>` / `>=` / `<=>` は
**辞書式比較** (memcmp + 長さ tiebreak)。

### Array

`[]` (index get) / `[]=` (index set) / `+` (concat → 新配列) /
`*` (Integer 倍した新配列) / `<<` (mutating push、self を返す)。
インデックス負の値は末尾基準 (`a[-1]` で最終要素)。範囲外 read は
`nil` を返す。範囲外 write は `nil` で auto-extend する。
`==` / `!=` は **要素ごとの値比較** (再帰的、`[1, [2, 3]] == [1, [2,
3]]` は `true`)。Array#`<=>` は未対応。

### 比較

`==`, `!=` は Ruby と同じ値比較:
- Integer 同士は値比較。
- 同型のヒープオブジェクト同士は再帰的に値比較 (String はバイト列、
  Array は要素ごと)。
- 異なる型は常に false (`1 == "1"` → `false`、`nil == false` → `false`)。

`<`, `<=`, `>`, `>=`, `<=>` は Integer 同士 / String 同士に対応。
`<=>` の混合型は Ruby と同じく `nil` を返す。`<` 等の混合型は
runtime エラー (`stderr` に "type mismatch")。

### Integer

`+`, `-`, `*`, `/`, `%` は signed 64bit C 演算 (オーバーフロー未定義、
1 bit は LSB-tag に消費される)。`<<` は左ビットシフト。`<=>` あり。

### 論理

`&&`, `||`。**結果は `true` / `false` に正規化される** (Ruby の
「最後に真だった値を返す」とは異なる)。`if` / `while` の述語として
使うぶんには違いが見えない。

## 制御構造

### `if` / `else`

```ruby
if cond
  ...
elsif other
  ...
else
  ...
end

x if cond           # postfix
y unless cond
```

述語が **`false` (= raw 0) でないものはすべて真**。整数 0 も `[]` も
`""` も真扱い (Ruby と同じ)。

### `while`

```ruby
while cond
  ...
end

x while cond        # postfix
```

`break` / `next` / `redo` は未対応。

### 三項演算子

```ruby
cond ? a : b        # OK
```

### `return`

`def` 内の任意の位置から早期 return できる。トップレベルでの `return`
は未定義。

## メソッド (関数) 定義

```ruby
def fib(n)
  if n < 2
    1
  else
    fib(n-2) + fib(n-1)
  end
end

def add(a, b) = a + b      # endless method OK
```

引数は **位置引数のみ**。デフォルト値・キーワード引数・splat (`*args`) ・
ブロック引数 (`&blk`) はすべて未対応。`yield` も未対応。

再定義は許される (最後の `def` が勝つ)。

## メソッド呼び出し

レシーバ無し:

```ruby
fib(10)
add 1, 2
```

レシーバ有りで使えるのは下記の builtin メソッドのみ:

| 受信側 | メソッド | 引数 | 戻り値 |
|---|---|---|---|
| Array | `size` / `length` | () | Integer |
| Array | `[]` | (Integer) | element / `nil` |
| Array | `[]` | (Integer, Integer) | 部分配列 / `nil` |
| Array | `[]=` | (Integer, value) | value |
| Array | `push` | (value) | self |
| Array | `pop` | () | last element / `nil` |
| String | `size` / `length` | () | Integer (バイト長) |
| String | `[]` | (Integer) | 1 文字の String / `nil` |
| String | `[]` | (Integer, Integer) | 部分文字列 / `nil` |
| 任意 | `to_s` | () | String (Ruby `Kernel#to_s` 風) |
| 任意 | `to_i` | () | Integer (String は先頭 10 進数を解析、それ以外は 0 / 自身) |

これら以外のメソッド名 (`.each` / `.map` / `.compact` ...) は parser
から普通の関数呼び出しとして扱われ、未定義関数として失敗する。

### 文字列リテラル / interpolation

`"abc"` の他に `"#{expr}"` 形式の interpolation 対応。各 `#{expr}` は
parse 時に `expr.to_s` で String 化されて concat される (`node_add`
の `str + str` 経路)。`"#{x}"` 形式は内部的には `node_call_to_s(x)`
+ `node_str_lit("...", N)` の連結。

## ビルトイン関数

| 名前 | 引数 | 効果 |
|---|---|---|
| `p` | (any) | inspect 風に標準出力へ。`int` は数値、`String` は `"..."`、`Array` は `[...]` |
| `bf_add` | (Integer, Integer) | 動作確認用の Integer 加算 |
| `zero` | () | `0` を返す (動作確認用) |

## 未対応の Ruby 機能

実用 Ruby との大きな差分:

- **OO 機能なし** — `class` / `module` / `def` の中で `self` / `instance var`
- **block / yield / proc / lambda なし**
- **例外・`begin/rescue/ensure` なし**
- **Symbol / Hash / Range / Regexp なし**
- **`require` / `load` なし** (1 ファイルのみ)
- **stdin / IO 操作なし** (`p` だけ)
- **`each` / `map` 等の collection iteration なし** — `while` で書く

baruby は **GC を試すための最小語彙**として設計されている。Ruby らしさを
求めるなら abruby / koruby を見るべし。
