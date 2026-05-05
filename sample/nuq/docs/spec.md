# nuq 言語仕様

`nuq` は **jq クローン**。jq は JSON データの抽出・変形を行う DSL (専用
言語) で、シェルパイプライン的な発想を持つ — 入力 JSON を **フィルタ**
の連鎖で変形し、結果の JSON を出力する。

完全な jq 言語仕様は [jq Manual](https://jqlang.github.io/jq/manual/) を
参照。本書は nuq で動く範囲を端的に示す。

## 用語

- **フィルタ (filter)**: 入力 1 つを受け取って 0 個以上の出力を生成する変換。jq プログラムの基本単位。
- **パイプ (pipe) `|`**: 左フィルタの各出力を右フィルタの入力として渡す。
- **`.` (アイデンティティ)**: 入力をそのまま出力する基本フィルタ。
- **ジェネレータ**: 1 入力から複数値を生成するフィルタ (例: `range(5)` は 0〜4 を 5 つ生成)。

## 値 (JSON 標準 + 内部)

| 種別 | 例 |
|---|---|
| `null` | `null` |
| 真偽 | `true` `false` |
| 数値 | `42` `3.14` (内部は fixnum + double) |
| 文字列 | `"hello"` |
| 配列 | `[1, 2, 3]` |
| オブジェクト | `{"a": 1, "b": 2}` |

これら以外の値型は持たない (jq 標準と同じ)。

## 起動と入出力

```sh
echo '{"foo": [1,2,3]}' | nuq '.foo | map(. * 2)'    # → [2,4,6]
nuq -n '[range(5)]'                                  # → [0,1,2,3,4]
nuq -r '.name' file.json                             # 出力をクオートなしで
nuq -c '.[] | .name'                                 # コンパクト出力
nuq -s '.[0]' file.json                              # slurp: 全入力を配列にして渡す
nuq -R 'split(":")' /etc/passwd                       # raw 入力 (1 行 = 1 文字列)
```

| フラグ | 意味 |
|---|---|
| `-c` | コンパクト出力 (改行/インデント抑制) |
| `-r` | 文字列出力時にクオートを付けない |
| `-R` | 入力を 1 行 1 文字列として読む |
| `-s` | 全入力を配列にまとめて 1 個の入力として渡す |
| `-n` | 入力を読まず `null` を 1 個入力扱い |
| `--indent N` | インデント幅 |

## 基本フィルタ

### `.` (アイデンティティ)

入力をそのまま出力。プログラムの起点になる:

```sh
echo '42' | nuq '.'           # 42
echo '{"a":1}' | nuq '.'      # {"a":1}
```

### フィールド/添字アクセス

```
.foo                       # キー foo の値
.foo.bar                   # ネスト
.["my key"]                # クオート付きキー
.[0]                       # 配列の先頭
.[2:5]                     # スライス [2..5)
.foo?                      # null 安全 (キー無しでも null を返す)
.[]                         # **イテレート** — 配列/オブジェクトを 1 要素ずつ流す
```

`.[]` は配列なら全要素、オブジェクトなら全値を **個別の出力** として
吐き出す。後続フィルタは各値ごとに 1 回ずつ走る。

## 演算子

| カテゴリ | 演算子 |
|---|---|
| 算術 | `+ - * / %` |
| 比較 | `< > <= >= == !=` |
| 論理 | `and or not` (`not` はメソッドのように `\| not` で使う) |
| Alternative | `//` (左が `null`/`false` なら右を採る) |
| 文字列 | `+` (連結) |

```
. + 1                      # 入力 + 1
.a + .b                    # 2 つのフィールドの和
.x // "default"            # null/false の代わり
```

## パイプとカンマ

### パイプ `|`

左の出力を右の入力にする:

```
.foo | length              # foo の長さ
.users | .[] | .name       # 全 user の name
```

### カンマ `,` (fan-out)

複数のフィルタを並列に評価し、結果を順に流す:

```
.foo, .bar                  # foo の値、その後 bar の値
echo '{"a":1,"b":2}' | nuq '.a, .b'   # 1 行目: 1, 2 行目: 2
```

## 構築

### 配列構築 `[ ... ]`

中身のフィルタの **全出力を 1 つの配列に集める**:

```
[1, 2, 3]                  # リテラル
[range(5)]                 # → [0,1,2,3,4]
[.[] | select(. > 0)]      # 正の要素だけの配列
```

### オブジェクト構築 `{ ... }`

```
{name: .username, age: .age}        # 値はフィルタ
{name, age}                          # 短縮形 (= {name: .name, age: .age})
{(.key): .val}                       # 計算キー
```

## 制御構造

### 条件 `if-then-elif-else-end`

```
if .age >= 18 then "adult" elif .age >= 13 then "teen" else "child" end
```

### 例外 `try-catch`

```
try (.a / 0) catch "err"             # → "err" (エラーを文字列に置換)
.a // empty                           # null なら何も出力しない
```

`error("msg")` で例外を投げる。`catch` の本体内で `.` は例外メッセージ。

### 変数束縛 `as $x`

```
.foo as $x | .bar | $x + .          # foo を保持しつつ bar をいじる
```

`as` は **左の各出力をスコープ内の `$x` に束縛**してから右を評価。

### `reduce`

集約。アキュムレータパターン:

```
reduce .[] as $item (0; . + $item)   # 配列の和 (= add)
```

書き方: `reduce <gen> as $var (<init>; <update>)`。`<gen>` の各出力に
対し、`<update>` で `.` (= 現在の累積値) を更新する。

### `foreach`

各ステップを出力する版の reduce:

```
foreach .[] as $item (0; . + $item)
# 累積和: 1 → 3 → 6 → ... を順に出す
```

書き方は reduce と同じだが **毎ステップ出力** される。

### `label / break`

早期脱出:

```
label $out | .[] | if . > 100 then ., break $out else . end
```

## ユーザ定義 `def`

```
def avg: add / length;
def factorial: if . <= 1 then 1 else . * (. - 1 | factorial) end;
def double($x): . * $x;        # パラメータ付き

[1, 2, 3, 4] | avg              # → 2.5
5 | factorial                    # → 120
3 | double(10)                   # → 30
```

`def` 後はカンマ ではなくセミコロンで区切る。フィルタ末尾に他の式を続けて書く。

## 文字列補間

```
.name | "hello, \(.)!"           # \(.) で式を埋め込む
"\(.first) \(.last)"
```

## フォーマット (`@`)

特定の出力エンコーディングに変換:

```
@csv                 # 配列を CSV 行に
@tsv                 # 配列を TSV 行に
@json                # 任意の値を JSON 文字列に
@text                # 文字列はそのまま、他は @json と同様
@uri                 # URI エンコード
@html                # HTML エスケープ
@sh                  # shell quote
@base64              # Base64 エンコード
@base64d             # Base64 デコード
```

```
[1, "ab,cd", 3] | @csv           # "1,\"ab,cd\",3"
"hello world" | @uri              # "hello%20world"
```

## 組み込み関数 (70+)

### 構造アクセス

```
keys    keys_unsorted    values    has(key)    in(obj)    contains(value)
length    type    empty
.[]   .[]?    paths    leaf_paths    getpath(p)    setpath(p; v)    delpaths(ps)
```

### 配列・選択

```
add    any    all    min    max    min_by(f)    max_by(f)
unique    unique_by(f)    sort    sort_by(f)    group_by(f)
reverse    flatten    flatten(depth)    range(n)    range(from;to)    range(from;to;step)
limit(n; gen)    first    first(gen)    last    last(gen)    nth(n; gen)
indices(v)    index(v)    rindex(v)
```

### filter/map 系

```
map(f)              # = [.[] | f]
map_values(f)       # オブジェクト各値に f
select(cond)        # cond が真の入力だけ通す
to_entries          # {a:1, b:2} → [{key:"a",value:1}, ...]
from_entries        # 逆
with_entries(f)     # = to_entries | map(f) | from_entries
recurse             # 再帰展開 (.. と類似)
recurse(f)          # 任意のフィルタで展開
```

### 文字列

```
length    ascii_downcase    ascii_upcase    explode    implode
split(sep)    join(sep)    test(re)    match(re)    capture(re)    scan(re)    sub(re; replacement)    gsub(re; replacement)
ltrimstr(s)    rtrimstr(s)    startswith(s)    endswith(s)
ascii    tojson    fromjson
```

(注: 正規表現の対応は限定的 — jq の `test`/`match` 互換だが、複雑な
パターンは未対応の場合あり。)

### 数値

```
floor    ceil    round    sqrt    pow(x; y)    log    log10    exp
```

### 型変換

```
tonumber    tostring    ascii    tojson    fromjson
```

### その他

```
error(msg)    halt    halt_error(code)
input    inputs    null
debug    debug(msg)
```

## 例

```sh
# JSON ファイルから user の name 一覧を取得
nuq '[.users[].name]' users.json

# active かつ age > 30 の人を絞り込む
nuq '[.users[] | select(.active and .age > 30)]'

# 都市別に集計
nuq 'group_by(.city) | map({city: .[0].city, count: length})'

# CSV 出力
nuq -r '.users[] | [.name, .email, .age] | @csv'

# 平均と合計
nuq 'def avg: add / length; def stats: {sum: add, avg: avg}; .scores | stats'

# 再帰展開で全 leaf を集める
nuq '[.. | numbers]'

# パスを変える
nuq '.users[] |= (.name |= ascii_upcase)'        # 各 user の name を大文字化

# reduce + alternative
nuq 'reduce .[] as $x (0; . + ($x.score // 0))'
```

## 持たない / 制限

- `path(...)` の本格利用 (一部のみ)
- `walk(f)` の完全実装
- 一部の高度な正規表現機能
- jq 1.7+ で導入された言語拡張 (`?//` `&` 等)
- 環境変数アクセス `env.NAME`
- I/O アクセス系 (`input` / `inputs` は限定対応)
- SQL 風機能 (`INDEX` など)

詳細: [`done.md`](done.md) / [`todo.md`](todo.md)。
