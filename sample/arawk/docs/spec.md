# arawk 言語仕様

`arawk` は **POSIX awk のサブセット**インタプリタ。awk はテキスト処理用の
ドメイン特化言語で、入力レコードをパターン-アクション規則の連鎖で処理する。
arawk は POSIX 1003.1-2017 の awk 規格に準拠する範囲 (**正規表現関連を除く**)
をカバーする。

完全な awk 仕様は
[POSIX awk](https://pubs.opengroup.org/onlinepubs/9699919799/utilities/awk.html)
や [The AWK Programming Language](https://awk.dev/) を参照。本書は arawk で
動く範囲を端的に示す。

## プログラムの構造

awk プログラムは「パターン-アクション」のリストと、`BEGIN` / `END` 特殊
ブロック、`function` 定義から成る:

```awk
function name(args) { body }            # 関数定義
BEGIN  { 初期化 }
pattern { action }                      # 各レコードで pattern が真なら action 実行
pattern                                 # action 省略時は `{ print }`
       { action }                       # pattern 省略時は全レコード
END    { 後処理 }
```

実行モデル:

1. **BEGIN** ブロックを順に実行 (入力読まず)
2. 入力ファイル / stdin から **1 レコードずつ**読む (RS = `\n` 既定)
3. 各レコードに対し、pattern-action 規則を上から順に評価:
   - pattern が真 → action 実行
4. 全レコード処理後、**END** ブロックを実行
5. `exit n` で `n` を終了ステータスとして終了

## 値の種類

| 種別 | 例 | 備考 |
|---|---|---|
| 整数 (fixnum) | `42` `-3` | 63-bit 即値 |
| 浮動小数 | `3.14` `1e10` | IEEE-754 double (heap) |
| 文字列 | `"hello"` | エスケープ: `\n \t \r \\ \" \/` |
| 文字列-数値 (strnum) | フィールド値 / `getline` の入力 | 数値形なら数値扱い、 そうでなければ文字列扱い |
| 連想配列 | `a["k"]` | スカラとは別の名前空間 |
| 未初期化 | (代入前の変数) | 数値文脈で 0、文字列文脈で `""` |

awk の値は **数値と文字列の二面性**を持つ。`"42" + 1` は `43`、`1 "x"` は
`"1x"` (concat)。フィールド値は **strnum** で、数値らしき形なら数値、そうで
なければ文字列として比較される (`"foo" == "FOO"` は文字列比較)。

## リテラル

```awk
42                  # 整数
3.14   1e10         # 浮動小数
"hello"             # 文字列 (ダブルクオートのみ; シングルは不可)
```

## 演算子

| カテゴリ | 演算子 |
|---|---|
| 算術 | `+ - * / % ^ **` |
| 単項 | `+expr` `-expr` `!expr` `++lv` `--lv` `lv++` `lv--` |
| 比較 | `< > <= >= == !=` |
| 論理 | `&& || !` |
| 文字列 | `a b` (juxtaposition → concat) |
| 三項 | `cond ? then : else` |
| 配列 | `a[k]`, `a[i, j]` (多次元キー → `i SUBSEP j` 連結), `k in a`, `delete a[k]`, `delete a` |
| 代入 | `= += -= *= /= %= ^=` |
| 行 | `$N` (フィールド), `$0` (レコード全体), `NR`, `NF` 等 |

優先順位 (低 → 高): 代入 → `?:` → `||` → `&&` → `in` → 比較 → concat → `+/-` → `*/%` → unary → `^` → `$` → primary。

## 制御構造

### `if / else`

```awk
if (cond) stmt
if (cond) stmt else stmt
```

### `while` / `do-while`

```awk
while (cond) body
do body while (cond)            # body は必ず 1 回実行
```

### `for`

```awk
for (init; cond; step) body
for (k in arr) body             # 配列のキーを順次
```

### ジャンプ

| 文 | 効果 |
|---|---|
| `break` | 最内 for/while/do-while を抜ける |
| `continue` | 同上ループの次反復へ |
| `next` | 現レコードの残り pattern-action をスキップ、次レコードへ |
| `nextfile` | 現入力ファイルの残レコードをスキップ、次のファイルへ |
| `exit [n]` | END を実行してから終了 (BEGIN/main から)、END からは即終了 |
| `return [v]` | 関数から脱出 (省略時は未初期化値) |

## 入力フィールド

| 変数 | 意味 |
|---|---|
| `$0` | 現レコード全体 (文字列) |
| `$N` | N 番目のフィールド (1-origin); 範囲外は `""` |
| `NF` | 現レコードのフィールド数 |
| `NR` | これまで読んだレコード総数 |
| `FNR` | 現入力ファイル内でのレコード番号 |
| `FILENAME` | 現入力ファイル名 (stdin なら `""`) |
| `FS` | フィールド区切り (既定 `" "` = 連続空白) |
| `OFS` | print のフィールド区切り (既定 `" "`) |
| `RS` | レコード区切り (既定 `"\n"`; 単一文字のみ対応) |
| `ORS` | print の出力レコード区切り (既定 `"\n"`) |
| `SUBSEP` | 多次元配列キーの区切り (既定 `"\034"`) |
| `CONVFMT` | 数値→文字列変換書式 (既定 `"%.6g"`) |
| `OFMT` | print の浮動小数書式 (既定 `"%.6g"`; 現状 CONVFMT と統合扱い) |
| `RSTART` / `RLENGTH` | match() の結果 (Phase 2 で有効化予定) |
| `ENVIRON` | 環境変数の連想配列 (例: `ENVIRON["HOME"]`) |
| `ARGC` / `ARGV` | コマンドライン引数 (`ARGV[0]="arawk"`, 以降が入力ファイル名) |

`$N = expr` で代入すると `$0` が OFS 区切りで再構築され、`NF` を増やすと
不足するフィールドが `""` で埋まる。`NF = N` で代入すると過剰な末尾
フィールドが捨てられる/不足分が空文字で埋められ、`$0` も再構築される。

`FS` への代入は **現レコードのフィールド分割を invalidate** する: 次に
`$N` を読むときに新しい FS で再分割される。

## 関数

### 組み込み関数

| カテゴリ | 関数 |
|---|---|
| 文字列 | `length` `length(s)` `substr(s, i [, n])` `index(s, t)` `split(s, arr [, sep])` `sprintf(fmt, ...)` `tolower(s)` `toupper(s)` |
| 数値 | `int(x)` `sin(x)` `cos(x)` `sqrt(x)` `exp(x)` `log(x)` `atan2(y, x)` `rand()` `srand([seed])` |
| I/O | `getline` 各形態 / `close(name)` / `fflush([name])` / `system(cmd)` |

POSIX の `sub` / `gsub` / `match` / `gensub` 等の **正規表現関連は未実装**
(Phase 2 で astrogre を統合して解禁予定)。

### ユーザー定義関数

```awk
function name(p1, p2,    local1, local2) {
    body
    return value
}
```

awk の慣習: 仮引数リストの末尾に **空白を多めに開けて**書いた変数は
「ローカル変数」扱い (実体は単に余分な仮引数で、呼び出し側は値を渡さない
ので未初期化のまま使える)。再帰 OK。

```awk
function fib(n) {
    if (n < 2) return n
    return fib(n - 1) + fib(n - 2)
}
BEGIN { print fib(20) }       # 6765
```

スカラは値渡し、配列は参照渡し (= 同じ実体を見る)。ただし呼び出し側で
未初期化のスカラを渡し、関数内で配列としてアクセスして auto-vivify
した場合は、配列が**関数ローカル**に閉じる (POSIX のグレーゾーン; gawk
とは挙動が一部違う)。

## I/O

### 出力

```awk
print expr_list                 # OFS 区切り、ORS 末尾
print expr_list > "file"        # ファイル上書き
print expr_list >> "file"       # 追記
print expr_list | "cmd"         # コマンドにパイプ (popen)
printf fmt, args                # 書式付き出力 (同じ redirect が使える)
```

`printf` の書式: `%d %i %u %o %x %X %c %s %f %e %E %g %G`、`*` で幅・
精度引数指定可。`%%` リテラル。

### 入力 (`getline`)

```awk
getline                    # 現入力 → $0 ($0/NR/NF/FNR/FILENAME 更新)
getline var                # 現入力 → var (NR/FNR 更新)
getline < "file"           # file → $0 (副作用は $0/NF のみ)
getline var < "file"       # file → var
"cmd" | getline            # cmd 出力 → $0
"cmd" | getline var        # cmd 出力 → var
```

戻り値: `1` (読めた) / `0` (EOF) / `-1` (I/O エラー)。

### close / fflush / system

```awk
close("file_or_cmd")    # 開いている stream を flush + close (-1 if 未開)
fflush()                # stdout + 全 output stream を flush
fflush("stdout")        # stdout のみ
fflush(name)            # その stream のみ
system("cmd")           # シェルコマンド実行 (戻り値は wait status)
```

## 例

```awk
# ファイル内の各単語の出現回数を多い順に出力
{
    for (i = 1; i <= NF; i++) w[$i]++
}
END {
    for (k in w) print w[k], k | "sort -nr"
}
```

```awk
# 列の合計と平均を計算
{ s += $3; n++ }
END {
    if (n > 0) printf "sum=%d avg=%.2f\n", s, s/n
}
```

```awk
# 別ファイルを動的に読み込む
BEGIN {
    while ((getline line < "/etc/hosts") > 0) {
        print "host:", line
    }
    close("/etc/hosts")
}
```

## 持たない / 制限

正規表現関連 (Phase 2 で `sample/astrogre` 統合により解禁予定):

- `/regex/` 形式の literal pattern
- `~` / `!~` 演算子
- `sub(re, ...)` / `gsub(re, ...)` / `match(s, re)`
- `split(s, arr, regex)` の第 3 引数の regex
- 動的 regex (`$0 ~ pat`)
- 複数文字 / regex の FS / RS

その他:

- gawk 拡張 (`gensub`, `asort`, `asorti`, `mktime`, `strftime`, `systime`, `@f()`, `(i, j) in a`)
- ARGV を mutate して入力ループを駆動する挙動 (POSIX 標準だが arawk は OPTION.input_files 駆動)
- 多バイト文字 (UTF-8 等) の正しい長さ計算 / FS 分割 (現状はバイト単位)

詳細: [`todo.md`](todo.md) / [`perf.md`](perf.md) / [`runtime.md`](runtime.md)。
