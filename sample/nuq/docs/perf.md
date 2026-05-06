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
| `deep_field` (`[.[] \| .stats.followers] \| add`) | — | — | — | — | — |
| `extract_field` (`[.[] \| .name] \| length`) | — | — | — | — | — |
| `filter_count` (`[.[] \| select(.active and .age > 30)] \| length`) | — | — | — | — | — |
| `group_by` (`group_by(.city) \| map({city: .[0].city, count: length})`) | — | — | — | — | — |
| `identity` (`.`) | — | — | — | — | — |
| `keys_aggregate` (`[.[] \| keys] \| add \| unique \| length`) | — | — | — | — | — |
| `length` (`length`) | — | — | — | — | — |
| `recurse_paths` (`.[0] \| [paths] \| length`) | — | — | — | — | — |
| `sort_by` (`sort_by(.score) \| .[-10:] \| map(.name)`) | — | — | — | — | — |
| `sum_score` (`[.[] \| .score] \| add`) | — | — | — | — | — |
| `transform` (`map({name, email, top_tag: .tags[0]})`) | — | — | — | — | — |

(絶対値はランごとの variance が ±10% 程度あるので倍率テーブルだけ
維持。ms は `make bench` の出力を参照。)

vs jq (≧1.0 = nuq の方が速い):

| bench | jq | jaq | gojq | **nuq AOT** |
|---|---|---|---|---|
| `deep_field` | 1.00x | 1.09x | 1.22x | **1.40x** |
| `extract_field` | 1.00x | 1.11x | 1.30x | **1.38x** |
| `filter_count` | 1.00x | 0.99x | 1.22x | **1.39x** |
| `group_by` | 1.00x | 1.15x | 1.35x | **1.49x** |
| `identity` | 1.00x | 1.04x | 1.25x | **1.50x** |
| `keys_aggregate` | 1.00x | 2.76x | 3.16x | **3.56x** |
| `length` | 1.00x | 1.20x | 1.29x | **1.33x** |
| `recurse_paths` | 1.00x | 1.21x | 1.34x | **1.41x** |
| `sort_by` | 1.00x | 1.16x | 0.63x | **1.50x** |
| `sum_score` | 1.00x | 1.18x | 1.37x | **1.48x** |
| `transform` | 1.00x | 0.92x | 1.47x | **1.47x** |

実用ワークロード **11 中 11 すべてで jq 越え** (1.3-3.4×)。
`transform` の改善 (1.31→1.50×) は object literal の fast path
(per-emit `GC_malloc(sizeof(VALUE))` 撲滅 + static key の VALUE
pre-intern)。`keys_aggregate` は jaq / gojq より速い。

## Micro-bench (jaq examples/benches; input = scalar n via stdin)

絶対値:

| bench | n | jq | jaq | gojq | nuq int | nuq AOT |
|---|---|---|---|---|---|---|
| `ack` (`ack(3; .)`) | 7 | 450 ms | 627 ms | 528 ms | 60 ms | 53 ms |
| `add` (`[range(.) \| [.]] \| add \| length`) | 2k | 2.9 ms | 2.3 ms | 2.5 ms | 1.1 ms | 1.2 ms |
| `cumsum` (`[foreach range(.) as $x (0; . + $x)] \| length`) | 500k | 134 ms | 125 ms | 186 ms | 18 ms | 18 ms |
| `empty` (`empty`) | 1 | 2.5 ms | 1.3 ms | 1.7 ms | 1.1 ms | 1.1 ms |
| `group-by` (`group_by(. % 2) \| length`) | 100k | 146 ms | 32 ms | 87 ms | 20 ms | 19 ms |
| `kv` (`[range(.) \| {(tostring): .}] \| add \| length`) | 5k | 6.9 ms | 5.9 ms | 8.1 ms | 5.0 ms | 4.4 ms |
| `last` (`last(range(.))`) | 1M | 118 ms | 28 ms | 143 ms | 13 ms | 12 ms |
| `min-max` (`[range(.)] \| min, max`) | 1M | 200 ms | 183 ms | 211 ms | 19 ms | 20 ms |
| `pyramid` (recursive multi-emit) | 8k | 6.2 ms | 6.6 ms | 8.7 ms | 6.1 ms | 6.7 ms |
| `reverse` (`[range(.)] \| reverse \| length`) | 1M | 440 ms | 49 ms | 213 ms | 25 ms | 26 ms |
| `sort` (`[range(.) \| -.] \| sort \| length`) | 300k | 122 ms | 34 ms | 119 ms | 22 ms | 22 ms |
| `to-fromjson` (`[range(.) \| tojson] \| join \| fromjson`) | 100k | 844 ms | 107 ms | 61 ms | 45 ms | 42 ms |
| `try-catch` (`[range(.) \| try error catch .] \| length`) | 500k | 109 ms | 125 ms | 122 ms | 11 ms | 8.5 ms |
| `upto` (recursive def) | 8k | 442 ms | 5.0 ms | 421 ms | 7.3 ms | 8.5 ms |

vs jq:

| bench | n | jq | jaq | gojq | **nuq AOT** |
|---|---|---|---|---|---|
| `ack` | 7 | 1.00x | 0.72x | 0.85x | **8.43x** |
| `add` | 2k | 1.00x | 1.25x | 1.17x | **2.41x** |
| `cumsum` | 500k | 1.00x | 1.07x | 0.72x | **7.54x** |
| `empty` | 1 | 1.00x | 1.93x | 1.48x | **2.31x** |
| `group-by` | 100k | 1.00x | 4.61x | 1.67x | **7.75x** |
| `kv` | 5k | 1.00x | 1.16x | 0.85x | **1.55x** |
| `last` | 1M | 1.00x | 4.28x | 0.83x | **9.69x** |
| `min-max` | 1M | 1.00x | 1.09x | 0.95x | **10.24x** |
| `pyramid` | 8k | 1.00x | 0.95x | 0.72x | **0.93x** |
| `reverse` | 1M | 1.00x | 8.98x | 2.06x | **17.24x** |
| `sort` | 300k | 1.00x | 3.56x | 1.03x | **5.62x** |
| `to-fromjson` | 100k | 1.00x | 7.87x | 13.91x | **20.17x** |
| `try-catch` | 500k | 1.00x | 0.87x | 0.89x | **12.89x** |
| `upto` | 8k | 1.00x | 88.56x | 1.05x | **51.87x** |

micro 14 中 13 で jq 越え (pyramid のみ 0.93× で互角ライン、それ以外
はすべて 1.5× 〜 50×+)。

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

### 解消済み outlier (履歴)

各々の直し方は `done.md` の v0.1 / v0.2 / v0.3 節に詳細あり。簡略
まとめ:

- **`pyramid 8k`**: 140× 遅 → 0.93× (互角ライン)。NODE_DEF が VALUE
  返しで per-emit `nuq_make_array(1)` を割っていたのを EMIT pool に
  切り替え。
- **`kv 5k`**: 33× 遅 → 1.5× 速。object lookup を open-addressing
  hash idx 化 (FNV-1a, threshold 16, lazy build) + `add` builtin に
  `all_objects` fast path (pairwise clone カスケード解消)。
- **`try-catch 500k`**: 4× 遅 → 12-14× 速。pre-existing バグで
  `error` (0-arg) の builtin 登録漏れ → user-call dispatch fail loop で
  毎 iter stderr 出力。`BUILTIN0("error", ALLOC_node_error0)` 1 行追加。
  さらに AST fusion で `[X] | length` を `emit_count(X)` に潰した
  分も乗ってる。
- **`sort_by` (real)**: 4× 遅 → 1.5× 速。insertion sort を qsort +
  key 配列 + 平行 index ソートに。
- **`min-max / sort / group-by`**: 30-60% の更なる縮小。`nuq_cmp` /
  `nuq_op_add` の fixnum fast path を `static inline` 化して関数
  call を消去。

### 残る相対的な弱点

#### `upto` で jaq 比 0.6-0.8× レンジ (構造的)

jaq は recursive `def` を bytecode に compile して tail-call elimination
する。nuq は tree walker なので各 recursive call が C stack frame +
EVAL dispatch。**vs jq では 50-60× で勝っている** が、jaq との差は
TCE 起因で構造的。CPS 化 (todo B-1) しても TCE が入るわけではない
ので限界がある。

#### `pyramid 8k` で 0.93× (互角ライン)

各 emit 自体は EMIT pool で軽いが、deep recursion (8000 ネスト) と
multi-emit per level の dispatch 圧で律速。jq は libjq の値ハンドリ
ングが重いので互角に収まる。pool top の register 常駐化 (todo B-4)
で多少改善見込み。

### nuq AOT vs nuq interp

ほとんどのベンチで AOT は interp と ±5% 程度。**これは jq の構造的
な性質**: jq の hot work は集合演算 (`map / select / sort / add /
group_by`) に集約されていて、その本体は builtin の C ループ。AST
レベルの dispatch コストは支配的でないので、SD specialization で
dispatch を消しても大きな差にならない。

例外: **再帰 def の AST が hot loop になる場合** は AOT が伸びる。
これを引き出すために `def` 本体を独立 entry として AOT 登録している
(`nuq_compile_all_def_bodies` / `nuq_load_all_def_bodies` in
`runtime.c`)。`nuq_user_call` 内の `EVAL(c, fd->body)` は runtime
resolved dispatcher 経由なので、top-level filter の SD からは inline
できない — 個別 entry にすると再帰呼び出しが SD コードを叩く。

該当ベンチ (run variance あり、最良ケース):

- `upto 8k` (tail-ish recursive def): interp 60× → AOT 65-70× vs jq
- `ack 7` (recursive 2 引数 def): interp 7.5× → AOT 8.4× vs jq
- `pyramid 8k` (recursive multi-emit): interp 1.0× → AOT 0.93-1.04×

それ以外のベンチでは差は noise レンジ。

実装メモ: `astro_cs_compile(filter)` は root の SD を生成するが、
`fd->body` のような **runtime dispatch で到達するノード** には
別途 entry registration が必要 — usage.md "Entry nodes" の規則。
nuq では parser が `nuq_def_block_intern` で集めた `def_tab` を
walk して全 body を登録している。

### value 演算の header inline 化 (実施済み)

pipe は既に 1 個の `node_pipe(lhs, rhs)` で SD specializer は
EVAL_ARG 経由で両側に入れる。「pipe 全体が 1 SD 関数になる」状態は
**もう達成されている**。CPS pipe は per-stage の EMIT 配列を消すこと
で alloc pattern を変える話で、specialization の到達範囲には関係
しない (interp も同じ alloc を使うので AOT vs interp の差にも効か
ない)。

代わりに hot な opaque-call の解消をやった。`value.c` の fast path
を `context.h` の `static inline` に切り出し、slow case は `_slow`
接尾辞付きの関数として `value.c` に残す:

```c
static inline VALUE
nuq_op_add(VALUE a, VALUE b) {
    if (LIKELY(NUQ_IS_FIX(a) && NUQ_IS_FIX(b))) {
        int64_t la = NUQ_FIX_VAL(a), lb = NUQ_FIX_VAL(b), r;
        if (LIKELY(!__builtin_add_overflow(la, lb, &r)))
            return nuq_make_int(r);
    }
    return nuq_op_add_slow(a, b);
}
```

inline 対象: `nuq_op_add / sub / mul / neg`、`nuq_eq`、`nuq_cmp`、
`nuq_truthy`、`nuq_make_int`。`nuq_op_div / mod` は jq 仕様で常に
double 演算 (5/2 == 2.5) なので fast path がうま味なし、slow に
そのまま流す。

効果 (vs jq):
- `min-max 1M`: 6.0× → **9.7×** (33ms → 21ms)
- `sort 300k`: 3.5× → **5.6×** (36ms → 24ms)
- `group-by 100k`: 5.3× → **7.3×** (29ms → 21ms)
- `cumsum 500k`: 5.0× → 5.6×

`nuq_cmp` / `nuq_op_add` を hot loop で呼ぶ系 (sort, group-by,
min-max, cumsum) で 30-60% の絶対値短縮。

**正直なところ: AOT は interp を引き離さなかった。** `static inline`
は両者に見える (context.h を両方が include) ので、interp dispatcher
のループも AOT の SD コードも同じく fast path を踏む。AOT-only の
上振れには **PGO 的な機構 (型 feedback + 仮定埋め込み + guard)**
が要る — 静的型情報じゃなく、観測した common case を SD に焼き
込む方向。ASTro framework には既に部品 (`HOPT(n)` ハッシュ、
`swap_dispatcher`、`hopt_index.txt`、`#ifndef NODE_SKIP_COLD`、
`-p / --pg-compile` driver) が揃っていて、luastro / naruby が実用
してる。nuq には未配線。

ただし jq は集合演算が中心で AST level の hot loop が薄いので、
PGO よりも **AST fusion (parse 時 peephole)** の方が相性がいい
(下節)。

### AST fusion (parse-time peephole)

`filter.c` の `nuq_make_pipe(lhs, rhs)` で 4 ルール + 右辺エッジ
fusion を実装:

- `map(F) | map(G)` → `map(F | G)` (中間配列消去)
- `select(F) | select(G)` → `select(F and G)` (短絡保存)
- `[body] | length` → `emit_count(body)` (専用ノード追加)
- `[body] | add` → `emit_fold_add(body)` (`add` の type-dispatch
  kernel `nuq_add_fold_items` を共有して outer array alloc 削減)
- **右辺エッジ fusion**: 左結合 chain を 1 段ずつ折り畳む。
  parse loop の毎 iteration で `make_pipe(lhs, rhs)` が呼ばれる
  ので、lhs が pipe ならその rhs と新 rhs を再 fusion 試行 →
  成功時に splice 戻す。`f | sel(a) | sel(b) | sel(c)` のような
  任意長 chain が左から順に collapse する。

意味保存は jq との差分テスト 169 件で確認。`emit_count` ルールが
`try-catch 500k` の 500k 回の中間配列 alloc を一掃して劇的に効いた:

| bench | pre-fusion | post-fusion |
|---|---:|---:|
| `try-catch 500k` | 8.79× | **12.89×** (peak 14×、variance あり) |
| `cumsum 500k` | 5.62× | **6.95×** |
| `keys_aggregate` (real) | 3.25× | **3.56×** (`[X] \| add` fusion) |
| `min-max 1M` | 9.48× | **10.24×** |
| `extract_field` (real) | 1.37× | 1.49× (`[X] \| length` fusion) |

`map(F) | map(G)` と `select | select` のルールは現行ベンチに
直接は出ないが、`def f: map(...) | map(...)` のようなユーザ
コードでは頻出パターン (右辺エッジ fusion で 3 段以上の chain も
拾える)。todo.md B-7 に追加候補ルール (`[body] | unique`、
`sort_by | first`、`reverse | length` など) をメモ。

## 設計上の妥協 (現状)

- emit は **CTX 上の flat VALUE pool** からのスライス (`EMIT { items,
  count }`)。各 NODE_DEF は pool top0 を保存 → push → return。後始末
  は呼び出し側が `c->pool_top = top0` で巻き戻す stack-discipline。
  Pros: per-emit GC alloc ゼロ、SD inlining 容易。Cons: pool 巻き戻し
  忘れがバグる。pool は startup で 4096 entries に pre-grow。
- pipe は **lhs を一旦配列に集めて iterate**。streaming にはなって
  いないが、実用 JSON サイズ (~10MB 以下) では問題にならない。
- object は **挿入順 parallel array + lazy hash idx**。small (≤16) は
  linear、超えたら open-addressing の uint32_t idx[] を build。jq
  互換のため keys は順序保持、`keys` / `to_entries` / iteration は
  常に挿入順。

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
