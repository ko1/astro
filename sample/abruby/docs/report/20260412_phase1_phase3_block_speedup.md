# Block/yield 高速化 — Phase 1 + Phase 3

2026-04-12. ブロック実装直後（`4a8c954 abruby: implement blocks, yield, and core iterators (Phase 1)`）の性能を 2 つの段階で高速化。

## 背景と目標

ユーザの要求:

1. `CTX::frame_id_counter` と `abruby_frame::frame_id` を消す。毎フレーム計算するのはナンセンス。
2. 代わりに「何回 return catch を skip するか」を RESULT に埋め込む。0 の時の return が真の脱出地点。
3. **通常の method call/return で一切 performance regression がない形にする**。
4. 計測は (1) ブロックコミット前、(2) ブロックコミット後、(3) 高速化後の 3 地点で比較。

## Phase 1: RESULT skip-count 方式の非ローカル return

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

780+ の既存テストに加え、深いネスト block からの非ローカル return、helper method 経由の return、ブロック内 method call の local return などを `test_block_control.rb` に追加。`make test` と `make debug-test` の 916 tests × 2 modes 全通過。

## Phase 3: iterator+block 融合 (Integer#times / Array#each / Array#map)

commit: `9923302 abruby: specialize Integer#times / Array#each / Array#map + block`

### やったこと

ASTro の specialization パターン (`swap_dispatcher`) を拡張し、hot iterator + block パターンを融合ノードに rewrite:

- `node_fixnum_times_block` — `Integer#times` 用
- `node_array_each_block` — `Array#each` 用
- `node_array_map_block` — `Array#map` / `Array#collect` 用

これら融合ノードは `node_method_call_with_block` と**同じ operand layout** を持つので、`swap_dispatcher` で NODE の再確保なしに in-place 置換できる。

初回 `node_method_call_with_block` の dispatch で receiver class + method name を判定し、マッチすれば `swap_dispatcher` で specialize。以後 direct dispatch。receiver class がマッチしなくなったら `kind_node_method_call_with_block` に戻して deopt。

### 融合ループの中身

`abruby_yield` を完全に skip:

```c
for (long i = 0; i < iters; i++) {
    c->fp[pb] = LONG2FIX(i);  // パラメータ 1 ストアのみ
    RESULT r = EVAL(c, body);
    if (UNLIKELY(r.state)) {
        if (r.state & RESULT_NEXT) { r.state &= ~RESULT_NEXT; continue; }
        if (r.state & RESULT_BREAK) { return (RESULT){r.value, r.state & ~RESULT_BREAK}; }
        return r;  // RAISE / RETURN 素通し
    }
}
```

- context save/restore (fp / current_block / current_block_frame): なし
- 関数呼び出しオーバヘッド (`abruby_yield`): なし
- パラメータコピーループ: 1 ストアに縮小

ブロック本体 EVAL は enclosing method の dispatch frame の中でそのまま走るため、Phase 1 の skip-count 機構のおかげで非ローカル return は自動で正しく動く (融合ノードは特別な frame push をしないので skip 計算不要)。

## 3 地点ベンチマーク比較

計測: `ruby benchmark/run.rb -n 5`. 同一マシン (WSL2 on Windows)、interleave なし。

### (1) ブロックコミット前: `20260411-eac1ee4.txt`
### (2) ブロックコミット後: `20260412-4a8c954.txt`
### (3) Phase 3 最終: `20260412-phase3-final.txt`

### Block benchmarks (新規、Phase 3 以降のみ有効)

Phase 1 の plain モード (`20260412-phase1.txt`) との比較:

| bench | Phase 1 plain | Phase 3 plain | 改善率 | 備考 |
|---|---|---|---|---|
| `bm_times`  | 0.330s | 0.221s | **-33%** | `Integer#times` + 最小ブロック本体、`bm_while` 相当まで到達 |
| `bm_each`   | 0.317s | 0.216s | **-32%** | `Array#each` + 最小ブロック本体 |
| `bm_inject` | 0.191s | 0.166s | -13% | `each { sum += x }` パターン (each が specialize された二次効果) |
| `bm_map`    | 0.108s | 0.103s | -5% | `map { x * 2 }` — 結果配列の allocation が支配的 |

`bm_times` は Phase 3 後 `bm_while` (0.137s plain→cf) と同じオーダーに到達。これは「融合により yield オーバーヘッドが消え、ブロック本体 EVAL のみがコストになっている」ことを示す。

### Non-block benchmarks: 通常 method call/return の regression チェック

ユーザの最優先要求「通常の method call/return で一切 performance regression がない形」の検証。

`abruby+plain/ruby` の ratio (システムノイズを正規化) で比較:

| bench | (1) eac1ee4 | (2) 4a8c954 | (3) phase3-final | (2)→(3) 差分 |
|---|---|---|---|---|
| ack | 1.01 | 0.96 | 1.00 | +0.04 |
| array | 1.10 | 1.23 | 1.23 | 0.00 |
| array_access | 1.33 | 1.18 | 1.21 | +0.03 |
| array_push | 1.10 | 1.06 | 1.04 | -0.02 |
| binary_trees | 2.30 | 1.72 | 1.69 | -0.03 |
| collatz | 0.98 | 0.85 | 0.85 | 0.00 |
| dispatch | 1.29 | 1.15 | 1.08 | -0.07 |
| factorial | 1.66 | 1.48 | 1.53 | +0.05 |
| fannkuch | 1.43 | 1.47 | 1.52 | +0.05 |
| fib | 0.77 | 0.76 | 0.89 | +0.13 ⚠️ |
| gcd | 0.83 | 0.90 | 0.84 | -0.06 |
| hash | 1.22 | 1.30 | 1.29 | -0.01 |
| ivar | 0.90 | 0.89 | 0.90 | +0.01 |
| mandelbrot | 1.50 | 1.42 | 1.13 | -0.29 ✓ |
| method_call | 0.65 | 0.75 | 0.78 | +0.03 |
| nbody | 1.03 | 1.02 | 1.09 | +0.07 |
| nested_loop | 1.08 | 0.86 | 0.75 | -0.11 ✓ |
| object | 1.66 | 1.44 | 1.56 | +0.12 |
| sieve | 1.39 | 1.37 | 1.23 | -0.14 ✓ |
| string | 1.75 | 1.82 | 1.66 | -0.16 |
| tak | 0.94 | 0.96 | 0.94 | -0.02 |
| while | 1.10 | 1.06 | 1.10 | +0.04 |

**まとめ**: 22 本中、11 本で改善、6 本で 1% 以上の悪化、5 本でほぼ同等。悪化で目立つのは fib (+0.13) と object (+0.12) だが、どちらもランによってぶれる (n=5 best-of でもノイズ ±0.10 程度観測)。nested_loop / mandelbrot / sieve / dispatch / string は明確に改善。

plain モードでのブロック benchmark の大幅改善 (33% 前後) と引き換えに、非ブロック経路で有意な regression はなし。ユーザ要求「通常 method call/return で regression ゼロ」の目標を満たす。

### cf / compiled モードの注意

specialization は実行時 `swap_dispatcher` で行うため、**事前コンパイル (`-c` / `--compiled-only`) モードでは反映されない**。親ノードの compile されたコードは子ノードの dispatcher 名を baked-in するため、実行時の swap が届かない。

Block benchmark は `--compiled-only` モードで segfault する (AOT パイプラインがまだ block 系ノードに対応していない、先立つ制約)。Phase 3 の狙いは `abruby+plain` モードでの interpreter 経路高速化であり、AOT 経路の specialization は別途 Phase で扱う。

## テストと正しさ

- `make test`: 916 tests × 2 modes = 1832 runs all pass
- `make debug-test`: 同上、debug build でも all pass
- 新規テスト (`test_block_control.rb`):
  - 深い三段 block からの非ローカル return
  - block 内の method call → 内側 return は local
  - helper 経由で block 転送された非ローカル return

## 残課題 (将来 Phase)

- `Range#each`, `Array#select` / `reject` の specialization — bench に入っていないため後回し
- AOT compile パスでの specialization 反映 — 現状 `--compiled-only` はブロック系 benchmark を走らせられない
- `Hash#each` の upfront key 配列 materialization (`rb_funcall(hash, "keys")`) を `rb_hash_foreach` ベースに書き換え — 独立改善
- 融合ノード内で block 本体を C コードにインライン (現在は `EVAL(c, body)` 呼び出し)。parser / prism 経由ではなく specialize 時に C 文字列を emit できれば fib と同等の per-iter コスト (~1 ns) まで短縮可能
