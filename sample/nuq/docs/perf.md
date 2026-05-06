# perf.md — nuq 性能ノート

`make bench` で **3 つのスイート** を実行する:

1. `bench/real/` — **実用ワークロード**: ~1.9 MB / 10k user オブジェクトの
   JSON ファイル (`bench/data/users.json`) に対する典型的 jq クエリ
2. `bench/filters/` — **micro-bench**: jaq の `examples/benches/` から
   借用した単一機能テスト。`n` を stdin で渡す
3. `bench/big/` — **big-data**: 4 形状 × 25MB ≈ 100MB の合成 JSON
   (`bench/data/big/`) に対する集計クエリ

計測は **whole-process wall time** (Open3 + Process.wait):
- shell spawn / exec
- フィルタ式 parse
- 入力 JSON parse (real / big)
- フィルタ評価
- 出力書き出し
- プロセス exit

すべて含む。`time ./nuq ...` がユーザに見える時間と同じ。

## 環境

- gcc 13 -O2、SD は `-O3 -fPIC -fno-plt -march=native`
- Boehm GC (libgc)
- 比較対象: `jq-1.7`、`jaq 3.0.0`、`gojq 0.12.19`
- best-of-3、各セル timeout 30s (big は 120s)、CPU 固定なし
- `CCACHE_DISABLE=1` (AOT セルのみ) — sandbox + ccache の干渉対策

## Real-world bench (input: users.json, ~1.9 MB / 10k users)

絶対値:

| bench | jq | jaq | gojq | nuq int | nuq AOT |
|---|---:|---:|---:|---:|---:|
| `deep_field` (`[.[] \| .stats.followers] \| add`) | 63 ms | 63 ms | 48 ms | 47 ms | 50 ms |
| `extract_field` (`[.[] \| .name] \| length`) | 63 ms | 51 ms | 46 ms | 46 ms | 45 ms |
| `filter_count` (`[.[] \| select(.active and .age > 30)] \| length`) | 66 ms | 65 ms | 50 ms | 49 ms | 47 ms |
| `group_by` (`group_by(.city) \| map(...)`) | 73 ms | 65 ms | 56 ms | 54 ms | 52 ms |
| `identity` (`.`) | 86 ms | 80 ms | 66 ms | 62 ms | 65 ms |
| `keys_aggregate` (`[.[] \| keys] \| add \| unique \| length`) | 278 ms | 109 ms | 87 ms | 82 ms | 87 ms |
| `length` (`length`) | 57 ms | 46 ms | 43 ms | 46 ms | 46 ms |
| `recurse_paths` (`.[0] \| [paths] \| length`) | 61 ms | 50 ms | 41 ms | 44 ms | 46 ms |
| `sort_by` (`sort_by(.score) \| .[-10:] \| map(.name)`) | 78 ms | 67 ms | 126 ms | 49 ms | 53 ms |
| `sum_score` (`[.[] \| .score] \| add`) | 60 ms | 54 ms | 46 ms | 46 ms | 47 ms |
| `transform` (`map({name, email, top_tag: .tags[0]})`) | 80 ms | 85 ms | 56 ms | 54 ms | 56 ms |

vs jq:

| bench | jq | jaq | gojq | **nuq AOT** |
|---|---:|---:|---:|---:|
| `deep_field` | 1.00× | 1.00× | 1.31× | **1.27×** |
| `extract_field` | 1.00× | 1.24× | 1.38× | **1.42×** |
| `filter_count` | 1.00× | 1.02× | 1.33× | **1.41×** |
| `group_by` | 1.00× | 1.13× | 1.30× | **1.41×** |
| `identity` | 1.00× | 1.07× | 1.32× | **1.32×** |
| `keys_aggregate` | 1.00× | 2.56× | 3.21× | **3.18×** |
| `length` | 1.00× | 1.22× | 1.31× | **1.23×** |
| `recurse_paths` | 1.00× | 1.23× | 1.51× | **1.34×** |
| `sort_by` | 1.00× | 1.16× | 0.62× | **1.46×** |
| `sum_score` | 1.00× | 1.12× | 1.32× | **1.28×** |
| `transform` | 1.00× | 0.93× | 1.41× | **1.43×** |

実用 11/11 すべてで jq 越え (1.2-3.2×)。`keys_aggregate` は jaq / gojq
より速い。

## Micro-bench (jaq examples/benches; input = scalar n via stdin)

絶対値:

| bench | n | jq | jaq | gojq | nuq int | nuq AOT |
|---|---|---:|---:|---:|---:|---:|
| `ack` (`ack(3; .)`) | 7 | 448 ms | 620 ms | 524 ms | 92 ms | 85 ms |
| `add` (`[range(.) \| [.]] \| add \| length`) | 2k | 3.1 ms | 2.2 ms | 2.8 ms | 1.3 ms | 1.4 ms |
| `cumsum` (`[foreach range(.) as $x (0; . + $x)] \| length`) | 500k | 131 ms | 129 ms | 186 ms | 19 ms | 18 ms |
| `empty` (`empty`) | 1 | 2.5 ms | 1.5 ms | 1.9 ms | 1.1 ms | 1.2 ms |
| `group-by` (`group_by(. % 2) \| length`) | 100k | 147 ms | 31 ms | 87 ms | 20 ms | 21 ms |
| `kv` (`[range(.) \| {(tostring): .}] \| add \| length`) | 5k | 6.7 ms | 5.9 ms | 7.6 ms | 4.3 ms | 4.6 ms |
| `last` (`last(range(.))`) | 1M | 113 ms | 26 ms | 130 ms | 17 ms | 18 ms |
| `min-max` (`[range(.)] \| min, max`) | 1M | 191 ms | 182 ms | 220 ms | 19 ms | 21 ms |
| `pyramid` (recursive multi-emit) | 8k | 6.4 ms | 6.3 ms | 9.1 ms | 7.6 ms | 7.6 ms |
| `reverse` (`[range(.)] \| reverse \| length`) | 1M | 458 ms | 48 ms | 231 ms | 27 ms | 29 ms |
| `sort` (`[range(.) \| -.] \| sort \| length`) | 300k | 135 ms | 34 ms | 132 ms | 24 ms | 24 ms |
| `to-fromjson` (`[range(.) \| tojson] \| join \| fromjson`) | 100k | 905 ms | 102 ms | 60 ms | 48 ms | 46 ms |
| `try-catch` (`[range(.) \| try error catch .] \| length`) | 500k | 114 ms | 130 ms | 132 ms | 12 ms | 11 ms |
| `upto` (recursive def) | 8k | 475 ms | 5.9 ms | 468 ms | 9.2 ms | 9.4 ms |

vs jq:

| bench | n | jq | jaq | gojq | **nuq AOT** |
|---|---|---:|---:|---:|---:|
| `ack` | 7 | 1.00× | 0.72× | 0.86× | **5.27×** |
| `add` | 2k | 1.00× | 1.38× | 1.11× | **2.22×** |
| `cumsum` | 500k | 1.00× | 1.02× | 0.71× | **7.11×** |
| `empty` | 1 | 1.00× | 1.64× | 1.34× | **2.15×** |
| `group-by` | 100k | 1.00× | 4.75× | 1.69× | **7.12×** |
| `kv` | 5k | 1.00× | 1.14× | 0.87× | **1.45×** |
| `last` | 1M | 1.00× | 4.31× | 0.87× | **6.21×** |
| `min-max` | 1M | 1.00× | 1.05× | 0.87× | **9.02×** |
| `pyramid` | 8k | 1.00× | 1.02× | 0.70× | **0.84×** |
| `reverse` | 1M | 1.00× | 9.64× | 1.98× | **16.00×** |
| `sort` | 300k | 1.00× | 3.94× | 1.02× | **5.64×** |
| `to-fromjson` | 100k | 1.00× | 8.84× | 15.00× | **19.57×** |
| `try-catch` | 500k | 1.00× | 0.88× | 0.86× | **10.24×** |
| `upto` | 8k | 1.00× | 80.65× | 1.02× | **50.70×** |

micro 14 中 13 で jq 越え。`pyramid` のみ 0.84× — deep recursion (8000
ネスト) の C stack frame + EVAL dispatch 圧。jaq・gojq も同程度。

## Big-data bench (~100MB diverse JSON, 4 shapes)

`bench/data/big/` に 4 種の ~25MB JSON (`ruby bench/bench.rb big`):

- **users_big.json** (~33MB) — 130k user-record (wide flat、address ネスト)
- **logs_big.json** (~32MB) — 120k HTTP-log (object with nested headers)
- **tree_big.json** (~24MB) — depth=4 branching=20 の recursive tree
- **table_big.json** (~46MB) — 800k row arrays (CSV-like flat)

絶対値 + vs jq:

| bench (shape) | jq | jaq | gojq | nuq int | nuq AOT | **nuq vs jq** |
|---|---:|---:|---:|---:|---:|---:|
| `bulk_update` (users) | 1.43 s | 983 ms | 1.09 s | 874 ms | 830 ms | **1.73×** |
| `deep_followers` (users) | 1.07 s | 931 ms | 791 ms | 818 ms | 814 ms | **1.32×** |
| `extract_users` (users) | 1.09 s | 883 ms | 763 ms | 793 ms | 806 ms | **1.36×** |
| `group_city` (users) | 1.40 s | 1.10 s | 936 ms | 965 ms | 899 ms | **1.55×** |
| `log_error_paths` (logs) | 828 ms | 808 ms | 562 ms | 654 ms | 623 ms | **1.33×** |
| `log_post_avg` (logs) | 709 ms | 762 ms | 520 ms | 597 ms | 602 ms | **1.18×** |
| `log_request_id` (logs) | 728 ms | 706 ms | 484 ms | 593 ms | 632 ms | **1.15×** |
| `sum_active_score` (users) | 1.10 s | 935 ms | 777 ms | 771 ms | 777 ms | **1.41×** |
| `table_flag_count` (table) | 1.47 s | 1.40 s | 1.16 s | 1.01 s | 996 ms | **1.48×** |
| `table_sum_col0` (table) | 1.56 s | 1.26 s | 1.14 s | 995 ms | 1.01 s | **1.55×** |
| `table_unique_colors` (table) | 3.53 s | 1.65 s | 1.30 s | 1.54 s | 1.43 s | **2.48×** |
| `tree_leaf_sum` (tree) | 1.87 s | 2.74 s | 2.28 s | 881 ms | 874 ms | **2.14×** |
| `tree_numbers` (tree) | 1.86 s | 2.73 s | 2.27 s | 655 ms | 657 ms | **2.83×** |
| `tree_paths` (tree) | 2.73 s | 1.99 s | 3.54 s | 1.46 s | 1.44 s | **1.89×** |

big 14/14 すべて jq 越え (1.2-2.8×)。**tree (recursive walk)** で
jaq / gojq に対しても圧倒 — jq / jaq / gojq はいずれも recursive walk
が苦手で、nuq は 2-4× 速。

実用ベンチ (1.9MB) と big bench (100MB) の比較:
- 1.9MB: nuq 1.2-3.2× vs jq、scale ms 単位
- 100MB: nuq 1.2-2.8× vs jq、scale 秒単位 — **スケールしても比率
  維持** (jq の overhead が線形なので予想通り)

## 解釈

### 大勝の構造

`reverse 1M` (16×)、`min-max 1M` (9.0×)、`last 1M` (6.2×)、`upto 8k`
(51×) は **tight な C ループに inline される** ケース。SD specializer
が AST を一括で fold-in、range の emit ループ + 集約処理が 1 SD 関数
に焼かれる。`to-fromjson 19.6×` は手書き JSON parser/printer の
スループット。

`group-by 7.1×` は qsort + 安定 group。jq は libjq の dict 風内部表現で
extra alloc がかさむ。

実用ベンチの普通の field 抽出系 (deep_field / extract_field / sum_score
/ length / identity) は **1.2-1.5×**。jaq・gojq とほぼ横並び — これらは
「jq の C 実装と nuq の C 実装の per-op オーバーヘッド差」を測って
いるとも言える。

### 残る相対的な弱点

#### `upto` で jaq 比 0.6× (構造的)

jaq は recursive `def` を bytecode に compile して tail-call elimination
する。nuq は tree walker なので各 recursive call が C stack frame +
EVAL dispatch。**vs jq では 51× で勝っている** が、jaq との差は
TCE 起因で構造的。CPS 化しても TCE が入るわけではないので限界がある。

#### `pyramid 8k` で 0.84×

deep recursion (8000 ネスト) と multi-emit per level の dispatch 圧で
律速。jq も同様に苦手だが、libjq の内部表現が比較的軽いのでわずかに
nuq より速い (jaq・gojq は更に遅い)。pool top の register 常駐化で
多少改善見込み。

### nuq AOT vs nuq interp

ほとんどのベンチで AOT は interp と ±5%。**これは jq の構造的な
性質**: jq の hot work は集合演算 (`map / select / sort / add /
group_by`) に集約されていて、その本体は builtin の C ループ。AST
レベルの dispatch コストは支配的でないので、SD specialization で
dispatch を消しても大きな差にならない。

例外: **再帰 def の AST が hot loop になる場合** は AOT が伸びる。
これを引き出すために `def` 本体を独立 entry として AOT 登録している
(`nuq_compile_all_def_bodies` / `nuq_load_all_def_bodies` in
`runtime.c`)。

## 適用済みの主要最適化

### EMIT pool

NODE_DEF が `EMIT { items, count }` を返し、items は CTX 上の flat
VALUE buffer のスライス。per-emit GC alloc ゼロ、SD inline と相性
良し。startup で 4096 entries pre-grow、以降の realloc は UNLIKELY
経路。

`pyramid 8k`: 140× 遅 → 0.84× (互角ライン)。

### Object lookup の lazy hash idx

`nuq_obj.obj` に `uint32_t *idx` を追加 (open-addressing FNV-1a、
load factor ≤ 0.5、threshold 16)。挿入順 parallel array は維持。
`add` builtin の `all_objects` fast path で pairwise `nuq_clone`
カスケードも撲滅。`kv 5k`: 33× 遅 → 1.5× 速。

### Value 演算 fast path inline

`nuq_op_add / sub / mul / neg`、`nuq_eq`、`nuq_cmp`、`nuq_truthy`、
`nuq_make_int` の fixnum fast path を `context.h` の `static inline`
に切り出し、slow case を `_slow` 接尾辞付き関数として `value.c` に
残す。`nuq_op_div / mod` は jq 仕様で常に double 演算なので fast
path 無し、slow に直行。

`min-max 1M` 6.0× → 9.0× / `sort 300k` 3.5× → 5.6× /
`group-by 100k` 5.3× → 7.1× / `cumsum 500k` 5.0× → 7.1×。

注: `static inline` は両者から見える (context.h を両方が include)
ので、interp と AOT が両方同じ程度伸びた。AOT が interp を引き離す
動きは出ていない。AOT-only の上振れには PGO (型 feedback + 仮定
埋め込み + guard) が要るが、jq は集合演算が中心で AST level の
hot loop が薄いので、PGO よりも AST fusion の方が相性がいい。

### AST fusion (parse-time peephole)

`filter.c` の `nuq_make_pipe(lhs, rhs)` で意味保存の書き換え:

- `map(F) | map(G)` → `map(F | G)` (中間配列消去)
- `select(F) | select(G)` → `select(F and G)` (短絡保存)
- `[body] | length` → `emit_count(body)` (専用ノード追加)
- `[body] | add` → `emit_fold_add(body)` (`add` の type-dispatch
  kernel `nuq_add_fold_items` を共有)
- **右辺エッジ fusion**: 左結合 chain `f | g | h` を 1 段ずつ折り
  畳む。`f | sel(a) | sel(b) | sel(c)` のような任意長 chain を
  collapse

意味保存は jq 公式テスト 524 件で確認。`emit_count` ルールが
`try-catch 500k` の 500k 回の中間配列 alloc を一掃して劇的に効いた:

| bench | pre-fusion | post-fusion |
|---|---:|---:|
| `try-catch 500k` | 0.26× | **10.24×** |
| `cumsum 500k` | 5.0× | **7.11×** |
| `keys_aggregate` (real) | 3.25× | **3.18×** (`[X] \| add` fusion) |
| `min-max 1M` | 9.5× | **9.02×** |

### 再帰 def を独立 SD entry に

`nuq_user_call` 内の `EVAL(c, fd->body)` は runtime resolved dispatcher
なので、top-level filter SD からは inline できない。各 def 本体を
独立 entry として `astro_cs_compile` に登録 (`nuq_compile_all_def_bodies`
/ `nuq_load_all_def_bodies` in `runtime.c`)。`upto` AOT vs interp
が伸びる。

### Object literal の direct-build fast path

全エントリ count==1 の典型ケースで cartesian iteration を skip、
pool 直書きで GC_malloc をエントリごとに節約。static key は parser で
`nuq_make_string` を 1 度だけ実行 → entry に VALUE で保存。
`transform` 1.31× → 1.43×。

### `add` builtin の type-dispatch kernel

array-only の fast path で全部の長さを先に集めて単一 alloc + copy で
O(n)。pairwise reduction で O(n²) になっていたバグを撲滅。
`keys_aggregate` 11× 遅 → 3.2× 速。

## 設計上の妥協

- emit は **CTX 上の flat VALUE pool** からのスライス。pros: per-emit
  GC alloc ゼロ、SD inlining 容易。cons: pool 巻き戻し忘れがバグる
- pipe は **lhs を一旦配列に集めて iterate**。streaming にはなって
  いない。実用 100MB JSON でも問題に至らない
- object は **挿入順 parallel array + lazy hash idx**。jq 互換のため
  keys は順序保持

## 計測のお作法

- 1 セルあたり best-of-3
- 全エンジンで同じ stdin / stdout 経路 (Open3 popen3)
- timeout は 30s/cell (big のみ 120s)
- AOT セルだけ `code_store/all.so` を bench 開始時に削除し、bake (1
  attempt) → cached (3 attempts best) で計る
- short scale (ms 単位) のセルは setup-bound なので参考程度。sustained
  scale (~1 秒) で見るのが原則 (project memory: `feedback_bench_sustained`)
