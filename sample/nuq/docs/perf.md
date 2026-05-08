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
- メモリ管理: per-run arena + Cheney copying GC (詳細は
  [runtime.md §5](runtime.md#5-メモリ管理--per-run-arena--cheney-copying-gc))。
  外部 GC ライブラリ依存なし — `libm` + `libc` のみで動く
- 比較対象: `jq-1.7`、`jaq 3.0.0`、`gojq 0.12.19`
- best-of-3、各セル timeout 30s (big は 120s)、CPU 固定なし
- `CCACHE_DISABLE=1` (AOT セルのみ) — sandbox + ccache の干渉対策

## Real-world bench (input: users.json, ~1.9 MB / 10k users)

vs jq (best-of-3):

| bench | jq | jaq | gojq | **nuq AOT** |
|---|---:|---:|---:|---:|
| `deep_field` (`[.[] \| .stats.followers] \| add`) | 1.00× | 1.10× | 1.38× | **3.13×** |
| `extract_field` (`[.[] \| .name] \| length`) | 1.00× | 1.17× | 1.33× | **2.95×** |
| `filter_count` (`[.[] \| select(.active and .age > 30)] \| length`) | 1.00× | 1.08× | 1.39× | **3.23×** |
| `group_by` (`group_by(.city) \| map({...})`) | 1.00× | 1.14× | 1.34× | **3.65×** |
| `identity` (`.`) | 1.00× | 1.21× | 1.12× | **2.65×** |
| `keys_aggregate` (`[.[] \| keys] \| add \| unique \| length`) | 1.00× | 2.71× | 3.51× | **4.99×** |
| `length` (`length`) | 1.00× | 1.15× | 1.30× | **2.58×** |
| `recurse_paths` (`.[0] \| [paths] \| length`) | 1.00× | 1.11× | 1.22× | **2.64×** |
| `sort_by` (`sort_by(.score) \| .[-10:] \| map(.name)`) | 1.00× | 1.17× | 0.70× | **3.43×** |
| `sum_score` (`[.[] \| .score] \| add`) | 1.00× | 1.07× | 1.25× | **2.84×** |
| `transform` (`map({name, email, top_tag: .tags[0]})`) | 1.00× | 0.88× | 1.49× | **2.89×** |

実用 11/11 すべてで **2.6-5.0× vs jq**。直前世代 (1.6-3.6×) からの追加
高速化要因:

- **GC scan ループの早期 break バグ修正** (`gc_arena_collect`): `for(;;)`
  の頭で 1 回 cache していた `chunk_end` を毎反復で再評価。chunk
  transition 直前に書き込まれた obj が scan されず stale ptr が残る
  silent corruption を解消、ついでに `transform` / `keys_aggregate` の
  10K-scale segfault も消滅 (詳細は本ファイル末尾)。
- **`in_arena` を O(N chunks) → O(log N)**: GC 開始時に from-space
  chunks を address でソートした index を作って binary search +
  min/max range pre-check。`tree_paths` で 80% CPU を食っていた
  ホットスポット消滅 (3.27 s → 0.93 s で回るように)。
- **GC threshold を入力サイズに連動**: `nuq_run` 開始時に
  `arena_gc_threshold = max(2 × arena_total, 16 MB)` で初期値を
  bump。1.9 MB JSON が ~17 MB の内部表現に膨らんで 16 MB 閾値で
  即 GC 発火するムダを回避 — real bench 一律 +30%。
- **JSON parser fast path**: `parse_string_raw` の no-escape 分岐
  (closing quote まで scan して 1 回の alloc + memcpy で済ます) と
  `parse_number` の int fast path (`strtoll` + 中間 buffer 回避)。
  parse 比率が大きい real / big の全 cell に効く。
- **静的キー distinct な object literal で `nuq_object_set` →
  `nuq_object_append`**: parser が intern 時に kkind / kname を見て
  全 entry が distinct な静的 cstring と判定できれば
  `all_distinct_static` フラグを立て、runtime の fast path で
  dedup-scan + PIN3 を skip。`transform` / `reshape` 系で効く。

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
| `bulk_update` (users) | 1.43 s | 974 ms | 1.10 s | 393 ms | 401 ms | **3.56×** |
| `deep_followers` (users) | 1.07 s | 893 ms | 754 ms | 341 ms | 343 ms | **3.11×** |
| `extract_users` (users) | 1.05 s | 875 ms | 739 ms | 343 ms | 354 ms | **2.96×** |
| `group_city` (users) | 1.35 s | 1.03 s | 851 ms | 370 ms | 371 ms | **3.65×** |
| `log_error_paths` (logs) | 837 ms | 793 ms | 546 ms | 259 ms | 257 ms | **3.25×** |
| `log_post_avg` (logs) | 759 ms | 728 ms | 495 ms | 230 ms | 230 ms | **3.30×** |
| `log_request_id` (logs) | 706 ms | 691 ms | 469 ms | 216 ms | 211 ms | **3.34×** |
| `sum_active_score` (users) | 1.04 s | 916 ms | 756 ms | 337 ms | 338 ms | **3.09×** |
| `table_flag_count` (table) | 1.45 s | 1.38 s | 1.13 s | 472 ms | 459 ms | **3.17×** |
| `table_sum_col0` (table) | 1.53 s | 1.24 s | 1.12 s | 466 ms | 469 ms | **3.26×** |
| `table_unique_colors` (table) | 3.37 s | 1.57 s | 1.25 s | 811 ms | 787 ms | **4.28×** |
| `tree_leaf_sum` (tree) | 1.77 s | 2.61 s | 2.21 s | 364 ms | 359 ms | **4.94×** |
| `tree_numbers` (tree) | 1.69 s | 2.66 s | 2.10 s | 277 ms | 272 ms | **6.23×** |
| `tree_paths` (tree) | 2.49 s | 1.82 s | 3.22 s | 933 ms | 934 ms | **2.66×** |

big 14/14 すべて jq 越え (**2.7-6.2×**)。**tree (recursive walk)** で
jaq / gojq に対しても圧倒 — `tree_numbers` 6.2×、`tree_leaf_sum` 4.9×。
パス収集が必要な `tree_paths` (~2M paths を集める) は 100MB 級
arena への scan が支配項なので 2.7×、それでも他エンジン全敗。

実用ベンチ (1.9MB) と big bench (100MB) の比較:
- 1.9MB: nuq 2.6-5.0× vs jq、scale ms 単位
- 100MB: nuq 2.7-6.2× vs jq、scale 秒単位 — **スケールするほど比率
  伸びる** (in_arena binary search のお陰で GC scan が O(N²) → O(N log N) に)

## JSONL/NDJSON bench (実 GitHub Archive データ、~100 MB / 30k events)

`bench/data/jsonl/gharchive.jsonl` は GitHub Archive の 1 時間スライス
(CC0)、`make bench-data-jsonl` でダウンロード ~120 MB gzipped を
30 K records (~100 MB text) に truncate。コマンドは `ruby bench/bench.rb
jsonl` または `make bench-jsonl`。

streaming 系 (per-value 入力) と aggregating 系 (`-n + [inputs]`) を
混ぜた 9 フィルタ:

| bench | jq | jaq | gojq | **nuq AOT** |
|---|---:|---:|---:|---:|
| `identity` (`.`) | 1.00× | 1.47× | 1.42× | **6.10×** |
| `extract_login` (`select(.type=="PushEvent") \| .actor.login`) | 1.00× | 1.06× | 1.00× | **2.45×** |
| `commits_message` (`...payload.commits[]?.message`) | 1.00× | 1.00× | 0.91× | **4.23×** |
| `reshape` (`{user, t, r, ts}` 投影) | 1.00× | 1.01× | 1.06× | **5.15×** |
| `select_recent_PR` (PullRequestEvent + opened) | 1.00× | 1.07× | 1.05× | **4.82×** |
| `type_histogram` (`group_by(.type)`) | 1.00× | 1.10× | 1.03× | **2.37×** |
| `unique_repos` (`unique \| length`) | 1.00× | 1.10× | 1.04× | **2.38×** |
| `count_pushes` (`[inputs \| select] \| length`) | 1.00× | 1.15× | 1.15× | **2.57×** |
| `top_users` (`[inputs] \| group_by \| sort_by \| .[0:10]`) | 1.00× | 1.08× | 1.03× | **2.38×** |

JSONL **9/9 で 2.4-6.1× vs jq**。前世代までは:
- streaming 5/5 で 2.2-3.6× (parse+print スループット)
- group_by / unique 1.4× 維持
- **`count_pushes` / `top_users` だけ 0.33-0.35×** (jq の incremental
  streaming 集計に構造的に勝てなかった大敗)

最後の 2 件が逆転したのは下記 2 つの最適化がそれぞれ効いた結果:

- **SIMD string scanner** で `parse_string_raw` / `print_string` の
  inner byte-loop を SSE2 16-byte stride に。`identity` (parse +
  print のスループット勝負) が 3.6× → 6.1×。
- **`inputs | F` の streaming-pipe fusion**: `pipe(inputs, F)` →
  `node_b_inputs_pipe(F)`、`[inputs | F] | length` →
  `node_b_count_inputs(F)`、`pipe(inputs_pipe(F), G)` →
  `inputs_pipe(F | G)` (chain absorption)。30 K records を pool に
  同時保持しないので、`count_pushes` が 0.35× → 2.57× に逆転。
  `top_users` も SIMD と GC threshold 緩和の合算で 0.33× → 2.38×。

## メモリ — peak RSS (HWM)

`/usr/bin/time -f '%M'` (Linux maxresident, KB) を `BENCH_MEM=1
ruby bench/bench.rb {real,big}` で採取。**worst-of-2** (時間ベンチが
best-of-3 で取るのと違って、メモリは high-water mark を見るため
最悪値を採る)。

### Real (input: users.json, 1.9 MB)

| bench | jq | jaq | gojq | nuq int | nuq AOT | **nuq/jq** |
|---|---:|---:|---:|---:|---:|---:|
| `deep_field` | 26.6 MB | 26.1 MB | 25.2 MB | 33.1 MB | 33.2 MB | **1.25×** |
| `extract_field` | 26.5 MB | 26.2 MB | 25.1 MB | 33.4 MB | 33.2 MB | **1.25×** |
| `filter_count` | 26.4 MB | 26.4 MB | 25.0 MB | 33.2 MB | 33.4 MB | **1.27×** |
| `group_by` | 27.1 MB | 28.2 MB | 26.6 MB | 33.4 MB | 33.4 MB | **1.23×** |
| `identity` | 28.1 MB | 25.9 MB | 26.6 MB | 33.1 MB | 33.1 MB | **1.18×** |
| `keys_aggregate` | 33.1 MB | 36.8 MB | 38.9 MB | 35.9 MB | 36.0 MB | **1.09×** |
| `length` | 26.2 MB | 26.0 MB | 23.6 MB | 33.1 MB | 33.1 MB | **1.26×** |
| `recurse_paths` | 26.1 MB | 26.1 MB | 23.9 MB | 33.0 MB | 33.1 MB | **1.27×** |
| `sort_by` | 27.1 MB | 27.0 MB | 42.6 MB | 33.2 MB | 37.6 MB | **1.39×** |
| `sum_score` | 26.6 MB | 26.0 MB | 25.8 MB | 33.8 MB | 33.9 MB | **1.27×** |
| `transform` | 30.1 MB | 26.4 MB | 28.8 MB | 34.2 MB | 34.4 MB | **1.14×** |

実用 1.9 MB スケールでは皆 25-40 MB レンジに収まり、ほぼ全部
**スタートアップ + JSON parse + 出力バッファ** の固定コスト。nuq が
一律 ~34MB でやや重いのは AST + 組み込み def 群の永続常駐 (parse-time
の `--no-compile` 状態でも prelude を全部 AST 化する)。jq は 26 MB
付近。比率 1.1-1.3× は 5-10 MB の差で、永続データのレイアウト差。

### Big (input: 25-46 MB / shape)

| bench (shape) | jq | jaq | gojq | nuq int | nuq AOT | **nuq/jq** |
|---|---:|---:|---:|---:|---:|---:|
| `bulk_update` (users) | 492.1 MB | 414.2 MB | 484.8 MB | 542.6 MB | 542.4 MB | **1.10×** |
| `deep_followers` (users) | 393.7 MB | 411.2 MB | 350.2 MB | 493.4 MB | 493.4 MB | **1.25×** |
| `extract_users` (users) | 393.7 MB | 410.9 MB | 349.1 MB | 493.1 MB | 493.1 MB | **1.25×** |
| `group_city` (users) | 402.6 MB | 433.7 MB | 370.7 MB | 494.6 MB | 494.2 MB | **1.23×** |
| `log_error_paths` (logs) | 218.1 MB | 297.3 MB | 217.5 MB | 335.8 MB | 335.9 MB | **1.54×** |
| `log_post_avg` (logs) | 217.3 MB | 293.2 MB | 214.1 MB | 343.8 MB | 343.6 MB | **1.58×** |
| `log_request_id` (logs) | 220.7 MB | 293.4 MB | 222.4 MB | 335.0 MB | 334.9 MB | **1.52×** |
| `sum_active_score` (users) | 392.6 MB | 411.9 MB | 352.5 MB | 498.6 MB | 498.6 MB | **1.27×** |
| `table_flag_count` (table) | 483.4 MB | 360.4 MB | 490.7 MB | 586.6 MB | 587.0 MB | **1.21×** |
| `table_sum_col0` (table) | 511.7 MB | 357.0 MB | 551.5 MB | 586.9 MB | 586.9 MB | **1.15×** |
| `table_unique_colors` (table) | 536.7 MB | 407.7 MB | 552.7 MB | 599.1 MB | 599.2 MB | **1.12×** |
| `tree_leaf_sum` (tree) | 277.0 MB | 300.0 MB | 453.3 MB | 402.4 MB | 402.4 MB | **1.45×** |
| `tree_numbers` (tree) | 286.9 MB | 301.7 MB | 455.6 MB | 385.4 MB | 381.4 MB | **1.33×** |
| `tree_paths` (tree) | 776.1 MB | 780.4 MB | 1.10 GB | 847.1 MB | 843.3 MB | **1.09×** |

100 MB スケールでは全エンジン 200 MB～1.1 GB レンジ。**nuq は jq の
1.1-1.6× に収まる**。これは:

- **入力 JSON は永続領域に保持** — 25-46 MB の入力 + 内部表現
  (jq では 1 値あたり 24-40 byte) で 200-500 MB の常駐は皆共通。
- **中間値は arena + Cheney copying GC** — 走行中にバンドル alloc
  が積もったら threshold で minor 回収して live-set ~2× に圧縮。
  GC 採用前は `[paths]` 系で 11 GB / `reduce ([]; . + [$i])` 系で
  1.4 GB 出ていた所が、jq と同等オーダーに落ちた。
- **gojq の `tree_paths` だけ 1.10 GB に膨らんでいる** のは
  Goランタイムが path 配列を全部保持する gojq 実装の特性。nuq は
  847 MB で gojq に対 0.78×、jq の 1.09× で済む。

### Cheney GC + 線形性解析 — `acc + [$i]` 系クエリの実測

`reduce range(N) as $i ([]; . + [$i]) | length` を `/usr/bin/time -f
'%e %M'` で計測 (時間 = wall-clock 秒, メモリ = peak RSS):

| N | jq | gojq | nuq (Cheney 単独) | **nuq (＋線形性 ★)** |
|---:|---:|---:|---:|---:|
| 1 e5 | 0.04 s / 6.4 MB | 39.7 s / 67.5 MB | 5.72 s / 24 MB | **0.01 s** / 13 MB |
| 1 e6 | 0.34 s / 24.5 MB | (>30 min 見込み) | 1621 s / 110 MB | **0.09 s** / 65 MB |
| 1 e7 | 3.55 s / 252 MB | — | — (時間切れ) | **0.99 s** / 688 MB |

★ 線形性解析を入れると **jq の 3-4× 速い** スケールに乗る。N=1e6 で
0.09 s vs jq 0.34 s。

#### 段階別の効果

1. **何もしない (immutable copy)**: `acc + [$i]` は毎反復で `acc` を
   全コピー → 総 alloc 量は `Σ (i+1) × 8 byte ≈ N²/2 × 8` (N=1e6 で
   4 TB)。N=1e5 程度でも arena が GB スケールに膨らんで OOM。

2. **Cheney GC (本コミット直前まで)**: 1 GC ごとに live-set だけ
   to-space に転送、garbage は recycle。**peak RSS は N に対し
   ほぼ一定** (24-110 MB)。同じ "毎回コピー" 路線の gojq より省メモリ
   (N=1e5 で gojq 67 MB → nuq 24 MB)。
   速度は依然として O(N²)。N=1e5 で 5.72 s (jq の 140× 遅) — alloc は
   速くなったが「コピー量」が変わってないので。

3. **Cheney GC + 線形性解析 (本コミット)**: AST 解析で `acc + [$i]`
   の LHS が "linear" (他から参照されない) と判定できれば
   `node_add` を `node_add_inplace` に書き換え、runtime で
   `nuq_array_push` で in-place 拡張する。**O(N²) → O(N)**。
   N=1e6 で **jq より速い** 0.09 s vs 0.34 s。jq の同じ最適化は
   refcount=1 の動的検出だが、nuq は parse-time に static に
   決定するので runtime の per-op overhead 0。

#### 線形性解析の仕組み (linearity.c)

AST を walk して各 *dot-scope* (pipe RHS / reduce update / foreach
update,extract / 関数 body が境界) について `.` の syntactic read 数
を数える。`reduce range(N) as $i ([]; UPDATE)` の UPDATE が:

- 直接 `node_add(node_identity, RHS)` 形で書かれている
- UPDATE scope 内で他に `.` を読んでいない (RHS、`$x = .` 形の as
  のあとの $x 参照、`.x` フィールドアクセスなど **すべて 0**)

を満たすと、その `node_add` のカインドを `node_add_inplace` に
書き換える。後段の dispatch / AOT は新しいカインドを通常通り
扱う (専用 NODE_DEF が `node.def` にある)。

Runtime の `nuq_op_add_inplace` は `in_arena(LHS)` で安全性を再確認
する: LHS が arena 上の "走行中" 値なら mutate、永続領域の値
(典型は入力 JSON、リテラル) なら copy にフォールバック。これで:

- top-level `. + [99]` (`.` = 入力 JSON、永続) → static 解析は mark
  するが runtime ガードで copy にする (jq 的なセマンティクスを保つ)
- nested の `reduce ... (.; . + [...])` でも、初回 iter は input が
  永続のためコピー、2 回目以降は acc が arena に乗っているので
  in-place — 結果として実用的には N-1 回が in-place

`as` で `.` を別名に縛ると ($a = .) その alias 参照も dot 参照と
してカウントするので、aliased dot を変更する不正は静的にブロック
される (一致しないと mark が外れて copy 経路に落ちる)。

詳細な GC 実装は [runtime.md §5](runtime.md#5-メモリ管理--per-run-arena--cheney-copying-gc)、
線形性解析の実装は `linearity.c` のヘッダコメント参照。

### 計測コマンド

```bash
make
BENCH_MEM=1 ruby bench/bench.rb real    # 実用ベンチ
BENCH_MEM=1 ruby bench/bench.rb big     # big-data ベンチ
```

`/usr/bin/time -f '%M'` を内部で wrap、`BENCH_ATTEMPTS` (default 3)
回中 max を採る (worst-of-N)。

## 解釈

### 大勝の構造

micro 系の桁違いの勝ち:

- **`reverse 1M` 30× / `min-max 1M` 15× / `last 1M` 10× / `upto 8k`
  53×**: tight な C ループに inline されるケース。SD specializer が
  AST を一括で fold-in、range の emit ループ + 集約処理が 1 SD 関数
  に焼かれる。
- **`to-fromjson 100k` 24×**: 手書き JSON parser / printer のスル
  ープット (SIMD scanner で更に伸びた)。
- **`group-by 100k` 20×**: qsort + 安定 group。jq は libjq の dict
  風内部表現で extra alloc がかさむ。

JSONL 系の構造的勝ち:

- **`identity` (純 parse + print) 6.1×**: SIMD string scanner が
  effective。jq の per-byte scanner との差。
- **`count_pushes` 2.6× / `top_users` 2.4×**: 元々 0.3× で大敗
  していた構造的問題を `inputs | F` streaming-pipe fusion で逆転。

実用 (real) bench の field 抽出系 (deep_field / extract_field /
sum_score / length / identity) は **2.6-3.1×**。jaq・gojq に対して
も 1.2-1.5× リード。

### 残る相対的な弱点

#### `upto` で jaq 比 0.7× (構造的)

jaq は recursive `def` を bytecode に compile して tail-call
elimination する。nuq は tree walker なので各 recursive call が C
stack frame + EVAL dispatch。**vs jq では 53× で勝っている** が、
jaq との差は TCE 起因で構造的。CPS 化しても TCE が入るわけではない
ので限界がある。

#### `pyramid 8k` で 0.77×

deep recursion (8000 ネスト) と multi-emit per level の dispatch 圧
で律速。jq も同様に苦手だが、libjq の内部表現が比較的軽いのでわず
かに nuq より速い (jaq / gojq は更に遅い 0.6-0.9×)。pool top の
register 常駐化 (todo B-3) で多少改善見込み。

### nuq AOT vs nuq interp

ほとんどのベンチで AOT は interp と ±5%。**これは jq の構造的な
性質**: jq の hot work は集合演算 (`map / select / sort / add /
group_by`) に集約されていて、その本体は builtin の C ループ。AST
レベルの dispatch コストは支配的でないので、SD specialization で
dispatch を消しても大きな差にならない (interp も builtin に飛び込ん
で同じ C ループに入る)。

例外: **再帰 def の AST が hot loop になる場合** は AOT が伸びる
(§6 「再帰 def の AOT 補助」)。`upto` の AOT vs interp で差が出る。

## 適用済みの主要最適化

トピック別にまとめる (時系列の詳細は git log)。

### 1. 値表現 + alloc

- **EMIT pool**: NODE_DEF が `EMIT { items, count }` を返し、items
  は CTX 上の flat VALUE buffer のスライス。per-emit GC alloc
  ゼロ、SD inline と相性良し。startup で 4096 entries pre-grow、
  以降の realloc は UNLIKELY 経路。`pyramid 8k`: 140× 遅 → 互角。
- **Value 演算 fast path inline**: `nuq_op_add / sub / mul / neg`、
  `nuq_eq`、`nuq_cmp`、`nuq_truthy`、`nuq_make_int` の fixnum
  fast path を `context.h` の `static inline` に切り出し、slow case
  を `_slow` 接尾辞付き関数として `value.c` に残す。`nuq_op_div /
  mod` は jq 仕様で常に double 演算なので fast path なし。
  `min-max 1M` 6.0× → 9.0× / `sort 300k` 3.5× → 5.6× /
  `group-by 100k` 5.3× → 7.1× / `cumsum 500k` 5.0× → 7.1×。
- **Object lookup の lazy hash idx**: `nuq_obj.obj` に `uint32_t
  *idx` を追加 (open-addressing FNV-1a、load factor ≤ 0.5、
  threshold 16)。挿入順 parallel array は維持。`add` builtin の
  `all_objects` fast path で pairwise `nuq_clone` カスケードも
  撲滅。`kv 5k`: 33× 遅 → 1.5× 速。
- **`add` builtin の type-dispatch kernel**: array-only の fast
  path で全長を先に集めて単一 alloc + copy で O(n)。pairwise
  reduction で O(n²) になっていたバグを撲滅。`keys_aggregate`
  11× 遅 → 3.2× 速。
- **Object literal の direct-build fast path**: 全エントリ count==1
  の典型ケースで cartesian iteration を skip、pool 直書きで
  per-entry alloc を節約。static key は parser で `nuq_make_string`
  を 1 度だけ実行 → entry に VALUE で保存。
- **Object literal の dedup-skip 化**: parse 時
  (`nuq_obj_ctor_intern`) に entry を見て、全 entry が `kkind == 0`
  (静的 cstring key) かつ kname が pairwise distinct なら
  `all_distinct_static = true` を立てる。runtime の fast path が
  これを見て `nuq_object_set` の dedup linear scan + PIN3 を skip
  し、専用の `nuq_object_append` で keys[len] / vals[len] 直書き
  + len++ + lazy hash idx update のみ。`{a, b, top_tag: ...}` の
  ような typical jq literal が 30K 回呼ばれるシナリオで効く。

注: 上記の `static inline` は context.h で interp / AOT 両方から
見えるので、interp と AOT が同程度伸びる。AOT が interp を引き離す
動きは jq の workload では出ていない (集合演算が builtin C ループ
に集約しているので AST level の hot loop が薄い)。AOT-only の上振れ
には PGO 系が要るが、AST fusion の方が相性が良い (todo B-5)。

### 2. メモリ管理 (per-run arena + Cheney GC)

詳細は [runtime.md §5](./runtime.md#5-メモリ管理--per-run-arena--cheney-copying-gc)。

- **per-run arena**: 中間 VALUE は bump alloc → 16 MB しきい値で
  Cheney semispace 回収 → run 終了で wholesale reset。永続領域
  (AST / リテラル / `--arg*` / module data) は plain `calloc` で
  プロセス終了まで保持。**libgc 依存なし** (`libm` + `libc` のみ)。
- **線形性解析** (`linearity.c`): `acc + [$i]` 系を AST 静的解析で
  in-place mutation に降格。reduce が O(N²) → O(N)。N=1e6 で
  0.09 s vs jq 0.34 s で**逆転勝ち**。
- **GC scan loop の subtle bug 修正** (correctness): Cheney scan の
  inner `for(;;)` が transition 直前まで現 chunk に積まれた obj を
  scan せずに次 chunk に進む silent corruption があった (10K-scale
  `transform` / `keys_aggregate` の intermittent segfault の根本
  原因)。`if (scan_ptr >= chunk_end) break;` の単純形に書き直し、
  毎反復で chunk_end を再評価。詳細は
  [runtime.md §5.6](./runtime.md#56-cheney-scan-ループの-subtle-bug-修正済み参考)。
- **`in_arena` を O(N chunks) → O(log N)**: GC 開始時に from-space
  chunks を `[lo, hi)` ペアに展開して address 順 qsort、
  `gc_from_min` / `gc_from_max` で envelope pre-check + binary
  search に。`tree_paths` で `gc_forward_value` の 80% 占めだった
  hot spot を消滅、3.27 s → 0.93 s で完走 (前は err)。
- **GC threshold を入力サイズに連動**: `nuq_run` 開始時に
  `arena_gc_threshold = max(2 × arena_total, NUQ_GC_THRESHOLD)` で
  初期値を bump。1.9 MB JSON が ~17 MB の内部表現に膨らんで 16 MB
  閾値で即 GC を踏む overhead を回避。real bench 全 11 件で +30%。
- **`nuq_make_string` を `NUQ_GC_DEFER` で囲う**: caller が arena
  内 byte ptr (slice 系の `s` 引数) を渡すパターンを安全に。
  alloc + memcpy の対を defer で守って source bytes が動かないよう
  にする。`nuq_op_div_slow` の string split、`nuq_slice_eval` の
  string slice 等で必須。

### 3. Parse / print スループット

- **JSON parser の no-escape fast path** (`parse_string_raw`): 開き
  quote の次から閉じ quote までを一度 scan、`'\\'` も control char
  も無ければ入力 span の長さが確定する → 1 回の
  `nuq_make_string(p, len)` で済む (alloc + memcpy 各 1 回)。従来は
  32-byte growable buffer + 段階 realloc + 最後にもう 1 度 take で
  copy していたので alloc 2 回 + copy 2 回。
- **JSON parser の integer fast path** (`parse_number`):
  `[-]?[0-9]+` を inline accumulate して `'.'` / `'e'` / overflow が
  無ければ `NUQ_FIX(ll)` を直に返す。`strtoll` + 中間 buffer copy
  が消える。
- **SIMD string scanner** (`parse_string_raw` / `print_string`):
  inner byte-loop の `'"' / '\\' / <0x20` 探しを SIMD stride に。
  SSE2 (16-byte) と AVX2 (32-byte) の 2 路実装、起動時の
  `__builtin_cpu_supports("avx2")` で関数ポインタを切り替える
  runtime CPU dispatch。

  ```c
  __m128i v = _mm_loadu_si128((const __m128i *)p);
  __m128i a = _mm_cmpeq_epi8(v, _mm_set1_epi8('"'));
  __m128i b = _mm_cmpeq_epi8(v, _mm_set1_epi8('\\'));
  __m128i c = _mm_cmpeq_epi8(_mm_min_epu8(v, _mm_set1_epi8(0x1F)), v);
  int m = _mm_movemask_epi8(_mm_or_si128(_mm_or_si128(a, b), c));
  if (m) { offset += __builtin_ctz(m); break; }
  ```

  SSE2 は x86_64 ABI baseline、AVX2 路は `__attribute__((target("avx2")))`
  で globally に -mavx2 を要求せずに済む。control char 検出は
  `min_epu8(v, 0x1F) == v` で unsigned compare 相当 — UTF-8 高位バイト
  (0x80+) でも正しく false。
- **JSON parser で `nuq_object_set` → `nuq_object_append`**: 元々
  parser は dedup 必須 (jq の dup-key は last-wins) を意識して
  `nuq_object_set` (per-call PIN3 + linear dedup scan) を使っていた。
  実用 JSON で dup key は事実上無いので、`nuq_object_append` (dedup
  scan + PIN3 を skip) に降格。trade-off は dup-keys 病的入力で last-
  wins ではなく "両方 keep" になる点 (jq 公式テストには影響なし)。
  profile 上 `nuq_object_set` 3.8% → 0% (top hot spot から消滅)。

profile 上 `parse_string_raw` 6.4% → 2.6% → 1.7% (SSE2 → AVX2 で
更に削減)。JSONL `identity` (純 parse + print): 3.6× → 6.1× →
**6.5×**。`extract_login`: 2.5× → 5.0× → **5.4×**、
`select_recent_PR`: 2.3× → 4.8× → **5.8×**。AVX2 単独効果は
+5-15% (string heavy ほど効く)。`transform` 系: 50 ms → 30 ms →
20 ms の 2 段改善 (threshold 修正 + parser fast path)。

### 4. AST fusion (parse-time peephole)

`filter.c` の `nuq_make_pipe(lhs, rhs)` で意味保存の書き換え。意味
保存は jq 公式テスト 524 件 + ローカル差分テストで常時チェック。

**汎用ルール**:

- `map(F) | map(G)` → `map(F | G)` (中間配列消去)
- `select(F) | select(G)` → `select(F and G)` (短絡保存)
- `[body] | length` → `emit_count(body)` (中間配列なしで count)
- `[body] | add` → `emit_fold_add(body)` (`add` の type-dispatch
  kernel `nuq_add_fold_items` を共有、外側 array alloc 削除)
- **右辺エッジ fusion**: 左結合 chain `f | g | h` を 1 段ずつ折り
  畳む。`f | sel(a) | sel(b) | sel(c)` のような任意長 chain を
  collapse。

| bench | pre-fusion | post-fusion |
|---|---:|---:|
| `try-catch 500k` | 0.26× | **10.24×** |
| `cumsum 500k` | 5.0× | **7.11×** |
| `min-max 1M` | 9.5× | **9.02×** (横ばい — 既に builtin が hot) |

**`inputs` streaming-pipe fusion** (3 ルール、JSONL workload 専用):

- `pipe(inputs, F)` → `node_b_inputs_pipe(F)`: input を 1 件ずつ
  pull → F に流す → F の emit のみ pool に accumulate。inputs
  全体の materialize を回避。
- `[inputs | F] | length` → `node_b_count_inputs(F)` (Rule 3 サブ):
  F の match を pool にも貯めず count だけ。
- `pipe(inputs_pipe(F), G)` → `inputs_pipe(F | G)` (chain
  absorption): `inputs | F1 | F2 | F3` が単一 per-input loop に
  collapse。`make_pipe(F, G)` で再帰的に走るので
  `select | select` 等の既存 fusion とも合成可能。

意味的に `(inputs | F) | G == inputs | (F | G)` — pipe の結合性。
JSONL bench の構造的大敗を逆転:

| bench | pre-streaming | post-streaming |
|---|---:|---:|
| `count_pushes` | 0.35× | **2.57×** |
| `top_users` | 0.33× | **2.38×** |

### 5. GC root pin protocol + debug infra

helper が VALUE / VALUE[] を arena allocator 越しに保持する箇所は
`NUQ_GC_PIN1` / `NUQ_GC_PIN_ARR` で root 化、Cheney scan 時に
forwarding する。binary op、比較系、`node_pipe` / `node_if` /
`node_as[_pattern]` / reduce / foreach / sort_by / group_by /
unique / paths walker / json parser まで audit 済み。

- **Pin stack の growable 化**: `nuq_gc_roots` (root pin) /
  `nuq_gc_arrs` (PIN_ARR) を fixed size から heap-allocated
  growable へ。元々 65536 / 256 の固定 cap で、後者は **bounds
  check 自体が無く** silent buffer overflow で隣接 global
  (`arena_first` 等) を破壊する landmine だった (深い recursive
  filter `pyramid 8000` で発火)。`realloc` で 4096/256 → ×2 ずつ。
  push の hot path の overhead は cap check 1 回のみ。
- **`node_pipe` の `saved` を共有 PIN_ARR slot に統合**: 元々
  PIN_ARR(local) + PIN1(saved) で per-frame に 2 個積んでいたのを、
  local の末尾 slot に saved を入れて 1 個の PIN_ARR で済ませる。
  深い recursive filter での pin stack 圧を 1/2 に。
- **debug build** (`make gctest`): `-DNUQ_GC_DEBUG_MPROTECT=1
  -DNUQ_GC_DEBUG_STRESS=1` で pin 漏れの早期検出インフラを
  提供。詳細は
  [runtime.md §5.5](./runtime.md#55-debug-build-stale-pointer-を即-segfault-にする)
  参照。production threshold (16 MB) では届かない latent な pin
  漏れがこの mode で表面化 — 本ファイルで列挙した builtin /
  runtime の pin 修正はほぼ全てこの経路で発見。`make test` 370/370
  / `make gctest` 370/370 を維持。

### 6. 再帰 def の AOT 補助

`nuq_user_call` 内の `EVAL(c, fd->body)` は runtime resolved
dispatcher なので、top-level filter SD からは inline できない。各
def 本体を独立 entry として `astro_cs_compile` に登録
(`nuq_compile_all_def_bodies` / `nuq_load_all_def_bodies` in
`runtime.c`)。`upto` の AOT vs interp 差が伸びる。

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
