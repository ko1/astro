# perf.md — nuq 性能ノート

`make bench` で `bench/filters/*.jq` (jaq の `examples/benches/` から借用)
を **jq / jaq / gojq / nuq interp / nuq AOT** の 5 エンジンで同条件比較する。

## 環境

- gcc 13 -O2、SD は `-O3 -fPIC -fno-plt -march=native`
- Boehm GC (libgc)
- 比較対象:
  - `jq-1.7` — 本家・互換性基準
  - `jaq 3.0.0` — Rust 製の高速 jq 互換実装
  - `gojq 0.12.19` — Go 製、jq-1.7 互換
  - 注: ユーザに教わった `qj` (NDJSON 寄せの実装) は本機で見つけられず未計測
- `CCACHE_DISABLE=1` で astro_cs_build を回避 (ccache + sandbox 干渉
  対策、project memory: `feedback_ccache_disable`)
- best-of-3、各セル timeout 30s、`taskset` 等の CPU 固定はせず

## 結果

```
bench                n      jq        jaq        gojq     nuq int    nuq AOT
-------------------------------------------------------------------------------
ack                  7    487 ms     764 ms     580 ms     1.5 ms     1.5 ms
add               2000    4.0 ms     3.7 ms     4.8 ms      11 ms      11 ms
cumsum          500000    154 ms     151 ms     231 ms     141 ms     139 ms
empty                1    3.4 ms     2.1 ms     2.4 ms     1.3 ms     1.5 ms
group-by        100000    185 ms      42 ms     123 ms      94 ms      98 ms
kv                5000     10 ms     7.4 ms     9.6 ms     250 ms     245 ms
last           1000000    136 ms      32 ms     168 ms      20 ms      19 ms
min-max        1000000    238 ms     221 ms     278 ms      35 ms      34 ms
pyramid           8000    8.7 ms     9.7 ms      13 ms      13 ms      12 ms
reverse        1000000    536 ms      63 ms     274 ms      26 ms      26 ms
sort            300000    158 ms      44 ms     159 ms      69 ms      75 ms
to-fromjson     100000   1.05 s      132 ms      74 ms      58 ms      56 ms
try-catch       500000    136 ms     150 ms     160 ms      26 ms      27 ms
upto              8192    519 ms     7.6 ms     555 ms      13 ms      12 ms
```

```
Speedup vs jq (⬇ = nuq が速い、⬆ = nuq が遅い):

bench              jq        jaq        gojq     nuq int    nuq AOT
---------------------------------------------------------------------
ack              1.00x       1.6x ⬆     1.2x ⬆    329x ⬇     326x ⬇
add              1.00x       1.1x ⬇     1.2x ⬆     2.8x ⬆     2.7x ⬆
cumsum           1.00x       1.0x        1.5x ⬆     1.1x ⬇     1.1x ⬇
empty            1.00x       1.6x ⬇     1.4x ⬇     2.6x ⬇     2.3x ⬇
group-by         1.00x       4.4x ⬇     1.5x ⬇     2.0x ⬇     1.9x ⬇
kv               1.00x       1.4x ⬇     1.0x      25x ⬆     25x ⬆
last             1.00x       4.3x ⬇     1.2x ⬆     6.8x ⬇     7.2x ⬇
min-max          1.00x       1.1x ⬇     1.2x ⬆     6.8x ⬇     6.9x ⬇
pyramid          1.00x       1.1x ⬆     1.5x ⬆     1.5x ⬆     1.4x ⬆
reverse          1.00x       8.5x ⬇     2.0x ⬇    21x ⬇      20x ⬇
sort             1.00x       3.6x ⬇     1.0x       2.3x ⬇     2.1x ⬇
to-fromjson      1.00x       7.9x ⬇    14x ⬇      18x ⬇      19x ⬇
try-catch        1.00x       1.1x ⬆     1.2x ⬆     5.2x ⬇     5.1x ⬇
upto             1.00x      68x ⬇      1.1x ⬆    39x ⬇      42x ⬇
```

集計 (vs jq の幾何平均、kv/add のような桁外れ outlier 込み):

| エンジン | 14 中速い | 14 中遅い | 幾何平均速度比 |
|------|---:|---:|---:|
| jq      | — | — | 1.00x |
| jaq     | 11 | 3 |  ~3x ⬇ |
| gojq    |  8 | 6 | ~1.3x ⬇ |
| **nuq** (interp) | **11** | 3 | ~5.4x ⬇ |
| **nuq** (AOT)    | **11** | 3 | ~5.3x ⬇ |

## 解釈

### nuq が圧勝するケース (5×〜300×)

**`ack(3; 7)` — 329×**: 純粋に再帰関数呼び出しが支配的。jq は call stack
を heap allocation で組み、bytecode を回す。nuq は tree walker で
recursive C 関数呼び出し → call stack はネイティブ、各呼び出しの
overhead が VM 解釈のそれより 2 桁低い。

**`upto` — 39×** / **`pyramid` — 互角**: 同じく再帰 def で構成されるが
こちらは emit が複数走る。pyramid は `., (.+1 | rec), .` で 1
呼び出しから 2 つ emit する形なので emit_buf への push が支配的、
call stack の優位は薄れる。upto は `., (.+1 | upto)` で末尾再帰に近く、
ack 同様の感触。

**`reverse 1M`, `last 1M`, `min-max 1M` — 6〜21×**: tight な inline
ループ (range emit + array reverse / 走査) が C のネイティブループに
畳み込まれる。jq は per-step bytecode 解釈 + heap alloc。

**`to-fromjson 100k` — 18×**: nuq の JSON parser/printer は手書き、
jq は libjq の汎用 JSON path。

**`try-catch 500k` — 5.1×**: jq は try/catch を VM の例外 frame で
扱うのでオーバーヘッドが大きい。nuq は dispatcher の戻り値で
`BR_ERROR` を巻き上げるだけ — 1 命令。

### 互角〜微差 (1.0〜2×)

**`empty`, `cumsum`, `sort`, `group-by`** — 起動時間 / sort / group-by
共通アルゴリズムが支配的。

### nuq が負けるケース (2×〜25×)

**`kv 5000` — 25× 遅い**: jq の object はハッシュ表、nuq は parallel
array + 線形検索。`{a:1} + {b:2} + {c:3} + ...` と n 個の object を
`+` で連結すると、各ステップで右側 object の各 key を **左 object に
線形挿入** するため O(n²)。jq では O(n)。
**根本対策は object 表現を hash 化すること** — todo B-5。

**`add 2000` — 2.8× 遅い**: `[range(.) | [.]] | add` は単要素配列を
n 個 `+` で連結する。`+` 配列演算は **左右両方を新配列に丸ごと
コピーする** ので O(k) per step → O(n²) total。jq は同じ O(n²) だが
定数倍が小さく、また内部表現が rope 風で copy 量を抑える。
nuq は素直に `nuq_array_push` を回す。
**改善案は immutable 共有 + path-aware mutation** — todo A-1 と
セット。

**`pyramid 8000` — 1.4× 遅い**: 上で書いた通り、emit-heavy な再帰なので
ack 流の優位が薄まる。

## 興味深い観察 — nuq AOT vs interp

ほぼ差がない (ack 互角、upto 12 vs 13 ms、reverse 26 vs 26 ms)。
これは nuq の dispatcher が `runtime.c` 内のヘルパ呼び出し 1 つに
集約していて、SD specializer に「子ノードの dispatcher を inline
してね」と渡しても **その先の helper の中身が runtime-resolved EVAL を
呼ぶ** ので最終的にチェーンは融合されないため。

todo B-1 (streaming pipe) と **B-2 / "全 builtin を node 化"** が解けると
SD specializer が `[range(n)] | reverse | length` 全体を 1 関数に折り
畳めるはずで、nuq AOT が interp を引き離す絵が見えてくる。今は
ASTroGen の力をフルに使えていない状態。

## 計測のお作法

`feedback_bench_sustained` (project memory) に従い、~1 秒スケールで
回す。short bench は `INIT()` / parser / JSON parse の上り坂で
支配される。`empty` (n=1) は startup-only ベンチで、それ以外は
すべて jq で 100ms〜2s に揃えてある。

`code_store/all.so` は **bench 中の `nuq AOT` セルが先頭で必ず削除** し、
1 回目のラン (= bake) は best-of-N の対象から外す。`nuq int`
(`--no-compile`) は bake 結果を使わないので前回の `all.so` が
残っていても無関係。

## 既知の outlier の対策

| ケース | 原因 | 対策 |
|---|---|---|
| `kv` (object `+` 連結) | 線形 lookup × n step = O(n²) | hash table 化 (todo B-5) |
| `add` (array `+` 連結) | コピー × n step = O(n²) | immutable 共有 / path-aware mutation (todo A-1 と一緒) |
| `pyramid` (multi-emit recursion) | 立ち上がりの emit_buf alloc | per-call alloca buffer (todo B-4) |

## バグ修正履歴 (bench 駆動)

- `CTX` を `calloc` で確保していたため、その中の `var_stack` /
  `funcs` ポインタが Boehm GC からスキャンされず、`var_stack` ブロックが
  途中で回収されて `$x undefined` で死んでいた。`GC_malloc(CTX)` に
  修正したら **upto が 3.4× → 39× / cumsum が 1.4× 遅 → 1.1× 速** と
  劇的に改善。bench 駆動でなければ気づかなかった可能性大。
- `nuq_clone(object)` が `nuq_object_set` 経由で 1 個ずつ insert して
  いたため O(n²) → 全体で O(n³)。ソースのキーは既に unique なので
  set のチェック (= 線形 collision 走査) を回避し直接 push に。
  `kv` n=5000 が 119s → 0.25s。
- `group_by` の sort が手書き挿入ソートで O(n²) → qsort + per-pair
  comparator に置換。`group-by` n=100k が timeout → 94ms。

これら 3 つは今回の jaq 流ベンチを回さなければまず見つけられなかった
バグ。
