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

うち 157 件 (`*.diff.test` 群、約 46%) は **system の `jq` 自体を
oracle として期待出力を計算** する微分テスト。jq との挙動差は
その場で検出される。

## 値モデル

- 整数: 62-bit fixnum (1-bit タグ)
- 浮動小数: ヒープ box (`struct nuq_obj` の `dbl`)
- 文字列 / 配列 / オブジェクト: ヒープ box、Boehm GC
- `null` / `true` / `false`: 静的 singleton (`NUQ_NULL_OBJ` 等)
- オブジェクトは **挿入順を保持** (parallel `keys[]` / `vals[]`)
- 16 keys 超で **lazy hash idx** を build (open-addressing FNV-1a、
  load factor ≤ 0.5)。lookup は実質 O(1)、挿入順イテレーションは
  parallel array 側で従来通り

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

## 組み込み関数 (100+)

### 0 引数
| グループ | 名前 |
|---|---|
| メタ | `length` `utf8bytelength` `type` `keys` `keys_unsorted` `values` `empty` `not` |
| 変換 | `tostring` `to_string` `tonumber` `tojson` `fromjson` `explode` `implode` |
| 文字列 | `ascii_upcase` `ascii_downcase` `reverse` |
| 集合演算 | `sort` `unique` `add` `min` `max` `to_entries` `from_entries` `paths` `leaf_paths` `flatten` |
| 数値 | `floor` `ceil` `round` `fabs` `abs` `sqrt` |
| シーケンス | `first` `last` `any` `all` `input` `inputs` |
| 判定 | `isnan` `isinfinite` `infinite` `nan` `isnull` |
| 型 filter | `nulls` `booleans` `numbers` `strings` `arrays` `objects` `iterables` `scalars` |
| 環境 | `now` `env` `input_filename` |
| エラー | `error` `recurse` |

### 1 引数
| 名前 | 意味 |
|------|------|
| `select(f)` | `f` が truthy emit を出すなら input を emit |
| `map(f)` | `[.[] \| f]` 等価 |
| `map_values(f)` | 配列・オブジェクトの値を `f` で写像 (構造保持) |
| `with_entries(f)` | `to_entries \| map(f) \| from_entries` 等価 |
| `walk(f)` | bottom-up 木変換 |
| `recurse(f)` | f-fixed-point の DFS emit |
| `has(k)` | キー存在判定 (object/array 両対応) |
| `in(o)` | input が o のキーかどうか |
| `contains(v)` | 再帰判定 (string substring / array subset / object subset) |
| `IN(s)` | input が s の emit のいずれかに等しいか |
| `isvalid(f)` | f がエラーなく走るなら true |
| `range(N)` | `0..N-1` を emit |
| `split(s)` `splits(s)` | 文字列 split (前者は配列、後者は stream) |
| `join(s)` | 配列 join (null は空文字、非文字列は tostring) |
| `flatten(N)` | 配列を N 段だけ flatten |
| `ascii(N)` | codepoint → 1 文字 (UTF-8) |
| `startswith(s)` `endswith(s)` `ltrimstr(s)` `rtrimstr(s)` | 接頭・接尾 |
| `first(f)` `last(f)` `any(f)` `all(f)` | f を各要素に適用しつつ集約 |
| `sort_by(f)` `group_by(f)` `unique_by(f)` `min_by(f)` `max_by(f)` | key 関数版 |
| `getpath(p)` | path 配列で index 連鎖 |
| `del(path-expr)` | path 削除 (sugar for delpaths) |
| `delpaths(ps)` | 複数 path をまとめて削除 |
| `indices(s)` `index(s)` | 文字列内位置の全列挙 / 最初 |
| `test(s)` | substring 判定 (real regex は todo) |

### 2 引数
| 名前 | 意味 |
|------|------|
| `range(M; N)` | `M..N-1` |
| `limit(N; f)` | 先頭 N 個まで emit (負の数はエラー) |
| `nth(N; f)` | N 番目の emit (0-based) |
| `recurse(f; cond)` | cond truthy の間だけ recurse |
| `while(cond; update)` | cond の間 emit |
| `until(cond; update)` | cond truthy になるまで update、最終を emit |
| `setpath(p; v)` | path に値を設定 |
| `gsub(s; r)` `sub(s; r)` | substring 置換 (literal 引数なら jq 互換) |

### 3 引数
| 名前 | 意味 |
|------|------|
| `range(M; N; S)` | step S での range |

### 代入オペレータ
| 名前 | 意味 |
|------|------|
| `path = e`   | path を e で置き換え |
| `path \|= f` | path に f を適用して置き換え |
| `path += e` `-=` `*=` `/=` `%=` | path に対応 op を適用 |
| `path //= e` | path が falsy / null なら e で置き換え |

LHS は `.foo` / `.foo.bar` / `.[N]` チェーンに限定 (`.[]` 反復含みは
未対応)。

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

## バグ修正履歴 (v0.1 — bench 駆動)

`make bench` で見えた問題から逆引きで見つけたバグ:

- **CTX を `calloc` で確保していたため、内部の `var_stack` /
  `funcs` ポインタが Boehm GC からスキャンされず、GC_malloc 側の
  `var_stack` ブロックが live と認識されないまま回収される**バグ。
  foreach / reduce で n>=490 ぐらいから `$x undefined` が出ていた。
  pystro / astr に倣い `GC_malloc(CTX)` に修正。bench 駆動でなければ
  単発テストでは見つからなかったクラスのバグ (capa を超える alloc
  pressure が必要) で、修正後 `upto` が 3.4× → 39× / `cumsum` が
  1.4× 遅 → 1.1× 速になった。
- `nuq_clone(object)` が `nuq_object_set` 経由で 1 個ずつ insert
  していたため O(n²) → 全体で O(n³)。ソース側のキーは既に unique なので
  set のチェック (= 線形 collision 走査) を回避し直接 push に。
  `kv` n=5000 が 119s → 0.25s。
- `group_by` の sort が手書き挿入ソート → n=100k で 30s timeout。
  qsort + per-pair comparator (key は pair の 1 要素目) で O(n log n)。
  group-by 100k が timeout → 94ms。
- `node.def` で約 40 ノードに付いていた `@noinline` を整理。runtime
  helper を呼ぶだけのスタブだったので不要 (むしろ inline を阻害)。

## バグ修正履歴 (v0.2 — outlier 撲滅)

- **`error` (0-arg) が builtin 登録漏れ** — `try error catch .` で
  `error` が user-call 経路に流れ、未定義関数として stderr に
  "error/0 is not defined" を吐きつつ catch で握り潰されていた。
  500k iter で大量の I/O が支配的に。`filter.c` に
  `BUILTIN0("error", ALLOC_node_error0)` を 1 行追加。
  try-catch 500k: 0.26× → 8.75×。
- **object lookup の O(n²)** — `nuq_obj.obj` に `uint32_t *idx` を
  追加 (open-addressing FNV-1a、load factor ≤ 0.5、threshold 16)。
  `add` builtin にも `all_objects` fast path (pairwise clone を消した)
  を追加。kv 5k: 0.04× → 1.5× 速。500k に増やすと jq の 530ms に対し
  nuq 310ms と linear scaling 確認。
- **object literal で per-emit の小さい alloc が累積** — 全エントリ
  count==1 のとき cartesian iteration を skip して pool に直書き。
  static key (`{a: ...}`) は parser で 1 度 `nuq_make_string` した
  VALUE を `nuq_obj_entry.kname_value` に格納し、ctor 呼び出し
  ごとの再 alloc を撲滅。transform 1.31× → 1.50×。
- **再帰 def が SD specialize されてなかった** — `nuq_user_call` 内の
  `EVAL(c, fd->body)` は runtime-resolved dispatcher 経由なので、
  top-level filter の SD からは inline できない。各 def 本体を独立
  entry として `astro_cs_compile` に登録 (`nuq_compile_all_def_bodies`
  / `nuq_load_all_def_bodies`)。upto AOT vs interp が 1.0× → 1.1-2.5×
  (run variance あり)、ack 同 7.5× → 8.4×。usage.md "Entry nodes"
  の規則に従ったもの。

## 性能改善 (v0.3 — value op inline + AST fusion)

- **value 演算 fast path の `static inline` 化** (`context.h`):
  `nuq_op_add / sub / mul / neg`、`nuq_eq`、`nuq_cmp`、`nuq_truthy`、
  `nuq_make_int` の fixnum 高速パスを header inline に。slow path は
  `_slow` 接尾辞付き関数として `value.c` に残す。
  `min-max 1M` 6.0× → 9.7× / `sort 300k` 3.5× → 5.6× /
  `group-by 100k` 5.3× → 7.3× (vs jq、AOT)。
- **parse-time AST fusion** (`filter.c` の `nuq_make_pipe`):
  - `map(F) | map(G)` → `map(F | G)` (中間配列消去)
  - `select(F) | select(G)` → `select(F and G)` (短絡保存)
  - `[body] | length` → `node_emit_count(body)` (新ノード)
  - `[body] | add` → `node_emit_fold_add(body)` (新ノード、`add`
    の type-dispatch kernel `nuq_add_fold_items` を共有)
  - **右辺エッジ fusion**: parse は左結合なので `f | g | h` は
    `pipe(pipe(f, g), h)` になり、隣接ペアでない `g | h` は直接
    マッチしない。`nuq_make_pipe` で lhs が pipe のとき、その rhs
    と新 rhs を `nuq_try_fuse_pair` に投げ、成功なら splice 戻し。
    これで `f | sel(a) | sel(b) | sel(c)` のような任意長 chain が
    左から順に折り畳まる。
  全ルール意味保存 (jq との差分テスト 169 件 PASS)。代表的な効果:
  - `try-catch 500k`: 0.26× → **12-14× vs jq** (中間配列を完全消去)
  - `cumsum 500k`: 5.0× → **7.0× vs jq**
  - `keys_aggregate` (real): 3.25× → **3.6× vs jq** (`[X] | add`
    fusion で `add` builtin の dispatch を 1 step 短縮)
  - `sum_score` (real): 1.55× → 1.48× (誤差圏)

## jq 互換性パス (v0.4)

仕様の大穴をまとめて埋め、341 件 → builtin 100+ 程度に拡張。

### 言語仕様
- **代入オペレータ**: `=` `|=` `+=` `-=` `*=` `/=` `%=` `//=`
  (path 抽出 `.foo`、`.foo.bar`、`.[N]` チェーンに対応)
- **multi-elif chain**: 任意段数サポート (parse_if_tail 再帰)
- **path 操作**: `setpath(p; v)` / `delpaths(ps)` / `del(path)` /
  `leaf_paths`

### 高階・制御
- `recurse(f)` / `recurse(f; cond)` / `walk(f)`
- `while(cond; update)` / `until(cond; update)`
- `any(f)` / `all(f)` (1-arg version)

### 型 / 集合演算
- 型 filter: `nulls` / `booleans` / `numbers` / `strings` /
  `arrays` / `objects` / `iterables` / `scalars`
- `flatten` / `flatten(N)` / `IN(s)` / `isvalid(f)` / `splits(s)`
- `contains` を完全再帰版に + 型ミスマッチで jq 互換 error

### 文字列
- `ltrimstr` / `rtrimstr` / `gsub(s; r)` / `sub(s; r)` (substring)
- `ascii(N)` / `utf8bytelength`

### CLI / I/O
- `--arg` / `--argjson` / `--slurpfile` / `--rawfile`
- `input` / `inputs` / `env` (環境変数 object)
- `--seq` (RFC 7464) / `-e` / `--exit-status`
- short flag bundle (`-nc` 等) / `-S` (sort_keys 配線)

### バグ修正
- `node_iter` のエラー stderr 直書き → c->error 経由 (try / `?`
  で stderr 汚染が消える)
- `nuq_run` の出力順を「emits → error」に (jq compatible)
- value-helper の "nuq error: ..." stderr print を try / isvalid 中
  だけ抑制 (`nuq_suppress_error_print` カウンタ)

338/338 tests PASS。
