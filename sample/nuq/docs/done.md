# done.md — nuq 実装範囲スナップショット

ASTro 上の jq 実装。本書は **動く範囲** の checklist。
言語仕様 → [`spec.md`](./spec.md)、実装詳解 → [`runtime.md`](./runtime.md)、
ベンチ → [`perf.md`](./perf.md)、残作業 → [`todo.md`](./todo.md)。

## 互換スコア

```
$ make jqtest
Loaded 526 tests from /tmp/claude/jq.test
=== jq official test results ===
  total:   526
  pass:    524  (99.6%)
  fail:    2
```

残る 2 件は **decnum (gmp ベースの任意精度 10 進数) 必須** のテスト。
IEEE-754 double では原理的に通せない:

- **#128**: 巨大指数リテラル `9E+999999999` の保持 (リテラル文字列を
  symbolic に保つ必要がある)
- **#457**: `1000000000000000002` (> 2^53 の整数) の精度保持

jq 本体も decnum ビルドでないと通らない領域。

加えてローカルテストが `make test` で 338/338 PASS。うち約半数
(`*.diff.test` 群) は **system の `jq` 自体を oracle として** 期待出力を
計算する微分テストで、jq との挙動差は実行時に検出される。

## 値モデル

- 整数: 62-bit fixnum (1-bit タグ)
- 浮動小数: ヒープ box (`struct nuq_obj` の `dbl`)
- 文字列 / 配列 / オブジェクト: ヒープ box、Boehm GC
- `null` / `true` / `false`: 静的 singleton
- オブジェクトは **挿入順を保持** (parallel `keys[]` / `vals[]`)
- 16 keys 超で **lazy hash idx** を build (open-addressing FNV-1a、
  load factor ≤ 0.5)。lookup は実質 O(1)、挿入順イテレーションは
  parallel array 側でそのまま

## フィルタ言語

### 識別子 / リテラル
- `.` (恒等) / `..` (recurse — 自分 + 全 descendants)
- `null` / `true` / `false`
- 整数 / 浮動小数 (jq 互換: `1e10`, `-0.5`, `0.5e-3`, `Infinity`,
  `-Infinity`, `NaN`)
- 文字列 `"..."` (`\n \t \r \b \f \" \\ \/ \uXXXX` + サロゲートペア)
- 配列リテラル `[expr]` / 空配列 `[]`
- オブジェクトリテラル: `{a: e}`、shorthand `{a}` / `{$a}`、
  computed key `{(expr): e}`、`{@fmt "..."}` キー、複数 emit の
  cartesian fan-out

### アクセス
- 静的フィールド: `.foo` / `.foo.bar` / `."key"`
- null-safe: `.foo?`
- 動的インデックス: `.[expr]` (キーが stream なら fan-out)
- 反復: `.[]` / `.[]?`
- スライス: `.[a:b]` / `.[a:]` / `.[:b]` (配列・文字列両対応、負数 OK、
  codepoint 単位)

### 合成
- パイプ `f | g` — fan-out 対応
- コンマ `f, g` — 連結 emit
- 後付 `?` (== `try f`)
- alternative `f // g` — `f` が truthy emit を 1 個も出さなければ `g`

### 算術 / 比較 / 論理
- `+ - * / %` — jq 流の per-type 規則 (数値、文字列連結、配列連結、
  オブジェクト merge / deep merge、文字列反復、文字列 split、
  modulo は jq 互換の inf 扱い)
- 単項 `-`
- 比較 `==` `!=` `<` `<=` `>` `>=`、jq の型順序
- 論理 `and` / `or` / `not` (short-circuit)

### 構築
- 配列構築 `[body]`
- オブジェクト構築 — 値 / 計算キーの cartesian fan-out

### 変数 / pattern
- `expr as $x | body`
- 配列分解 `as [$a, $b, ...]`
- オブジェクト分解 `as {a: $a, b: $b}` / `{$a, $b}` / `{(.k): $v}`
- パターン交替 `as P1 ?// P2 ?// ... | body` (失敗で次の P へ)
- 参照 `$x`
- `$__loc__` 疑似変数

### ユーザ定義 `def`
- 0-arity: `def name: body;`
- 引数あり: `def f(g; h): body;` (filter-arg、call-by-name closure)
- 値引数: `def f($v): body;` (eager value-arg、cartesian fan-out)
- 複数定義の連鎖
- shadowing: 内側が外側を隠す
- **call-by-name closure**: `def f(x): ...` の `x` は caller scope の
  thunk として渡る。各参照で thunk body を caller scope で再評価。
  `def f(x): [$x, x, x]; f(1, 2)` のような generator 渡し / `$` shadow
  チェーンも正しく解決

### 制御
- `if cond then T elif ... else E end` (任意段の elif)
- `try f catch g` (`g` 省略可、その場合エラーを呑む)
- `f?` (== `try f catch empty`)
- `error` / `error(msg)`
- `empty`
- `label $name | body` / `break $name`
- `reduce SRC as PAT (INIT; UPDATE)`
- `foreach SRC as PAT (INIT; UPDATE; EXTRACT)` (EXTRACT 省略可)

### Path 操作
- `path(expr)` — path 配列を emit
- `paths` / `paths(f)` / `leaf_paths`
- `getpath(p)` / `setpath(p; v)` / `delpaths(ps)`
- `del(path-expr)`
- 代入 `=` `|=` `+=` `-=` `*=` `/=` `%=` `//=` の LHS は
  `.foo` / `.foo.bar` / `.[N]` チェーンに加え、`..` 経由の
  `(.. | select(P) | .b) |= F` のような recurse-then-update も動作

### 文字列補間 / フォーマット
- `"prefix \(expr) middle \(expr2) suffix"` (cartesian fan-out)
- 文字列値は raw 埋め込み、その他は `tojson`
- `@text` `@json` `@csv` `@tsv` `@uri` `@html` `@sh` `@base64`
  `@base64d`
- `@fmt "..."` (interpolate 中の値を fmt 経由で escape)

### モジュール
- `module {meta};` — module metadata 宣言
- `import "X" as foo;` — namespace 関数 import
- `import "X" as $var;` — JSON データ import (`$var` と `$var::var` 両方
  に bind)
- `include "X";` — namespace なし関数 import (後勝ち shadow)
- `import "X" as foo {search: "..."};` — モジュール固有の探索 dir
- `foo::a` — namespace 越しの関数呼び出し
- `$ns::name` — namespaced 変数参照
- `-L <dir>` CLI フラグ — 探索パス指定 (繰り返し可)
- 探索パターン: `<dir>/<rel>.jq` と `<dir>/<rel>/<rel>.jq` (jq 流
  ディレクトリ形式)
- `modulemeta` builtin — 入力 string を module relpath として読んで
  `{<meta>, deps:[...], defs:["name/arity", ...]}` を返す
- 循環 import は cache pre-register で安全に終結

## 互換挙動

### Lazy stream eval
`limit(N; gen)` / `first(gen)` / `last(gen)` / `nth(N; gen)` /
`any(gen; cond)` / `all(gen; cond)` / `isempty(gen)` は
`nuq_stream_eval` ヘルパが `comma` / `pipe` を遅延展開し、N 個取れた
時点で gen を打切る:

```jq
limit(1; 1, error)     # → 1 (error は評価されない)
first(1, error)        # → 1
isempty(1, error)      # → false (1 個 emit 確認で停止)
```

### 中途エラー時の partial 出力
pipe が中途で error した場合、既に emit 済みの値を stdout に出して
から error を surface する (jq 互換):

```jq
.[] | if .=="ho" then error else . end
# input: ["hi", "ho"]
# stdout: "hi"
# stderr: nuq: error: ho
# exit:   1
```

### 深さ制限マーカー (jq 互換)
- `tojson` は深さ 10001 以上で `"<skipped: too deep>"` プレースホルダ
- `fromjson` は深さ 10001 以上で `"Exceeds depth limit for parsing"`
- `setpath` は path 長 > 10000 で `"Path too deep"`
- `contains` は深さ 10001 以上で `"Containment check too deep"`
- object merge `*` は深さ 10001 以上で `"Object merge too deep"`

### Bignum stringify
`>2^53` の double を error メッセージで描画する際、`%.17g` のマンティッサ
+ 末尾ゼロパッディングで fixed-point 化 (jq の decnum-flavoured 出力)。
例: `1.2345678901234568e+29` → `123456789012345680000000000000`。

## 組み込み関数 (140+)

### 0 引数

| グループ | 名前 |
|---|---|
| メタ | `length` `utf8bytelength` `type` `keys` `keys_unsorted` `values` `empty` `not` |
| 変換 | `tostring` `to_string` `tonumber` `toboolean` `tojson` `fromjson` `explode` `implode` |
| 文字列 | `ascii_upcase` `ascii_downcase` `reverse` `ltrim` `rtrim` `trim` |
| 集合演算 | `sort` `unique` `add` `min` `max` `to_entries` `from_entries` `paths` `leaf_paths` `flatten` |
| 数値 | `floor` `ceil` `round` `fabs` `abs` `sqrt` `infinite` `nan` |
| 判定 | `isnan` `isinfinite` `isnull` |
| シーケンス | `first` `last` `any` `all` `input` `inputs` |
| 型 filter | `nulls` `booleans` `numbers` `strings` `arrays` `objects` `iterables` `scalars` |
| 環境 | `now` `env` `input_filename` `localtime` `gmtime` |
| エラー | `error` `halt` `halt_error` `recurse` |
| 補助 | `debug` `stderr` `modulemeta` |

### 1 引数

| 名前 | 意味 |
|------|------|
| `select(f)` | `f` が truthy emit を出すなら input を emit |
| `map(f)` | `[.[] \| f]` 等価 |
| `map_values(f)` | 配列・オブジェクトの値を `f` で写像 (構造保持) |
| `with_entries(f)` | `to_entries \| map(f) \| from_entries` 等価 |
| `walk(f)` | bottom-up 木変換 |
| `recurse(f)` | f-fixed-point の DFS emit |
| `add(f)` | `reduce .[] as $x (null; . + ($x \| f))` |
| `pick(f)` | `f` の path だけを残した投影 |
| `has(k)` | キー存在判定 |
| `in(o)` | input が o のキーかどうか |
| `contains(v)` | 再帰判定 (string substring / array subset / object subset) |
| `IN(s)` | input が s の emit のいずれかに等しいか |
| `INDEX(idx)` | input array を idx でグループ化 |
| `isvalid(f)` | `f` がエラーなく走るなら true |
| `range(N)` | `0..N-1` を emit |
| `split(s)` `splits(s)` | 文字列 split (前者は配列、後者は stream) |
| `join(s)` | 配列 join |
| `flatten(N)` | 配列を N 段だけ flatten |
| `ascii(N)` | codepoint → 1 文字 (UTF-8) |
| `startswith(s)` `endswith(s)` `ltrimstr(s)` `rtrimstr(s)` `trimstr(s)` | 接頭・接尾 |
| `first(f)` `last(f)` `any(f)` `all(f)` `isempty(f)` | streaming 集約 |
| `sort_by(f)` `group_by(f)` `unique_by(f)` `min_by(f)` `max_by(f)` | key 関数版 |
| `getpath(p)` | path 配列で index 連鎖 |
| `del(path-expr)` | path 削除 (sugar for delpaths) |
| `delpaths(ps)` | 複数 path をまとめて削除 |
| `paths(f)` | 条件 f を満たす path だけを emit |
| `indices(s)` `index(s)` `rindex(s)` | 文字列内位置 (codepoint 単位) |
| `test(s)` | substring 判定 |
| `tojson` `fromjson` | JSON encode / decode |
| `strftime(f)` `strptime(f)` `strflocaltime(f)` | 時刻書式 |
| `mktime` | tm-array → epoch |

### 2 引数

| 名前 | 意味 |
|------|------|
| `range(M; N)` | `M..N-1` |
| `limit(N; gen)` | 先頭 N 個まで emit (lazy stop) |
| `nth(N; gen)` | N 番目の emit (lazy stop) |
| `recurse(f; cond)` | cond truthy の間だけ recurse |
| `while(cond; update)` | cond の間 emit |
| `until(cond; update)` | cond truthy になるまで update、最終を emit |
| `setpath(p; v)` | path に値を設定 |
| `gsub(s; r)` `sub(s; r)` | substring 置換 (literal 引数) |
| `any(gen; cond)` `all(gen; cond)` | streaming any / all (lazy stop) |
| `INDEX(stream; idx)` | stream を idx でグループ化 |

### 3 引数

| 名前 | 意味 |
|------|------|
| `range(M; N; S)` | step S での range |
| `JOIN($idx; idx_expr; join_expr)` | index join |

### 代入オペレータ

| 名前 | 意味 |
|------|------|
| `path = e`   | path を e で置き換え |
| `path \|= f` | path に f を適用 |
| `path += e` `-=` `*=` `/=` `%=` | path に対応 op を適用 |
| `path //= e` | path が falsy / null なら e で置き換え |

LHS は `.foo` / `.foo.bar` / `.[N]` チェーンに加え、`..` 経由の
recurse-then-update (`(.. | select(...).b) |= ...`) も動作。

## CLI

| flag | 意味 |
|------|------|
| `-c` | コンパクト出力 |
| `-r` | raw 文字列出力 |
| `-R` | raw 文字列入力 |
| `-s` | slurp |
| `-n` | null input |
| `-S` | sort_keys 出力 |
| `-e` | --exit-status (truthy なしで exit 5) |
| `-L <dir>` | module 探索パス追加 |
| `--arg name v` `--argjson name v` | 変数注入 |
| `--slurpfile name f` `--rawfile name f` | ファイル注入 |
| `--seq` | RFC 7464 出力 |
| `--indent N` `--tab` | 整形 |
| `--no-compile` | AOT 無効、interpreter のみ |
| `--no-specialize` | SD 生成無効 |
| `--quiet` | 診断出力抑制 |
| `--dump-ast` | AST を S 式で dump |
| `--help` `-h` | help |

## 制限

- **decnum 未対応**: `>2^53` の整数や巨大指数 (`9E+999999999`) は精度
  落ち。jq 公式テストの残り 2 件はこの制約。
- **正規表現は substring**: `test` / `gsub` / `sub` / `splits` は
  literal 引数なら jq 互換。本格 regex は `sample/astrogre` 経由で
  integrate する方針 (project memory `regexp_astrorge`)。`match` /
  `capture` / `scan` は未実装。
- **`.[]` を含む代入 LHS**: `(.foo[]) = X` のような反復子を含む path
  抽出は限定対応。`.foo[]` を `..` 経由で表現すれば代用可能。
- **streaming pipe ではない**: `f | g` は f の出力を一度 EMIT pool に
  集めてから g を回す。実用 100MB JSON では問題に至らない。
- **`tostream` / `fromstream`**: 未実装。

## ASTro / Code Store

- `INIT()` で `astro_cs_init("code_store", ".", 0)` を呼ぶ
- 起動時にフィルタ式を 1 個の AST にコンパイルし、`astro_cs_compile` →
  `astro_cs_build` → `astro_cs_reload` で SD を生成して dlopen
- インタプリタ専用に絞るなら `--no-compile`
- ccache + sandbox の問題が出る環境では `CCACHE_DISABLE=1`
- 再帰 `def` 本体は独立 entry として個別登録 (`nuq_compile_all_def_bodies`
  / `nuq_load_all_def_bodies`)
