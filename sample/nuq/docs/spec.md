# spec.md — nuq 言語仕様

`nuq` は **jq クローン**。jq は JSON データの抽出・変形を行う DSL で、
入力 JSON を **フィルタ** の連鎖で変形して結果を出力する。

本書は nuq で動く構文を網羅した user-facing reference。実装詳解は
[`runtime.md`](./runtime.md)、ベンチは [`perf.md`](./perf.md)、
カバレッジは [`done.md`](./done.md)。完全な jq 言語仕様は
[jq Manual](https://jqlang.github.io/jq/manual/) を参照。

## 用語

- **フィルタ (filter)**: 入力 1 つを受け取って 0 個以上の出力を生成する
  変換。jq プログラムの基本単位。
- **パイプ `|`**: 左フィルタの各出力を右フィルタの入力に流す。
- **ジェネレータ**: 1 入力から複数値を生成するフィルタ (例: `range(5)`
  は 0〜4 を 5 つ生成)。
- **path 式**: 値の中の位置を表現する (`.foo`、`.foo.bar`、`.[N]`)。
  代入 `=` `|=` などで使う。

## 値モデル

| 種別 | 例 |
|---|---|
| `null` | `null` |
| 真偽 | `true` `false` |
| 数値 | `42` `3.14` `1e10` (内部は fixnum + IEEE-754 double) |
| 文字列 | `"hello"` (UTF-8) |
| 配列 | `[1, 2, 3]` (挿入順) |
| オブジェクト | `{"a": 1, "b": 2}` (**挿入順を保持**) |

これら以外の値型は持たない (jq 標準と同じ)。整数の精度は 62-bit fixnum
の範囲。`>2^53` の int リテラルは jq に揃えて double に降格 → 精度損失が
あり得る (`1000000000000000002` 等は decnum ビルドの jq 専用)。

## 起動

```sh
echo '{"foo":[1,2,3]}' | nuq '.foo | map(. * 2)'    # → [2,4,6]
nuq -n '[range(5)]'                                  # → [0,1,2,3,4]
nuq -r '.name' file.json                             # 出力をクオートなしで
nuq -c '.[] | .name'                                 # コンパクト出力
nuq -s '.[0]' file.json                              # slurp (全入力を 1 配列に)
nuq -R 'split(":")' /etc/passwd                       # raw 入力 (1 行 = 1 文字列)
nuq -L lib 'import "tools" as t; t::run' file.json   # module loader
```

| flag | 意味 |
|---|---|
| `-c` | コンパクト出力 |
| `-r` | 文字列を quote 無しで raw 出力 |
| `-R` | 入力 1 行 = 1 文字列 |
| `-s` | slurp (全入力を 1 配列にまとめる) |
| `-n` | 入力を読まず `null` を 1 個入力扱い |
| `-S` | object キーを sort して出力 |
| `-e` | `--exit-status` (truthy 出力なしで exit 5、エラーで 1) |
| `-L <dir>` | module 探索パス追加 (繰り返し可) |
| `--arg name v` | 文字列変数 `$name = "v"` を bind |
| `--argjson name v` | JSON 変数 `$name = (parse v)` を bind |
| `--slurpfile name f` | ファイル `f` を JSON parse して `$name` に |
| `--rawfile name f` | ファイル `f` の生データを `$name` に文字列で |
| `--seq` | RFC 7464 record-separator 出力 |
| `--indent N` | インデント幅 (default 2) |
| `--tab` | タブインデント |
| `--no-compile` | AOT 無効、interpreter のみ |
| `--dump-ast` | parse 結果を S 式で表示 |

## アクセス

```
.                      # 恒等 (入力をそのまま)
.foo                   # キー foo の値
.foo.bar               # ネスト
."my key"              # quote 付きキー
.[0]                   # 配列の N 番目 (負数 OK: -1 は末尾)
.[2:5]                 # スライス [2..5)
.[a:]                  # 末尾まで
.[:b]                  # 先頭から
.foo?                  # null-safe (キー無しを null 化)
.[]                    # **イテレート** — 配列なら全要素、object なら全値
.[]?                   # 同、scalar なら何も emit しない
..                     # **再帰** — 自分 + 全 descendants を emit
```

`.[]` と `..` はジェネレータ。後続フィルタは emit ごとに 1 回ずつ走る。

`.[expr]` は動的キー — `expr` の各 emit が 1 個のキーになる
(複数 emit なら fan-out)。

## 演算子

| カテゴリ | 演算子 |
|---|---|
| 算術 | `+ - * / %` (二項) / `-` (単項) |
| 比較 | `< > <= >= == !=` |
| 論理 | `and` / `or` / `not` (`not` は `\| not` で使う) |
| Alternative | `//` (左が `null`/`false` なら右を採る) |

jq 流の per-type 規則:

- 数値同士: 数値演算
- 文字列 + 文字列: 連結
- 配列 + 配列: 連結
- オブジェクト + オブジェクト: shallow merge (右上書き)
- 配列 - 配列: 集合差分
- オブジェクト * オブジェクト: **deep merge** (recursive)
- 文字列 * 整数: 反復
- 文字列 / 文字列: split
- `5 / 2 == 2.5` (jq 仕様で常に float)

比較順序は jq 互換: `null < false < true < 数値 < 文字列 < 配列 < オブジェクト`。
論理は short-circuit (`a and b` は a が確定した時点で b を評価しない)。

## 合成

### パイプ `|`

```
.foo | length              # foo の長さ
.users | .[] | .name        # 全 user の name
[range(5)] | reverse | .[0] # → 4
```

LHS が 0 個 emit なら RHS は 1 度も走らず全体が empty。

### コンマ `,` (fan-out)

```
.foo, .bar                  # foo の値、続いて bar の値
.[] | (.a, .b)              # 各要素の a と b を交互に
```

LHS の全 emit に続けて RHS の全 emit。

### 後付 `?` (== `try f`)

```
.foo? // "default"          # foo が無くても segfault せずに default
```

## 構築

### 配列構築 `[ ... ]`

中身フィルタの**全 emit を 1 配列にまとめて 1 個 emit**:

```
[1, 2, 3]                   # リテラル
[range(5)]                  # → [0,1,2,3,4]
[.[] | select(. > 0)]       # 正の要素だけの配列
```

### オブジェクト構築 `{ ... }`

```
{name: .username, age: .age}        # 値はフィルタ
{name, age}                          # 短縮形 = {name: .name, age: .age}
{$x}                                  # = {x: $x}
{(.key): .val}                       # 計算キー
{("foo" + "bar"): 1}                 # 同上
```

値が複数 emit なら **cartesian fan-out** (`{x: (1,2)}` → 2 個の object)。

## 制御

### 条件 `if-then-elif-else-end`

```
if .age >= 18 then "adult"
elif .age >= 13 then "teen"
elif .age >=  6 then "kid"
else "infant"
end
```

`elif` は任意段。`else` 省略時は false 分岐で input をそのまま返す。

### 例外 `try-catch`

```
try (.a / 0) catch "err"             # → "err"
try .x.y catch .                      # エラー文字列を `.` として受ける
.x.y?                                 # = try .x.y catch empty
```

`error` / `error("msg")` で例外を投げる。`catch` の本体内で `.` は
エラーメッセージ。中途で error が出た場合、先に emit 済みの値は
stdout に出てから error が surface される (jq 互換)。

### 変数束縛 `as $x`

```
.foo as $x | .bar | $x + .          # foo を保持しつつ bar をいじる
.x as [$a, $b] | $a + $b              # 配列分解
.x as {a: $a, b: $b} | $a * $b        # object 分解
.x as {$a, $b} | [$a, $b]             # = {a: $a, b: $b}
.x as {(.k): $v} | $v                 # 計算キー分解
.x as [$a, $b] ?// $c | ...           # 失敗で別パターンへ alt
```

destructuring patterns + `?//` alt が動く。

### `reduce`

```
reduce .[] as $item (0; . + $item)   # 配列の和 (= add)
reduce range(5) as $i (1; . * ($i+1))   # 5! = 120
```

書き方: `reduce <gen> as <pattern> (<init>; <update>)`。`<gen>` の各
emit に対し `<update>` で `.` (= 現在の累積値) を更新する。

### `foreach`

毎ステップ出力する版の reduce:

```
foreach .[] as $i (0; . + $i; .)   # 累積和: 1, 3, 6, ...
foreach range(5) as $x (
  null;
  if . == null then $x else . + $x end;
  .)                                 # 0, 1, 3, 6, 10
```

書き方: `foreach <gen> as <pattern> (<init>; <update>; <extract>)`。
`<extract>` 省略時は acc をそのまま emit。

### `label / break`

早期脱出:

```
label $out | .[] | if . > 100 then ., break $out else . end
```

`break $out` で対応する label まで unwind。

## ユーザ定義 `def`

```
def avg: add / length;
def factorial: if . <= 1 then 1 else . * (. - 1 | factorial) end;
def double($x): . * $x;          # value-arg ($-prefix)
def map_n(f; n): [.[range(n)] | f];   # filter-arg + filter-arg

[1, 2, 3, 4] | avg              # → 2.5
5 | factorial                    # → 120
3 | double(10)                   # → 30
[1,2,3,4] | map_n(.*., 3)        # → [1,4,9]
```

`def` の後はセミコロン区切りで複数定義を続けられる。本体の終わりに
通常のフィルタ式が来る。

### 引数の評価規則

引数の `$`-prefix で評価方式が変わる:

- **`def f($x): body`** — value-arg。call 時に $x を value として **eager
  に** 評価し bind。multi-emit なら cartesian で f を複数回起動。
- **`def f(x): body`** — filter-arg / call-by-name。call 時には**式 AST
  と caller scope を保存するだけ**。body 内で `x` を参照するたびに
  caller scope で式を再評価する。

```
def gen(x): [x, x];
10 as $a | gen($a, $a*2)
# → x は thunk `$a, $a*2` として渡る
# → body の各 x で caller scope で再評価 → 10, 20 を 2 emit ずつ
# → [10, 20, 10, 20]
```

`reduce` / `foreach` の `<gen>` も caller scope で評価される (filter-arg
と同じ)。

## モジュール

```
module {whatever};                          # 任意 metadata (modulemeta から見える)
import "stat" as s;                          # s::avg, s::stat ...
import "stat" as s {search: "./../lib/jq"};  # 探索 dir override
import "data" as $d;                         # JSON データ → $d / $d::d
include "common";                            # namespace なし注入

def total: s::stat | .sum;                   # ns 越し関数呼び出し
```

探索順:
1. `{search: "..."}` で指定された override (anchor module の dir 相対)
2. `-L` で渡された CLI 探索パス
3. CWD (`./`)

各 dir で `<rel>.jq` と `<rel>/<rel>.jq` (jq 流ディレクトリ形式) を
順に試す。データ import は `<rel>.json`。

```
modulemeta                # 入力の module relpath からメタデータを返す
                          # → {<meta>, deps:[...], defs:["name/arity", ...]}
```

`include` の後勝ち shadow ルール: `include "shadow1"; include "shadow2";`
で同名 def があれば shadow2 が優先。`import "X" as foo;` も同様に
後勝ち。

## 文字列補間 / フォーマット

### 補間

```
"hello, \(.)!"               # `.` を埋め込む
"\(.first) \(.last)"          # 複数フィールド
"x = \(.x | tostring)"        # 任意のサブフィルタ
```

`\(...)` の中で値が文字列なら **そのまま埋め込む** (raw)、それ以外は
JSON エンコードして埋め込む (jq の `tostring` semantics)。multi-emit
は cartesian fan-out。

### `@`-format

```
@text                # 文字列はそのまま、それ以外 tostring
@json                # 任意の値を JSON 文字列に (常に quote 付き)
@csv                 # 配列 → CSV 行 (`"` を `""` で escape)
@tsv                 # 配列 → TSV 行
@uri                 # RFC 3986 URI escape
@html                # HTML エンティティ
@sh                  # shell quote
@base64 / @base64d   # Base64 encode / decode
```

`@fmt` を直接 filter として使う形と、`@fmt "..."` で interpolate 中の
値を fmt 経由で escape する形 (`@uri "/v?\(.q)"`) の両方が動く。

## 組み込み関数

カテゴリ別の代表的な関数 (詳細リストは [`done.md`](./done.md))。

### 構造アクセス
`length` `utf8bytelength` `type` `keys` `keys_unsorted` `values`
`has(k)` `in(o)` `contains(v)` `paths` `paths(f)` `leaf_paths`
`getpath(p)` `setpath(p; v)` `delpaths(ps)` `del(path-expr)`

### 配列・選択
`add` `add(f)` `any` `all` `any(f)` `all(f)` `any(gen; cond)`
`all(gen; cond)` `min` `max` `min_by(f)` `max_by(f)` `unique`
`unique_by(f)` `sort` `sort_by(f)` `group_by(f)` `reverse` `flatten`
`flatten(N)` `range(N)` `range(M; N)` `range(M; N; S)` `limit(N; gen)`
`first` `last` `first(gen)` `last(gen)` `nth(N; gen)` `isempty(gen)`
`indices(v)` `index(v)` `rindex(v)`

### filter / map 系
`map(f)` `map_values(f)` `select(cond)` `to_entries` `from_entries`
`with_entries(f)` `recurse` `recurse(f)` `recurse(f; cond)` `walk(f)`
`while(c; u)` `until(c; u)`

### 文字列
`ascii_upcase` `ascii_downcase` `explode` `implode` `split(s)`
`splits(s)` `join(s)` `startswith(s)` `endswith(s)` `ltrimstr(s)`
`rtrimstr(s)` `trimstr(s)` `ltrim` `rtrim` `trim` `ascii(N)` `tojson`
`fromjson` `test(s)` `sub(s; r)` `gsub(s; r)`

### 数値
`floor` `ceil` `round` `sqrt` `fabs` `abs` `infinite` `nan` `isnan`
`isinfinite`

### 型 filter
`isnull` `nulls` `booleans` `numbers` `strings` `arrays` `objects`
`iterables` `scalars`

### I/O / 環境
`input` `inputs` `env` `now` `localtime` `gmtime` `mktime` `strftime`
`strptime` `strflocaltime` `input_filename` `debug` `stderr` `IN(s)`
`INDEX(s; idx)` `INDEX(idx)` `JOIN(...)` `pick(f)` `isvalid(f)`
`error` `error(msg)` `halt` `halt_error` `modulemeta`

### 型変換
`tostring` `tonumber` `tojson` `fromjson` `ascii` `explode` `implode`

## 代入

```
.foo = 1                 # foo を 1 に
.foo |= . + 1            # foo に f を適用
.foo += 1                # = .foo |= . + 1
.foo -= 1
.foo *= 2
.foo /= 2
.foo %= 2
.foo //= "default"       # null/false なら置換
```

LHS は `.foo` / `.foo.bar` / `.[N]` チェーンに加え、`..` 経由 (`.. |
select(P) | .b) |= F` のような recurse-then-update も動作。

`del(.foo.bar)` / `delpaths([["foo","bar"]])` で削除。

## 例

```sh
# user の name 一覧
nuq '[.users[].name]' users.json

# active かつ age > 30 の人を絞り込む
nuq '[.users[] | select(.active and .age > 30)]'

# 都市別に集計
nuq 'group_by(.city) | map({city: .[0].city, count: length})'

# CSV 出力
nuq -r '.users[] | [.name, .email, .age] | @csv'

# 平均 + 合計
nuq 'def avg: add / length;
     def stats: {sum: add, avg: avg};
     .scores | stats'

# 再帰展開で全数値 leaf を集める
nuq '[.. | numbers]'

# 各 user の name を大文字化
nuq '.users[].name |= ascii_upcase'

# 全 array に対して bottom-up sort
nuq 'walk(if type == "array" then sort else . end)'

# モジュール経由
nuq -L lib 'import "report" as r; .data | r::summarize'
```
