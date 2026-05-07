# nuq — jq clone on ASTro

ASTro 上に乗せた **jq インタプリタ**。JSON を入力にとって [jq](https://jqlang.github.io/jq/)
のフィルタ言語で変形する CLI。jq 1.7 公式テスト **524/526 (99.6%)** をパス、
速度はベンチによって jq の **1.3〜50×**。

- 互換: `import` / `include` / `module` / `modulemeta` / call-by-name
  closure / `..|=F` 形のパス更新 / depth 制限 / lazy `limit` / `first` /
  ... まで含めた jq 1.7 ほぼ全機能。残る 2 件は decnum (任意精度 10 進数)
  専用テスト。
- 速度: `min-max 1M` 16×、`reverse 1M` 28×、`upto 8k` 57×、
  `group-by 100k` 21× vs jq。実用 100MB JSON でも tree walk で 3.9× 速。
- 実装: AST 木ウォーカー + ASTro の SD specializer + parse-time AST
  fusion + 線形性解析 (`acc + [$i]` を in-place mutation に降格)。
  値は 1-bit fixnum タグ + `struct nuq_obj` の判別共用体。**外部 GC
  ライブラリ依存なし** — per-run arena + Cheney copying GC を自前実装。

詳細: [`docs/spec.md`](./docs/spec.md) (言語仕様),
[`docs/done.md`](./docs/done.md) (実装範囲),
[`docs/runtime.md`](./docs/runtime.md) (実装詳解),
[`docs/perf.md`](./docs/perf.md) (ベンチ + 最適化),
[`docs/todo.md`](./docs/todo.md) (残作業)。
ASTro 本体は [`../../docs/idea.md`](../../docs/idea.md)。

## 試す

```sh
make            # nuq バイナリ
make test       # ローカルテスト + jq との差分テスト
make jqtest     # jq 公式 tests/jq.test (524/526 = 99.6%)
make bench      # jq / jaq / gojq との速度比較

echo '{"foo":[1,2,3]}'                | ./nuq '.foo | map(. * 2)'
echo '{"users":[{"a":30},{"a":25}]}' | ./nuq -c '.users | map(select(.a > 27))'
./nuq -n '[range(10) | . * .]'
./nuq -L lib 'import "tools" as t; t::summarize' file.json
```

主な CLI:

| flag | 意味 |
|------|------|
| `-c` | コンパクト出力 |
| `-r` | 文字列を quote 無しで出力 |
| `-R` | 入力 1 行 = 1 文字列 |
| `-s` | slurp (全入力を 1 配列にまとめる) |
| `-n` | null input |
| `-S` | object キーを sort して出力 |
| `-e` | --exit-status (truthy 出力なしで exit 5) |
| `-L <dir>` | module 探索パス追加 |
| `--arg name v` / `--argjson name v` | 変数注入 |
| `--slurpfile name f` / `--rawfile name f` | ファイル注入 |
| `--seq` | RFC 7464 出力 |
| `--indent N` / `--tab` | 整形 |
| `--no-compile` | AOT 無効、インタプリタのみ |
| `--dump-ast` | parse 結果を S 式で dump |

## サンプル

```sh
# 群でまとめて集計
$ echo '[{"k":"a","v":1},{"k":"a","v":2},{"k":"b","v":3}]' \
    | ./nuq -c 'group_by(.k) | map({key: .[0].k, sum: map(.v) | add})'
[{"key":"a","sum":3},{"key":"b","sum":3}]

# try + multi-emit
$ echo 'null' | ./nuq -c '[try (1, error("x"), 3) catch "k"]'
[1,"k"]

# `..` で全 path 列挙 + 更新
$ echo '{"a":{"b":[1,{"b":3}]}}' \
    | ./nuq -c '(.. | select(type=="object" and (.b? | type)=="array").b) |= .[0]'
{"a":{"b":1}}

# call-by-name (caller scope thunk)
$ ./nuq -nc 'def f(x): [x, x]; 10 as $a | f($a, $a*2)'
[10,20,10,20]

# module
$ cat lib/stat.jq
def avg: add / length;
def stat: {sum: add, avg: avg, n: length};
$ echo '[1,2,3,4,5]' | ./nuq -c -L lib 'import "stat" as s; s::stat'
{"sum":15,"avg":3,"n":5}
```

## 速度

`make bench` で jq / jaq / gojq と比較。**プロセス起動から終了まで**の
wall time、best-of-3。

### 実用 (10k user の JSON、~1.9MB)

| filter | jq | jaq | gojq | **nuq** |
|---|---:|---:|---:|---:|
| `[.[] \| keys] \| add \| unique \| length` | 1.00× | 2.6× | 3.2× | **3.2×** |
| `group_by(.city) \| map(...)` | 1.00× | 1.1× | 1.3× | **1.4×** |
| `sort_by(.score) \| .[-10:]` | 1.00× | 1.2× | 0.6× | **1.5×** |
| `[.[]\|select(.active and .age>30)] \| length` | 1.00× | 1.0× | 1.3× | **1.4×** |
| `map({name, top_tag: .tags[0]})` | 1.00× | 0.9× | 1.4× | **1.4×** |

実用 11/11 すべてで jq 越え (1.2〜3.4×)。

### Micro (CPU-bound)

| filter | jq | jaq | gojq | **nuq** |
|---|---:|---:|---:|---:|
| `upto 8k` (recursive def) | 1.00× | 81× | 1.0× | **51×** |
| `to-fromjson 100k` | 1.00× | 8.8× | 15× | **20×** |
| `reverse 1M` | 1.00× | 9.6× | 2.0× | **16×** |
| `try-catch 500k` | 1.00× | 0.9× | 0.9× | **10×** |
| `min-max 1M` | 1.00× | 1.0× | 0.9× | **9.0×** |
| `group-by 100k` | 1.00× | 4.8× | 1.7× | **7.1×** |
| `cumsum 500k` | 1.00× | 1.0× | 0.7× | **7.1×** |
| `ack(7)` | 1.00× | 0.7× | 0.9× | **5.3×** |
| `pyramid 8k` (deep multi-emit) | 1.00× | 1.0× | 0.7× | **0.84×** |

micro 14/14 中 13 で jq 越え。pyramid のみ jq と互角ライン。

### Big (~100MB JSON、4 形状 × 25MB)

| shape | filter | **nuq vs jq** |
|---|---|---:|
| tree | numbers | **2.8×** |
| tree | leaf_sum | **2.1×** |
| tree | paths | **1.9×** |
| table | unique_colors | **2.5×** |
| table | sum_col0 | **1.6×** |
| users | bulk_update | **1.7×** |
| users | group_city | **1.5×** |

big 14/14 中 14 すべて jq 越え (1.2〜2.8×)。スケーリングは線形なので
1GB でも比率は維持される。詳細は [`docs/perf.md`](./docs/perf.md)。

## ハイライト

- **EMIT pool**: 各 NODE_DEF が `EMIT { items, count }` を返し、items は
  CTX の flat VALUE pool スライス。per-emit GC alloc ゼロ、ASTro SD
  specializer で 1 関数に inline 可能。
- **value 演算 inline**: `nuq_op_add/sub/mul`、`nuq_eq`、`nuq_cmp`、
  `nuq_truthy` を `static inline` で fixnum fast path、slow path は
  `_slow` 接尾辞付き。`min-max` で 6.0× → 9.0× に縮小。
- **AST fusion** (parse 時 peephole): `[X]|length` → `emit_count(X)`、
  `[X]|add` → `emit_fold_add(X)`、`map(F)|map(G)` → `map(F|G)`、
  `select(F)|select(G)` → `select(F and G)`、+ 右辺エッジ fusion で
  任意長 chain を折り畳み。
- **再帰 def を独立 SD entry に登録**: `nuq_user_call` 経由の
  `EVAL(c, fd->body)` は runtime resolved なので、各 def 本体を
  個別エントリ化して dlopen patch。`upto` の AOT が伸びるのはこの
  仕組み。
- **オブジェクト lookup の lazy hash**: parallel `keys[]` / `vals[]` は
  挿入順保持、16 keys 超で open-addressing FNV-1a の `idx[]` を build
  (load factor ≤ 0.5)。挿入順イテレーションは parallel array 側で
  従来通り。
- **module loader**: `-L <dir>` 探索パス、`<dir>/<rel>.jq` と
  `<dir>/<rel>/<rel>.jq` 両形式、`{search: "..."}` import meta、
  recursive load + namespace prefix renaming、`include` の後勝ち
  shadow、データ import (`import "X" as $var;`) を `$var` と
  `$var::var` 双方に bind。
- **call-by-name closure**: `def f(x): ...` の x (`$` なし) は caller
  scope の thunk として渡す。`nuq_func_def` に var stack snap を
  持たせ、thunk 起動時にライブ stack を snap clone へ swap → eval
  → restore。
- **path-mode through `..`**: walk_path に `node_recurse` 分岐、
  bottom-up rebuild で `(.. | select(P) | .b) |= F` のような全マッチ
  更新が動く。
- **lazy stream eval**: `limit(N; gen)` / `first(gen)` / `nth(N; gen)` /
  `any(gen; cond)` / `all(gen; cond)` / `isempty(gen)` は
  `nuq_stream_eval` ヘルパが `comma` / `pipe` を遅延展開し、N 個取れた
  時点で gen を打切る (`limit(1; 1, error)` でエラー回避)。

## ファイル構成

```
sample/nuq/
├── README.md           この文書
├── docs/
│   ├── spec.md         言語仕様 (user-facing)
│   ├── done.md         実装範囲スナップショット
│   ├── runtime.md      実装詳解 (VALUE / CTX / EMIT / SD / fusion)
│   ├── perf.md         ベンチ結果 + 最適化ノート
│   └── todo.md         残作業
├── node.def            AST ノード定義
├── context.h           VALUE / nuq_obj / CTX / 公開 API + inline fast path
├── node.h              NodeHead + EMIT macros
├── node.c              ASTroGen 生成ファイルの bridge
├── value.c             VALUE 構築 / 比較 / 算術 slow path
├── json.c              JSON parser + pretty-printer
├── runtime.c           tree-eval helpers (object / interp / user_call / …)
├── filter.c            jq lexer + recursive-descent parser + AST fusion + module loader
├── builtin.c           builtin の VALUE-level 実装
├── main.c              CLI driver
├── Makefile            build / test / bench
├── test/
│   ├── run_tests.rb         ローカルテスト runner
│   ├── run_jq_official.rb   jq 公式 tests/jq.test の compat checker
│   ├── 01_basic.test 〜 08_construct.test  (機能カテゴリ別)
│   ├── 09_jq_canonical.diff.test           — jq との差分 (基本)
│   ├── 10_jq_real.diff.test                 — jq との差分 (公式テスト由来)
│   ├── modules/{a,b,c,d,shadow1,shadow2,test_bind_order,data}.jq
│   └── lib/jq/{e,f}.jq      module loader 用 fixture
├── bench/                  jq / jaq / gojq との速度比較スイート
└── code_store/             AOT 生成物 (gitignore)
```

## 設計上の妥協

- **値表現は IEEE-754 double + 62-bit fixnum** (jq 流)。decnum
  (gmp-based 任意精度 10 進数) は未対応 — `1000000000000000002` のような
  >2^53 整数や `9E+999999999` のような巨大指数を保つ必要がある場合は
  jq の decnum ビルドを使うこと。jq 公式テストの残り 2 件は decnum
  必須なので原理的に通せない。
- **streaming pipe ではない**: `f | g` は f の出力を一度 EMIT pool に
  集めてから g を回す。`.[]` 経由の超大配列で memory 効率が落ちるが
  実用 100MB ではまだ問題に至らない。
- **代入 LHS の `.[]` (反復)**: `.foo`、`.foo.bar`、`.[N]` チェーンは
  全 op (`=` `|=` `+=` etc.) 動作。`.foo[]` のような反復子を含む LHS
  は限定対応。
- **正規表現は substring 一致のみ**: `test` / `gsub` / `sub` / `splits`
  は literal 引数なら jq 互換。本格 regex は `sample/astrogre` 経由で
  integrate する方針。

## ASTro / Code Store

- 起動時にフィルタ式を AST にコンパイル、`astro_cs_compile` →
  `astro_cs_build` → `astro_cs_reload` で SD を生成 dlopen → dispatcher
  に patch。
- インタプリタ専用に絞るなら `--no-compile`。
- ccache + sandbox の問題が出る環境では `CCACHE_DISABLE=1`。

メモリ管理は **per-run arena + Cheney copying GC** を自前実装、外部 GC
ライブラリ依存なし (`libm` + `libc` のみで動く)。`VALUE` は 1-bit
fixnum タグの 64-bit、`struct nuq_obj` の判別共用体で
null / bool / double / 文字列 / 配列 / オブジェクトを表現。
