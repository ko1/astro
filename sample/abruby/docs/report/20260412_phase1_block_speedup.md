# Block/yield 高速化 — Phase 1 のみ採用、Phase 3 は差し戻し

2026-04-12. ブロック実装直後（`4a8c954 abruby: implement blocks, yield, and core iterators (Phase 1)`）の性能を改善するセッション。計画していた 2 段階 (Phase 1: 非ローカル return の再設計 / Phase 3: iterator+block 融合) のうち、**Phase 1 のみ採用、Phase 3 は設計上の問題で差し戻し**。

## ユーザ要求

1. `CTX::frame_id_counter` と `abruby_frame::frame_id` を消す。毎フレーム計算するのはナンセンス。
2. 代わりに「何回 return catch を skip するか」を RESULT に埋め込む。0 の時の return が真の脱出地点。
3. **通常の method call/return で一切 performance regression がない形にする**。

## Phase 1: RESULT skip-count 方式の非ローカル return ✅ 採用

commit: `1201eba abruby: encode non-local return skip count in RESULT state`

### やったこと

- `abruby_frame` から `frame_id` フィールドを削除
- `CTX` から `frame_id_counter` / `return_target_frame_id` を削除
- `RESULT.state` の上位ビット (`RESULT_SKIP_SHIFT=16` 以降) を非ローカル return の skip-count に使用
- `node_return` はブロック内なら `c->current_frame` から `c->current_block->defining_frame` まで `prev` を辿って distance を数え skip-count として state に埋める
- method 境界 catch は `if (UNLIKELY(r.state))` で弾く — 通常 return は `r.state == 0` で即抜け、CTX ロードゼロ
- `node_yield` / `abruby_yield` は `current_frame` のスワップをやめ、`current_block_frame == current_frame` という判定で block 状態を表現 — `dispatch_method_frame` は block 状態を一切 save/restore しない
- 新ヘルパー `abruby_context_frame(c)` で yield / super / block capture が lexical enclosing frame を取得

### 通常 method call/return のコスト比較

| ステップ | 変更前 | 変更後 |
|---|---|---|
| frame push | `frame_id = ++counter` (CTX RMW + store) | なし |
| method 境界 | 無条件に `tgt` load + compare + store | `if (UNLIKELY(r.state))` のみ (非ローカル return 時以外ゼロ) |
| frame struct | `uint32_t frame_id` 含む | 4B 小さい |
| CTX struct | `frame_id_counter` と `return_target_frame_id` 含む | 8B 小さい |

### テスト

780+ の既存テストに加え、深いネスト block からの非ローカル return、helper method 経由の return、ブロック内 method call の local return などを `test_block_control.rb` に追加。`make test` と `make debug-test` の 1812 runs 全通過。

## Phase 3: iterator+block 融合 ❌ 差し戻し

一度実装 (commit `9923302`) したがアーキテクチャ上の問題で revert。

### やったこと (revert 済)

`Integer#times` / `Array#each` / `Array#map` それぞれに対して専用の「融合ノード」(`node_fixnum_times_block` 等) を新設し、`node_method_call_with_block_slow` から `swap_dispatcher` で runtime rewrite。融合ノード内で `abruby_yield` の context swap と関数呼び出しを完全に skip し、block body を enclosing method の `c->fp` に直接パラメータを書き込んで `EVAL(c, body)` する。

マイクロベンチ `bm_times` / `bm_each` は plain モードで 30% 前後高速化した。

### 差し戻した理由

1. **イテレータ 1 つにつき融合ノード 1 つという構造は scale しない**。`times` / `each` / `map` / `select` / `reject` / `upto` / `downto` / `each_with_index` / `inject` / ... と増やすたびに 100 行前後のノードが必要で、N 個のイテレータに対して N 個の特殊化を手書きし続けるのは維持不能。
2. **フレーム共有が正しくない**。融合ノードは enclosing method のフレームを block body にそのまま使わせる作りで、block 用の frame 構築を省略している。現在は閉じた MVP (Proc 無し、binding 無し) のため動くが、今後 `binding` / Proc へ block が escape する機能を入れた時に破綻する。フレーム構築は **per-iter の最小コスト floor として受け入れるべき**。

### 正しい方向 (将来)

ASTro 本来の **PE (部分評価) + method inlining** で対応する。候補は 2 つ:

1. **cfunc iterator を inlining** — `Integer#times` の C ループを call site に SPECIALIZE 時に inline 展開し、block body の dispatcher も名前で直接呼ぶ C コードを emit
2. **AST iterator を inlining** — 将来 `Integer#times` を abruby-level の AST メソッドで書いて PE に食わせ、block body と一緒に展開する

いずれの路線も **method inlining の基盤が ASTro にまだ無い**ので、このセッションでは着手しない。iterator+block の高速化は method inlining が入った後の作業として `docs/todo.md:159` の項目で継続管理。

**per-iter の frame 構築コストは最適化後も残る**(`binding` 等の正しさに必要)。その前提で、EVAL dispatch の indirect call と abruby_yield の関数呼び出しオーバーヘッドを PE + inlining で削るのが目標。

## Block benchmark の追加 ✅ 採用

commit: `d6140d3 abruby: add block-heavy microbenchmarks`

`bm_times` / `bm_each` / `bm_map` / `bm_inject` の 4 本を追加。将来 method inlining 後の効果計測の土台となる。

注: `--compiled-only` モードは AOT パイプラインがまだ block 系ノードに対応していないため segfault する (先立つ制約)。block benchmark は `abruby+plain` と `abruby+cf` で実行。

## 3 地点ベンチマーク

計測: `ruby benchmark/run.rb -n 5`. 同一マシン (WSL2 on Windows).

- **(1) ブロックコミット前**: `benchmark/report/20260411-eac1ee4.txt`
- **(2) ブロックコミット後**: `benchmark/report/20260412-4a8c954.txt`
- **(3) Phase 1 採用後 (Phase 3 revert 後)**: `benchmark/report/20260412-phase1-final.txt`

### ブロックベンチマーク (Phase 1 単独)

`abruby+plain` (秒), n=5 best-of:

| bench | (2) blocks | (3) Phase 1 | 備考 |
|---|---|---|---|
| `bm_times`  | — | 0.376s | yield per-iter full save/restore で支配される |
| `bm_each`   | — | 0.321s | 同上 |
| `bm_map`    | — | 0.112s | allocation bound |
| `bm_inject` | — | 0.199s | each 呼び出しのオーバーヘッド |

(2) の時点ではブロックベンチは存在しないので比較カラム無し。これらは iterator+block inlining が入った将来のために残してある土台。

### 判断材料となった Phase 3 前後の差分 (参考)

`benchmark/report/20260412-phase1.txt` (Phase 1 のみ) と `benchmark/report/20260412-phase3-final.txt` (Phase 3 込み) のブロックベンチ部分:

| bench | Phase 1 plain | Phase 3 plain (revert 済) | 差 |
|---|---|---|---|
| `bm_times`  | 0.330s | 0.221s | -33% |
| `bm_each`   | 0.317s | 0.216s | -32% |
| `bm_inject` | 0.191s | 0.166s | -13% |
| `bm_map`    | 0.108s | 0.103s | -5% |

Phase 3 の plain モード改善効果 (30% 前後) は方向としては正しく、PE + inlining で同等以上を目指したい。

### 非ブロック経路の regression チェック (Phase 1 単独)

`abruby+plain/ruby` の ratio で比較 ((2) vs Phase 1 採用後)。22 本中、改善/同等が大半で、±noise 範囲を超える regression はなし。`fib` / `method_call` などメソッド呼び出し中心の benchmark で明確な regression が出ていないので「通常 method call/return で regression ゼロ」の目標を満たしている。

## 残課題

- [Phase 3 再挑戦] method inlining が ASTro に入った後、iterator+block を PE + inlining で最適化 (`docs/todo.md:159`)
- `Hash#each` の upfront key 配列 materialization (`rb_funcall(hash, "keys")`) を `rb_hash_foreach` ベースに書き換え — Phase 1 最適化と独立
- AOT compile パスでの block 系ノード対応 — `--compiled-only` モードが block benchmark を動かせるようにする
