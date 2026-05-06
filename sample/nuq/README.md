# nuq — jq clone on ASTro

ASTro フレームワーク上に乗せた **jq サブセット** インタプリタ。
JSON を入力にとって [jq](https://jqlang.github.io/jq/) のフィルタ言語で
変換する。フィルタ言語のほぼ全カテゴリ (パイプ / コンマ / 算術 / 比較 /
論理 / `//` / 配列・オブジェクト構築 / `if-then-else-end` / `try-catch` /
`reduce` / `foreach` / `label/break` / 変数束縛 `as $x` / ユーザ定義
`def` / 文字列補間 / `@csv` 等のフォーマット) と 70+ の組み込み関数を
実装。

実装の詳細は [`docs/runtime.md`](./docs/runtime.md)、
動く範囲は [`docs/done.md`](./docs/done.md)、
未実装と性能 todo は [`docs/todo.md`](./docs/todo.md)、
性能測定の方針は [`docs/perf.md`](./docs/perf.md)。
ASTro 本体は [`../../docs/idea.md`](../../docs/idea.md)。

## 試す

```sh
make            # nuq バイナリ
make test       # test/*.test 338 件 (うち 157 件は real jq との差分テスト)

echo '{"foo": [1,2,3]}'           | ./nuq '.foo | map(. * 2)'
echo '{"users":[{"a":30},{"a":25}]}' | ./nuq -c '.users | map(select(.a > 27))'
./nuq -n '[range(10) | . * .]'
./nuq --no-compile '.[]' file.json   # AOT bake をスキップしてインタプリタだけ
./nuq --dump-ast '.foo | length'     # AST を S 式で dump
```

主なオプション:

| flag | 意味 |
|------|------|
| `-c` | コンパクト出力 (改行・インデントなし) |
| `-r` | raw 文字列出力 (出力が文字列ならクォート無し) |
| `-R` | raw 文字列入力 (各行を文字列として入力) |
| `-s` | slurp (全入力を 1 配列にまとめてから filter にかける) |
| `-n` | null input (`null` を 1 個入力する形) |
| `--indent N` | インデント幅 (default 2) |
| `--no-compile` | Code Store を使わずインタプリタで実行 |
| `--dump-ast` | parse 結果の AST を出力 |

## サンプル

```sh
# 群でまとめて集計
$ echo '[{"k":"a","v":1},{"k":"a","v":2},{"k":"b","v":3}]' \
    | ./nuq -c 'group_by(.k) | map({key: .[0].k, sum: map(.v) | add})'
[{"key":"a","sum":3},{"key":"b","sum":3}]

# `try`+ストリームの組合せ
$ echo 'null' | ./nuq -c '[try (1, error("x"), 3) catch "k"]'
[1,"k"]

# パスの再帰展開
$ echo '[1,[[2]],{"a":[3]}]' | ./nuq -c '[..]'
[[1,[[2]],{"a":[3]}],1,[[2]],[2],2,{"a":[3]},[3],3]

# def + reduce
$ echo '[1,2,3,4,5]' | ./nuq -c 'def avg: add / length; avg'
3
```

## テストスイート

```
$ make test
ruby test/run_tests.rb

passed: 338  failed: 0  skipped: 0  total: 338
```

## ベンチマーク

`make bench` で **jq / jaq / gojq / nuq** を 2 スイートで比較。
**プロセス起動から終了まで** の wall time を計測。

### 実用ベンチ — 10k user オブジェクト JSON ファイルへの典型クエリ

| bench (vs jq) | jq | jaq | gojq | **nuq AOT** |
|---|---:|---:|---:|---:|
| `[.[] \| .name] \| length` (extract) | 1.00x | 1.11x | 1.30x | **1.38x** |
| `[.[] \| .stats.followers] \| add` (deep) | 1.00x | 1.09x | 1.22x | **1.40x** |
| `[.[] \| .score] \| add` (sum) | 1.00x | 1.18x | 1.37x | **1.48x** |
| `length` | 1.00x | 1.20x | 1.29x | **1.33x** |
| `group_by(.city) \| map({...})` | 1.00x | 1.15x | 1.35x | **1.49x** |
| `[.[] \| select(.active and .age > 30)] \| length` | 1.00x | 0.99x | 1.22x | **1.39x** |
| `[.[] \| keys] \| add \| unique \| length` | 1.00x | 2.76x | 3.16x | **3.56x** |
| `map({name, email, top_tag: .tags[0]})` | 1.00x | 0.92x | 1.47x | **1.47x** |
| `sort_by(.score) \| .[-10:] \| map(.name)` | 1.00x | 1.16x | 0.63x | **1.50x** |
| `[.[0] \| paths] \| length` | 1.00x | 1.21x | 1.34x | **1.41x** |
| `.` (identity) | 1.00x | 1.04x | 1.25x | **1.50x** |

**実用 11 中 11 すべてで jq 越え** (1.3-3.4×)。

### Micro-bench — jaq examples/benches より

| bench | jq | jaq | gojq | **nuq AOT** |
|---|---:|---:|---:|---:|
| `upto 8k` (recursion) | 1.00x | 88.56x | 1.05x | **51.87x** |
| `to-fromjson 100k` | 1.00x | 7.87x | 13.91x | **20.17x** |
| `reverse 1M` | 1.00x | 8.98x | 2.06x | **17.24x** |
| `try-catch 500k` | 1.00x | 0.87x | 0.89x | **12.89x** |
| `min-max 1M` | 1.00x | 1.09x | 0.95x | **10.24x** |
| `last 1M` | 1.00x | 4.28x | 0.83x | **9.69x** |
| `ack(3; 7)` | 1.00x | 0.72x | 0.85x | **8.43x** |
| `group-by 100k` | 1.00x | 4.61x | 1.67x | **7.75x** |
| `cumsum 500k` | 1.00x | 1.07x | 0.72x | **7.54x** |
| `sort 300k` | 1.00x | 3.56x | 1.03x | **5.62x** |
| `add 2k` (array concat) | 1.00x | 1.25x | 1.17x | **2.41x** |
| `empty` (起動) | 1.00x | 1.93x | 1.48x | **2.31x** |
| `kv 5k` (object concat) | 1.00x | 1.16x | 0.85x | **1.55x** |
| `pyramid 8k` (multi-emit recursion) | 1.00x | 0.95x | 0.72x | **0.93x** |

**micro 14 中 13 で jq 越え** (pyramid のみ 0.93× で互角ライン)。

`upto` で **jaq (Rust) と同 50-80× レンジ** (run variance ±20%)。
適用済みの主要最適化:
- **EMIT pool**: NODE_DEF が `EMIT { items, count }` を返し、items は CTX 上の flat VALUE buffer のスライス。per-emit GC alloc ゼロ、SD inline と相性良し。
- **object lookup を hash 化** (open-addressing FNV-1a, threshold 16, lazy build) + `add` の type-aware fast path (all_arrays / all_strings / all_objects)。kv が 25× 遅 → 1.5× 速、`keys_aggregate` 系も伸び。
- **再帰 def を独立 SD entry に登録** — `nuq_user_call` 経由の `EVAL(c, fd->body)` は runtime resolved なので、各 def 本体を `astro_cs_compile` 個別エントリ化。upto / ack の AOT が interp の 1.2-2.5×。
- **value 演算 fast path を `static inline`** (context.h): `nuq_op_add / sub / mul / neg`、`nuq_eq`、`nuq_cmp`、`nuq_truthy`、`nuq_make_int` の fixnum 高速路。`min-max / sort / group-by` で 30-60% 縮小。
- **AST fusion** (parse 時 peephole): `[body] | length` → `emit_count`、`[body] | add` → `emit_fold_add`、`map(F) | map(G)` → `map(F | G)`、`select(F) | select(G)` → `select(F and G)`、+ 右辺エッジ fusion で長 chain も折り畳み。`try-catch 500k` が 0.26× → 12.9×、`cumsum 500k` が 5.0× → 7.5×。
- `error/0` builtin 登録漏れ修正、object literal の direct-build fast path、EMIT pool の起動時 pre-grow など細かい修正多数。

詳細は [`docs/done.md`](./docs/done.md) と [`docs/perf.md`](./docs/perf.md)。

`test/*.test` は jq 公式テストと同じフォーマット (filter 1 行 / 入力 JSON
1 行 / 期待出力 N 行 / 空行で区切り) を採用している。`*.diff.test`
ファイルは **system の `jq` を oracle にして期待出力を計算** する
微分テスト — jq の挙動と差が出た瞬間に落ちる。jq が無い環境では
skip 扱い。

実装中に jq との差分テストが拾った非自明バグ:

- `values` 組み込みは `select(. != null)` 等価 (オブジェクトを array に
  落とすのではない)。
- `"hello \(.)"` の文字列補間で値が文字列の場合は raw 出力 (jq の
  `tostring` と同じく `string→string`、それ以外 `→ tojson`)。
- `try (1, error("x"), 3) catch "k"` は `[1,"k"]` (失敗前の emit を
  保持する)。

## ファイル構成

```
sample/nuq/
├── README.md           この文書
├── docs/
│   ├── runtime.md      実装詳細 (VALUE / CTX / eval model / side tables)
│   ├── done.md         実装済み機能 + 組み込み一覧
│   ├── todo.md         未実装機能 + 性能 todo
│   └── perf.md         測定方針 + ベンチノート
├── node.def            AST ノード定義 (114 種)
├── context.h           VALUE / nuq_obj / CTX / 公開 API
├── node.h              NodeHead + EVAL マクロ
├── node.c              ASTroGen 生成ファイルの bridge
├── value.c             VALUE 構築 / 等価 / 順序 / JSON 算術ヘルパ
├── json.c              JSON parser + pretty-printer
├── runtime.c           tree-eval helpers (object ctor / interp / def call / side tables)
├── filter.c            jq フィルタ言語の lexer + recursive-descent parser
├── builtin.c           70+ の組み込み関数 (length, keys, map, select, range, ...)
├── main.c              CLI driver
├── Makefile            build / test / clean
├── test/
│   ├── run_tests.rb    テストランナー
│   ├── 01_basic.test       — 識別子 / リテラル / 算術
│   ├── 02_access.test      — `.foo` / `.[i]` / `.[]` / slice / `..`
│   ├── 03_pipe_comma.test  — `|` / `,` の合成
│   ├── 04_compare.test     — 比較 / 論理 / `//`
│   ├── 05_builtins.test    — 組み込み関数一通り
│   ├── 06_control.test     — `if` / `try` / `as` / `def` / `reduce` / `foreach`
│   ├── 07_strings.test     — interpolation + `@csv/@uri/...`
│   ├── 08_construct.test   — `[...]` / `{...}` のファンアウト
│   ├── 09_jq_canonical.diff.test  — jq との差分 (基本)
│   └── 10_jq_real.diff.test       — jq との差分 (jq 公式テスト由来)
└── code_store/         AOT 生成物 (gitignore)
```

## 制限 (詳細は docs/todo.md)

- **代入 / 更新代入** (`f = g`、`f |= g`、`+= -= */= //=`) — path 表現を
  返す accessor バリアントが必要で未実装。
- **path-aware `del / setpath / delpaths` / `leaf_paths`** — 上記と同根
  (`paths` 自体は実装あり)。
- **真の正規表現** (`test / match / capture / splits / sub / gsub`) —
  `sample/astrogre` 経由で integrate する方針 (project memory
  `regexp_astrorge`)。現状 `test` は substring 一致のみ。
- **streaming pipe** — 現状 `f | g` は EMIT pool 上に集めてから `g` を
  回す。長大入力では memory 効率が良くない。
- **多段 elif chain** — 1 段だけサポート。
- **`input` / `inputs` / `--seq` / `--arg` / `--argjson`** などの CLI
  入力経路。
- **`-S` (sort_keys)** — 受け取るが json print に渡してない (no-op)。
- **モジュール / `import` / `include`** — token は受け付けるが no-op。

GC は Boehm-Demers-Weiser、`VALUE` は 1-bit fixnum タグの 64-bit。
`struct nuq_obj` の判別共用体で null / bool / double / 文字列 / 配列 /
オブジェクトを表現。

> AOT bake で `astro_cs_build: make failed` が出る環境では
> `CCACHE_DISABLE=1` を環境変数で渡すと回避できる
> (project memory `feedback_ccache_disable` 参照)。
> インタプリタ単独で動かすなら `--no-compile` を付ける。

## 将来: JSON 以外の入力

`struct nuq_obj` の判別共用体は JSON のシェイプを直接表すが、YAML /
TOML はそのまま同じ構造に乗る (primitives + array + object)。新しい
front-end parser を一本書いて `--input yaml` のような flag で選ぶだけで、
既存のフィルタ実装はそのまま動く。XML / SQL のような構造の異なる
入力は、value 表現を抽象化する vtable をかぶせるのが筋 — todo F1
を参照。
