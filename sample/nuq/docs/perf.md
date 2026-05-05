# perf.md — nuq 性能ノート

`make bench` で **2 つのスイート** を実行する:

1. `bench/real/` — **実用ワークロード**: ~1.9 MB / 10k user オブジェクトの
   JSON ファイル (`bench/data/users.json`) に対する典型的 jq クエリ。
   field 抽出、`select`、`group_by`、`sort_by`、`map` などの組合せ。
2. `bench/filters/` — **micro-bench**: jaq の `examples/benches/` から
   借用した単一機能テスト (ack / range / reverse / sort 等)。`n` を
   stdin で渡す。

計測は **whole-process wall time** (Open3 + Process.wait):
- shell spawn / exec
- フィルタ式 parse
- 入力 JSON parse (real のみ)
- フィルタ評価
- 出力書き出し
- プロセス exit

を全部含む。`time ./nuq ...` がユーザに見える時間と同じ。

## 環境

- gcc 13 -O2、SD は `-O3 -fPIC -fno-plt -march=native`
- Boehm GC (libgc)
- 比較対象: `jq-1.7`、`jaq 3.0.0`、`gojq 0.12.19`
- best-of-3、各セル timeout 30s、`taskset` 等の CPU 固定はせず
- `CCACHE_DISABLE=1` (AOT セルのみ) — sandbox + ccache の干渉対策

## Real-world bench (input: users.json, ~1.9 MB / 10k users)

絶対値:

| bench | jq | jaq | gojq | nuq int | nuq AOT |
|---|---|---|---|---|---|
| `deep_field` (`[.[] \| .stats.followers] \| add`) | 83 ms | 66 ms | 58 ms | 50 ms | 51 ms |
| `extract_field` (`[.[] \| .name] \| length`) | 75 ms | 63 ms | 56 ms | 48 ms | 48 ms |
| `filter_count` (`[.[] \| select(.active and .age > 30)] \| length`) | 85 ms | 74 ms | 56 ms | 58 ms | 56 ms |
| `group_by` (`group_by(.city) \| map({city: .[0].city, count: length})`) | 90 ms | 75 ms | 63 ms | 51 ms | 48 ms |
| `identity` (`.`) | 90 ms | 86 ms | 70 ms | 61 ms | 62 ms |
| `keys_aggregate` (`[.[] \| keys] \| add \| unique \| length`) | 368 ms | 149 ms | 118 ms | 100 ms | 87 ms |
| `length` (`length`) | 63 ms | 54 ms | 49 ms | 43 ms | 41 ms |
| `recurse_paths` (`.[0] \| [paths] \| length`) | 65 ms | 54 ms | 48 ms | 40 ms | 40 ms |
| `sort_by` (`sort_by(.score) \| .[-10:] \| map(.name)`) | 89 ms | 81 ms | 139 ms | 385 ms | 406 ms |
| `sum_score` (`[.[] \| .score] \| add`) | 72 ms | 63 ms | 66 ms | 49 ms | 47 ms |
| `transform` (`map({name, email, top_tag: .tags[0]})`) | 99 ms | 103 ms | 64 ms | 71 ms | 72 ms |

vs jq (≧1.0 = nuq の方が速い):

| bench | jq | jaq | gojq | **nuq AOT** |
|---|---|---|---|---|
| `deep_field` | 1.00x | 1.25x | 1.43x | **1.62x** |
| `extract_field` | 1.00x | 1.19x | 1.34x | **1.58x** |
| `filter_count` | 1.00x | 1.15x | 1.50x | **1.51x** |
| `group_by` | 1.00x | 1.21x | 1.44x | **1.88x** |
| `identity` | 1.00x | 1.04x | 1.29x | **1.46x** |
| `keys_aggregate` | 1.00x | 2.47x | 3.13x | **4.24x** |
| `length` | 1.00x | 1.18x | 1.30x | **1.54x** |
| `recurse_paths` | 1.00x | 1.22x | 1.37x | **1.62x** |
| `sort_by` | 1.00x | 1.10x | 0.64x | **0.22x** ⬇ |
| `sum_score` | 1.00x | 1.14x | 1.10x | **1.55x** |
| `transform` | 1.00x | 0.96x | 1.55x | **1.39x** |

実用ワークロード 11 中 **10 で jq 越え**、`sort_by` だけが 4× 遅い
(insertion sort のままなので O(n²); todo)。`keys_aggregate` は jaq /
gojq よりも速い (`add` 配列連結を O(n) 化したため)。

## Micro-bench (jaq examples/benches; input = scalar n via stdin)

絶対値:

| bench | n | jq | jaq | gojq | nuq int | nuq AOT |
|---|---|---|---|---|---|---|
| `ack` (`ack(3; .)`) | 7 | 509 ms | 702 ms | 561 ms | 421 ms | 427 ms |
| `add` (`[range(.) \| [.]] \| add \| length`) | 2k | 3.7 ms | 2.7 ms | 3.0 ms | 1.6 ms | 1.9 ms |
| `cumsum` (`[foreach range(.) as $x (0; . + $x)] \| length`) | 500k | 146 ms | 147 ms | 222 ms | 139 ms | 143 ms |
| `empty` (`empty`) | 1 | 2.9 ms | 1.6 ms | 2.5 ms | 1.3 ms | 1.2 ms |
| `group-by` (`group_by(. % 2) \| length`) | 100k | 168 ms | 39 ms | 109 ms | 71 ms | 65 ms |
| `kv` (`[range(.) \| {(tostring): .}] \| add \| length`) | 5k | 8.2 ms | 7.2 ms | 8.2 ms | 242 ms | 244 ms |
| `last` (`last(range(.))`) | 1M | 121 ms | 28 ms | 153 ms | 17 ms | 18 ms |
| `min-max` (`[range(.)] \| min, max`) | 1M | 213 ms | 205 ms | 244 ms | 30 ms | 32 ms |
| `pyramid` (recursive multi-emit) | 8k | 7.7 ms | 8.6 ms | 11 ms | 1.14 s | 1.14 s |
| `reverse` (`[range(.)] \| reverse \| length`) | 1M | 495 ms | 54 ms | 243 ms | 21 ms | 23 ms |
| `sort` (`[range(.) \| -.] \| sort \| length`) | 300k | 142 ms | 39 ms | 138 ms | 73 ms | 79 ms |
| `to-fromjson` (`[range(.) \| tojson] \| join \| fromjson`) | 100k | 988 ms | 113 ms | 66 ms | 57 ms | 58 ms |
| `try-catch` (`[range(.) \| try error catch .] \| length`) | 500k | 120 ms | 137 ms | 134 ms | 641 ms | 627 ms |
| `upto` (recursive def) | 8k | 476 ms | 6.6 ms | 503 ms | 441 ms | 433 ms |

vs jq:

| bench | n | jq | jaq | gojq | **nuq AOT** |
|---|---|---|---|---|---|
| `ack` | 7 | 1.00x | 0.73x | 0.91x | **1.19x** |
| `add` | 2k | 1.00x | 1.34x | 1.23x | **1.94x** |
| `cumsum` | 500k | 1.00x | 1.00x | 0.66x | **1.03x** |
| `empty` | 1 | 1.00x | 1.81x | 1.15x | **2.39x** |
| `group-by` | 100k | 1.00x | 4.29x | 1.53x | **2.57x** |
| `kv` | 5k | 1.00x | 1.14x | 1.00x | **0.03x** ⬇ |
| `last` | 1M | 1.00x | 4.38x | 0.79x | **6.87x** |
| `min-max` | 1M | 1.00x | 1.04x | 0.87x | **6.63x** |
| `pyramid` | 8k | 1.00x | 0.89x | 0.69x | **0.01x** ⬇ |
| `reverse` | 1M | 1.00x | 9.15x | 2.03x | **21.31x** |
| `sort` | 300k | 1.00x | 3.66x | 1.03x | **1.81x** |
| `to-fromjson` | 100k | 1.00x | 8.74x | 15.07x | **17.07x** |
| `try-catch` | 500k | 1.00x | 0.88x | 0.90x | **0.19x** ⬇ |
| `upto` | 8k | 1.00x | 71.71x | 0.95x | **1.10x** |

micro 14 中 11 で jq 越え (kv / pyramid / try-catch が大きく劣る)。

## 解釈

### 大勝の構造

`reverse 1M` (21×)、`min-max 1M` (6.6×)、`last 1M` (6.9×) は
**tight な C ループに inline される** ケース。SD specializer が
`[range(.)] | reverse | length` の AST を一括で fold-in、range の
emit ループと reverse の copy ループ + length の単一読み出しが 1 SD
関数に焼かれる。`to-fromjson 17×` は手書き JSON parser/printer の
スループット (jq の libjq 比)。

`group_by 2.4×` は qsort + 安定 group。jq は libjq の dict 風内部表現
で extra alloc がかさむ。

実用ベンチの普通の field 抽出系 (deep_field / extract_field / sum_score
/ length / identity) はだいたい **1.4-1.7× 速い**。jaq・gojq とは横並
び (彼らも 1.2-1.5×)。これらは「jq の C 実装と nuq の C 実装の per-op
オーバーヘッド差」を測っているとも言える。

### 既知の outlier

#### `pyramid 8k`: 140× 遅い ⬇⬇⬇

`def pyramid($max): def rec: if . < $max then ., (.+1 | rec), . end; rec`

各レベルで 3 emit (self, recursive, self again)、深さ 8000。総 emit 量は
O(n²) の 64M。 nuq の各 emit は `nuq_make_array(1)` を 1 個 alloc する
のでメモリ圧が極大。jq は emit ごとの alloc が refcount-shared 値 で
nuq より遥かに軽い。

**根本対策**: emit を heap 配列で持つのではなく per-call alloca buffer
+ overflow spill に切替 (todo B-4)。

#### `kv 5k`: 33× 遅い ⬇⬇⬇

`[range(.) | {(tostring): .}] | add` で n 個の単 key オブジェクトを
連結。my object は parallel array + 線形 collision 走査なので、`+` の
連鎖が O(n²) lookup。

**根本対策**: object lookup を hash 化 (todo B-5)。今回 array 連結は
fast path で O(n) 化したが、object についてはまだ。

#### `try-catch 500k`: 5× 遅い ⬇

各 try が `nuq_make_array(...)` を数回 alloc。500k 回繰り返すので
GC 圧が支配的。**try-catch 自体の評価は速い** (例外処理機構なし、
ただの c->error チェック) が、emit array 構築のメモリ alloc コストが
全体を律速。alloca buffer (todo B-4) で改善見込み。

#### `sort_by` (real): 4× 遅い ⬇

`sort_by(.score)` は insertion sort (O(n²)) のまま実装している。
n=10000 では 10^8 ops。qsort 化 (key 配列 + 平行 index ソート) が
todo (簡単に直せるはず)。

#### `upto 8k`: 互角だが jaq に大敗 (1.1× / 71×)

jaq は recursive `def` を bytecode に compile して tail-call elimination
する。nuq は tree walker なので各 recursive call がスタックフレーム +
EVAL ディスパッチ。jaq との 65× 差はこの構造的なもの。

### nuq AOT vs nuq interp

ほとんどのケースで **AOT ≈ interp** (差は -3% 〜 +5% 程度)。これは:

1. SD bake コスト (~80 ms 起動時 1 回) が短期ベンチでは観測される。
2. nuq のホットパスは既に C 関数 (NODE_DEF dispatcher) なので、SD
   specialize の win は inline 経由の constant-folding 程度。
3. heap allocation (per-emit `nuq_make_array`) が支配的なケースでは
   どんなに inline されても allocator がボトルネック。

それでも `keys_aggregate` (4.24× vs interp の 3.67×) や `group_by` (1.88×
vs 1.77×) のような chain で AOT の方が微差で速い場合がある — gcc の
inline 効果が出ている兆候。

### CPS chain にしないとさらなる win は厳しい

todo B-1: pipe を CPS 化 (各段が next continuation を呼び出し、emit
materialization を消す)。これで:
- per-stage の emit 配列 alloc が消える
- pipe 全体が真に 1 SD 関数化される (現在は pipe ノードの SD 内に lhs/rhs
  の SD は inline されるが、各段で emit 配列を heap に作って iterate)

これが実装されると、`reverse 1M` 的な benchmark で interp と AOT の差が
顕著になる見込み。

## 設計上の妥協 (現状)

- emit を **immutable nuq_array** で表現 (per-stage heap alloc)。Pros: 単純、
  fan-out が自然に書ける、SD inlining と相性が良い。Cons: alloc 量。
- pipe は **lhs を一旦配列に集めて iterate**。streaming にはなって
  いないが、実用 JSON サイズ (~10MB 以下) では問題にならない。
- object は parallel array + 線形 lookup。小さい object には十分速い、
  大きい object (kv bench) で破綻する。

## 計測のお作法

- 1 セルあたり best-of-3。
- 全エンジンで同じ stdin / stdout 経路 (Open3 popen3)。
- timeout は 30s/cell (kv 5000 で nuq が ~250ms、pyramid 8000 で 1.1s)。
- AOT セルだけ `code_store/all.so` を bench 開始時に削除し、bake (1
  attempt) → cached (3 attempts best) で計る。

## バグ修正履歴 (bench 駆動)

bench を整備して見つけた問題:

- **再帰関数呼び出しの var binding バグ** — `f(a; b)` で a を eval して
  $param に bind した状態で b を eval していた。jq では a/b はどちらも
  caller scope で eval されるべき。修正で ack(3; n) の値が真値に。
- **`nuq_args_intern` に stack-local 配列を渡していた** — parser の
  args[8] スタック配列を side-table に置いていて、parse 後に dangle。
  GC 管理 heap にコピーするように修正で 2-arg 以上の def call が
  segfault しなくなった。
- **`add` of arrays が O(n²)** — pairwise reduction が累積配列を毎回
  copy。array-only の fast path で全部の長さを先に集めて単一 alloc +
  copy にして O(n)。`keys_aggregate` が 11× 遅 → 4.24× 速 と劇的改善。
- **`if c then t end` (no else) で segfault** — generated dispatcher が
  els->head.dispatcher を unconditionally deref。parser が常に
  `node_identity` を default として置くように。
- **`f?` (no catch handler) で segfault** — 同上、handler に
  `node_empty` の sentinel を必ず置く。
- **`foreach SRC as $x (INIT; UPDATE)` (no extract)** — 同上、parser が
  `node_identity` を default extract に。

これらは micro-bench を整備しなければ見つからなかったバグ。
