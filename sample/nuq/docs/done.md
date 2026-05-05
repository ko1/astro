# done.md — nuq 実装済み

ASTro 上の jq サブセット。本書は **動くフィルタ言語機能と組み込み関数**
を一覧する。未実装は [todo.md](./todo.md)、ランタイム解説は
[runtime.md](./runtime.md)、計測は [perf.md](./perf.md)。

## テストスイート

```
$ make test
... (test runner output) ...
passed=338  failed=0  total=338
```

うち約半数 (`*.diff.test` 群) は **system の `jq` 自体を oracle として
期待出力を計算** する微分テスト。jq との挙動差はその場で検出される。

## 値モデル

- 整数: 62-bit fixnum (1-bit タグ)
- 浮動小数: ヒープ box (`struct nuq_obj` の `dbl`)
- 文字列 / 配列 / オブジェクト: ヒープ box、Boehm GC
- `null` / `true` / `false`: 静的 singleton (`NUQ_NULL_OBJ` 等)
- オブジェクトは **挿入順を保持** (parallel `keys[]` / `vals[]`)
- ハッシュ表ではないので lookup は線形 — n が小さい jq 用途では問題なし

## フィルタ言語

### 識別子 / リテラル
- `.` (恒等)
- `..` (recurse — 自分 + すべての descendants)
- `null` / `true` / `false`
- 整数 / 浮動小数 (jq 互換: `1e10`, `-0.5` ほか)
- 文字列 `"..."` (`\n \t \r \b \f \" \\ \/ \uXXXX` エスケープ + サロゲートペア)
- 配列リテラル `[expr]` / 空配列 `[]`
- オブジェクトリテラル `{...}` — `{a: e}`、shorthand `{a}` `{$a}`、
  computed key `{(expr): e}`、`{@fmt "..."} ` キーも可。値は **fan-out**
  (`{x: (1,2)}` → 2 個のオブジェクト)

### アクセス
- 静的フィールド: `.foo`、`.foo.bar`、`."key"`、`.foo?` (型不一致を捨てる)
- 動的インデックス: `.[expr]`、`.[expr]?` (キーが stream なら fan-out)
- 反復: `.[]`、`.[]?`
- スライス: `.[a:b]`、`.[a:]`、`.[:b]`、配列・文字列両対応、負数 OK

### 合成
- パイプ `f | g` — fan-out 対応 (`f` の各 emit が `g` の input になる)
- コンマ `f, g` — `f` の全 emit に続いて `g` の全 emit
- 後付 `?` (== `try f`)

### 算術 / 比較 / 論理
- `+ - * / %` — jq 流の per-type 規則:
  - 数値同士は数値
  - 文字列 + 文字列 = 連結
  - 配列 + 配列 = 連結
  - オブジェクト + オブジェクト = 右上書き shallow merge
  - 配列 - 配列 = 左から右の要素を除去
  - オブジェクト * オブジェクト = **deep merge** (再帰オブジェクト融合)
  - 文字列 * 整数 = 反復
  - 文字列 / 文字列 = split
- 単項 `-`
- 比較 `==` `!=` `<` `<=` `>` `>=`、jq の型順序
  `null < false < true < number < string < array < object`
- 論理 `and` / `or` / `not` — short-circuit (LHS が確定時 RHS を評価しない)
- 代替 `f // g` — `f` が truthy emit を 1 個も出さなければ `g`

### 構築
- 配列構築 `[body]` — body の全 emit を 1 配列に集める
- オブジェクト構築 — key/value の cartesian fan-out: 各 entry が複数値を
  emit したらすべての組合せでオブジェクトを emit

### 変数 / 関数
- 束縛 `expr as $x | body` (`expr` の各 emit ごとに `$x` をバインドして
  body を回す)
- 参照 `$x`
- ユーザ定義 `def name: body;`、引数あり `def name(f; g): body;`、
  値引数 `def name($v): body;`、複数定義の連鎖 `def a: ...; def b: ...; rest`、
  shadowing は **新しい順** (内側が外側を隠す)

### 制御
- `if cond then T else E end`、`else` 省略時は input そのまま
- `if cond then T elif cond2 then T2 else E end` (1 段の elif サポート)
- `try f catch g` (`g` 省略可、その場合エラーを呑む)
- `f?` (== `try f`)
- `error` / `error(msg)`
- `empty` (何も emit しない)
- `label $name | body` / `break $name`
- `reduce SRC as $x (INIT; UPDATE)` — INIT を入力に評価して acc 初期化、
  SRC の各 emit ごとに `$x` を bind して acc を UPDATE で更新、最後に
  acc を 1 個 emit
- `foreach SRC as $x (INIT; UPDATE; EXTRACT)` — UPDATE のたびに EXTRACT
  を回して emit。EXTRACT 省略時は acc をそのまま emit

### 文字列補間 / フォーマット
- `"prefix \(expr) middle \(expr2) suffix"` — `\(...)` の中は完全な
  サブフィルタ。fan-out 対応 (各 stream の cartesian)。
- `@text` — 文字列ならそのまま、それ以外 `tojson`
- `@json` — `tojson`
- `@csv` — 配列を CSV 行に (`"` を `""` で escape)
- `@tsv` — 配列を TSV 行に (`\t \r \n \\` を escape)
- `@uri` — RFC 3986 unreserved 以外を `%HH` に
- `@html` — `< > & ' "` を HTML エンティティに
- `@sh` — シングルクォートで囲んで shell エスケープ
- `@base64` / `@base64d` — Base64 encode / decode

## 組み込み関数 (70 余り)

### 0 引数
| グループ | 名前 |
|---|---|
| メタ | `length` `type` `keys` `keys_unsorted` `values` `empty` `not` |
| 変換 | `tostring` `to_string` `tonumber` `tojson` `fromjson` `explode` `implode` |
| 文字列 | `ascii_upcase` `ascii_downcase` `ascii` `reverse` |
| 集合演算 | `sort` `unique` `add` `min` `max` `to_entries` `from_entries` `paths` |
| 数値 | `floor` `ceil` `round` `fabs` `abs` `sqrt` |
| シーケンス | `first` `last` `any` `all` |
| 判定 | `isnan` `isinfinite` `infinite` `nan` `isnull` |
| 環境 | `now` `env` `input_filename` |
| エラー | `error` `recurse` |

### 1 引数
| 名前 | 意味 |
|------|------|
| `select(f)` | `f` が truthy emit を出すなら input を emit |
| `map(f)` | `[.[] \| f]` 等価 |
| `map_values(f)` | 配列・オブジェクトの値を `f` で写像 (構造保持) |
| `with_entries(f)` | `to_entries \| map(f) \| from_entries` 等価 |
| `has(k)` | キー存在判定 (object/array 両対応) |
| `in(o)` | input が o のキーかどうか |
| `contains(v)` | 等価判定 (再帰版でなく eq ベース、v0 簡略) |
| `range(N)` | `0..N-1` を emit |
| `split(s)` | 文字列 split |
| `join(s)` | 配列 join (null は空文字、非文字列は tostring) |
| `startswith(s)` `endswith(s)` | 接頭・接尾判定 |
| `first(f)` `last(f)` | f の最初 / 最後の emit |
| `sort_by(f)` | 安定ソート、key を `f` で計算 |
| `group_by(f)` | キー値で groupby |
| `unique_by(f)` | key で重複除去 |
| `min_by(f)` `max_by(f)` | キー最小・最大 |
| `getpath(p)` | path 配列で index 連鎖 |
| `indices(s)` `index(s)` | 文字列内位置の全列挙 / 最初 |
| `test(s)` | substring 判定 (v0 simplified、real regex は todo) |

### 2 引数
| 名前 | 意味 |
|------|------|
| `range(M; N)` | `M..N-1` |
| `limit(N; f)` | 先頭 N 個まで emit (負の数はエラー) |
| `nth(N; f)` | N 番目の emit (0-based) |

### 3 引数
| 名前 | 意味 |
|------|------|
| `range(M; N; S)` | step S での range |

## CLI

| flag | 意味 |
|------|------|
| `-c` | コンパクト出力 |
| `-r` | raw 文字列出力 |
| `-R` | raw 文字列入力 (各行を文字列入力に) |
| `-s` | slurp (全入力を 1 配列にまとめる) |
| `-n` | null input |
| `-S` | sort_keys (受け取るが現状未配線) |
| `--indent N` | インデント幅 |
| `--no-compile` | Code Store 無効化 |
| `--no-specialize` | SD 生成無効化 |
| `--quiet` | 診断出力抑制 |
| `--dump-ast` | AST を dump |
| `--help` | help |

## ASTro / Code Store

- `INIT()` で `astro_cs_init("code_store", ".", 0)` を呼ぶ。
- 起動時にフィルタ式を 1 個の AST にコンパイルし、`astro_cs_compile` →
  `astro_cs_build` → `astro_cs_reload` で SD を生成して dlopen。
- インタプリタ専用に絞るなら `--no-compile`。
- ccache + sandbox の問題が出る環境では `CCACHE_DISABLE=1` で回避
  (project memory: `feedback_ccache_disable`)。

## バグ修正履歴 (v0)

実装中に **jq との差分テスト** が拾った非自明バグ:

- `values` 組み込みは `select(. != null)` 等価 (オブジェクトの値配列を
  返すのは別の builtin)。最初は誤実装していて、`values` を
  `{"a":1,"b":2}` に対して `[1,2]` を返していた。
- 文字列補間 `"\(.)"` で値が文字列の場合は raw を埋める (`tostring`
  semantics)。最初は無条件 `tojson` していて `"world"` → `"\"world\""`
  だった。
- `try (1, error("x"), 3) catch "k"` で 1 が emit された後にエラーを
  catch する必要がある。最初は body の emit を一度配列に貯めてから
  return code を見ていて、エラー時に貯めた emit を捨てていた。
  → 直接 caller の emit_buf に流し込むように変更。
- `reduce SRC as $x (...)` の `as $x` が `parse_postfix` の通常 `as` 経路に
  食われていた。`reduce/foreach` の SRC 専用に `parse_term_for_keyword`
  (postfix だが `as` で停止) を分離。
- CLI で `-5` のような負数リテラルがフィルタに使えない (option として
  reject される) 問題。未知の `-...` 引数はフィルタとして扱うように。
- オブジェクト値の中で pipe が使えない問題 (`{a: f | g}`)。最初は
  `parse_alt` で読んでいたが、jq は pipe まで許すので
  `parse_pipe_no_comma` を追加した。
