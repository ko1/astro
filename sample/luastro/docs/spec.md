# luastro 言語仕様

`luastro` は **Lua 5.4** のサブセットインタプリタ。Lua は軽量・組込み向けの
動的型付き言語で、テーブル (連想配列) を中心とした単純な構造を持つ。luastro
は Lua 5.4 リファレンスマニュアルの主要部分を実装する: 全字句構文・全文法・
整数/浮動小数の subtype・クロージャ・メタテーブル・標準ライブラリの主要部
(math/string/table/io.write/os/coroutine)・Lua パターンマッチ・弱テーブル・
`__gc` ファイナライザ・ucontext ベースのコルーチン。

完全な Lua 5.4 仕様は [Lua 5.4 Reference Manual](https://www.lua.org/manual/5.4/)
を参照。

## 値の種類 (8 つの基本型)

| 型 | 例 | 備考 |
|---|---|---|
| `nil` | `nil` | 値なし。**真偽判定で偽** |
| `boolean` | `true` `false` | |
| `number` (integer) | `42` `-3` | 64-bit 整数 |
| `number` (float) | `3.14` `1e10` | IEEE-754 double。整数と相互運用 |
| `string` | `"hello"` `'world'` `[[多行]]` | immutable、intern される |
| `table` | `{1, 2, 3}` `{a=1, b=2}` | **唯一の集合型** (配列 + 辞書) |
| `function` | `function(x) return x+1 end` | 第一級 |
| `userdata` | (C 拡張用) | luastro では限定的 |

(`thread` (coroutine) はあるが上の表では function に近い扱い。)

**真偽判定**: `false` と `nil` だけが偽。`0` や `""` は真。

## リテラル

```lua
nil       true   false
42        0xff   1e10        -- 整数 / 浮動小数
"hello"   'world'             -- 文字列
[[
multi-line
string
]]                            -- 長文字列 (改行含む)
{1, 2, 3}                     -- 配列スタイル
{name = "alice", age = 30}    -- レコードスタイル
{[expr] = val}                -- 任意キー
```

## 変数とスコープ

```lua
local x = 10               -- ローカル変数 (推奨)
y = 20                     -- グローバル変数 (推奨されない)
```

`local` を付けないと自動的にグローバル変数になる (Lua の慣習)。
スコープはブロック単位 (関数・`do...end`・`while`/`for`/`if` の本体)。

## 演算子

| カテゴリ | 演算子 |
|---|---|
| 算術 | `+ - * / // % ^` (`//` 整数除算、`^` 冪) |
| 単項 | `-` `not` `#` (長さ) |
| 比較 | `< > <= >= == ~=` (`~=` は不等) |
| 論理 | `and or not` (短絡; 値を返す) |
| 連結 | `..` (文字列連結) |
| ビット (5.3+) | `& \| ~ << >> ~` (Lua 5.4 で追加) |

`and` / `or` は **値を返す**:

```lua
x = a or default     -- a が真なら a、偽なら default
x = cond and v1 or v2  -- 条件演算子の代用 (cond, v1 が共に真ならば v1)
```

## 制御構文

### `if`

```lua
if cond then
  ...
elseif other then
  ...
else
  ...
end
```

### `while` / `repeat`

```lua
while cond do
  ...
end

repeat
  ...
until cond                    -- until 条件 (do-while と逆: until 真で抜ける)
```

### 数値 `for`

```lua
for i = 1, 10 do      -- 1, 2, ..., 10
  print(i)
end

for i = 10, 1, -1 do  -- 10, 9, ..., 1 (3 つ目は刻み)
  print(i)
end
```

### ジェネリック `for`

```lua
for k, v in pairs(tbl) do      -- 全キー/値 (順序不定)
  print(k, v)
end

for i, v in ipairs(arr) do     -- 配列部分 (1, 2, 3, ... 連続整数)
  print(i, v)
end
```

### `break` / `goto`

```lua
for i = 1, 10 do
  if i == 5 then break end
end

for i = 1, 10 do
  if i % 2 == 0 then goto continue end
  print(i)
  ::continue::
end
```

(Lua には `continue` は無く、`goto ::label::` で代用するのが慣習。)

## 関数

```lua
function add(a, b)
  return a + b
end

local f = function(x) return x * 2 end           -- 無名関数

function multi() return 1, 2, 3 end              -- 多値返却
local a, b = multi()                              -- 多値受取

function variadic(...)
  local args = {...}
  return #args                                    -- 可変長引数の数
end
```

- 関数は第一級 (変数に代入・引数として渡せる・戻り値にできる)。
- **多値返却** が言語機能 (タプルではない、独立した値の列)。
- **クロージャ**: 外側のローカル変数を捕獲する。

```lua
function counter()
  local n = 0
  return function() n = n + 1; return n end
end
local c = counter()
print(c(), c(), c())     -- 1   2   3
```

メソッド構文:

```lua
function obj:greet(name)        -- self を暗黙の第 1 引数として受け取る
  print("hi " .. name .. " from " .. self.id)
end

obj:greet("alice")              -- = obj.greet(obj, "alice")
```

## テーブル

Lua の唯一の集合型。配列 (1-origin) と辞書を兼ねる:

```lua
local t = {}
t[1] = "a"           -- 配列スタイル
t[2] = "b"
t.name = "alice"     -- = t["name"]、辞書スタイル
t["3.14"] = "pi"     -- 任意キー

#t                   -- 配列部分の長さ (連続整数キーの数)
t.name               -- "alice"
t[1]                 -- "a"

-- リテラル
local arr = {10, 20, 30}                          -- arr[1]=10, arr[2]=20, ...
local rec = {name="bob", age=25}                  -- rec.name="bob"
local mixed = {1, 2, foo="bar", [10]="deep"}     -- 混在可
```

### イテレーション

```lua
for i, v in ipairs(arr) do ... end   -- 配列部分のみ、1 から
for k, v in pairs(tbl) do ... end    -- 全キー/値、順序不定
```

## メタテーブル

テーブルに「演算時の振る舞い」を後付けで指定する仕組み。`setmetatable(t, mt)` で設定:

```lua
local v = {x=1, y=2}
local mt = {
  __add = function(a, b) return {x=a.x+b.x, y=a.y+b.y} end,
  __tostring = function(a) return "("..a.x..","..a.y..")" end,
  __index = function(t, k) return "default" end,
}
setmetatable(v, mt)

local w = v + v               -- mt.__add が呼ばれる → {x=2, y=4}
print(tostring(w))             -- "(2,4)"
print(v.unknown)               -- "default" (__index で fallback)
```

主なメタメソッド: `__add` `__sub` `__mul` `__div` `__mod` `__pow` `__unm`
`__concat` `__len` `__eq` `__lt` `__le` `__index` `__newindex` `__call`
`__tostring` `__gc`。

`__index` には**関数**と**テーブル**の両方を渡せる (テーブルなら継承的に検索)。

## 文字列

immutable で intern される。`..` で連結、`#s` で長さ。

```lua
"abc" .. "def"                 -- "abcdef"
#"hello"                       -- 5
string.upper("abc")            -- "ABC"
string.sub("hello", 2, 4)      -- "ell" (1-origin、両端含む)
string.format("%d / %.2f", 42, 3.14159)
string.find("hello", "l+")     -- 3, 4
string.gsub("aaa", "a", "b")   -- "bbb", 3
"hello":upper()                -- メソッド呼出 (= string.upper("hello"))
```

### Lua パターン (regex の Lua 方言)

正規表現とは別の独自記法。`%d` `%w` `%s` などのクラス + 量指定子 (`* + - ?`)
+ アンカー (`^ $`)。詳細は Lua マニュアル 6.4.1。

```lua
string.match("name=alice", "(%w+)=(%w+)")    -- "name", "alice"
string.gmatch("a b c", "%S+")()              -- "a"  (iterator)
```

## 例外 — `error` / `pcall`

```lua
function div(a, b)
  if b == 0 then error("zero divide") end
  return a / b
end

local ok, result = pcall(div, 1, 0)
if not ok then
  print("error:", result)
end
```

`pcall(f, args...)` は f を呼び、エラーなら `false, message`、成功なら `true, return-vals` を返す。

## コルーチン (ucontext ベース)

```lua
local co = coroutine.create(function(a)
  print("started:", a)
  local x = coroutine.yield(a * 2)    -- 一旦戻る
  print("resumed:", x)
  return "done"
end)

print(coroutine.resume(co, 10))      -- "started: 10"  /  true, 20
print(coroutine.resume(co, 100))     -- "resumed: 100" /  true, "done"
```

## 標準ライブラリ (実装済みの主なもの)

`math` — `math.pi math.sqrt math.floor math.ceil math.abs math.max math.min
math.random math.sin math.cos math.log math.exp math.huge`

`string` — `string.len string.upper string.lower string.sub string.rep
string.reverse string.byte string.char string.format string.find string.match
string.gmatch string.gsub`

`table` — `table.insert table.remove table.concat table.sort table.unpack`

`io.write` `io.read` `os.time os.clock os.date os.exit`

`coroutine.create coroutine.resume coroutine.yield coroutine.status`

`pairs ipairs next select unpack(=table.unpack) tonumber tostring type print
assert error pcall xpcall setmetatable getmetatable rawget rawset rawequal`

## 例

```lua
-- フィボナッチ
local function fib(n)
  if n < 2 then return n end
  return fib(n-1) + fib(n-2)
end
print(fib(20))    -- 6765

-- テーブルベースのオブジェクト
local Counter = {}
Counter.__index = Counter

function Counter.new(start)
  return setmetatable({n = start}, Counter)
end

function Counter:inc()  self.n = self.n + 1  end
function Counter:get()  return self.n end

local c = Counter.new(10)
c:inc(); c:inc()
print(c:get())   -- 12

-- コルーチンでジェネレータ
local function gen(n)
  for i = 1, n do coroutine.yield(i*i) end
end
for v in coroutine.wrap(function() gen(5) end) do
  io.write(v, " ")
end                       -- 1 4 9 16 25
```

## 持たない / 制限

- `require` / `package.path` (複数ファイル管理)
- `debug` ライブラリ
- `dofile` / `loadfile` / `load` の動的読み込み
- C extensions / FFI

詳細: [`done.md`](done.md) / [`todo.md`](todo.md)。
