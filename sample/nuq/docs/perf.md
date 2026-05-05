# perf.md — nuq 性能ノート

このドキュメントは nuq が他の JSON クエリツール (主に reference の
`jq`) と比べてどれくらいの位置にいるか、どこで負けるかを書く。
実装上の最適化は v0 ではほぼ未着手で、性能改善のバックログは
[`todo.md`](./todo.md) の §B にまとまっている。

## ベースライン

- gcc 13 -O2、SD は `-O3 -fPIC -fno-plt -march=native`
- Boehm GC (allocator のみ、無制限増)
- 比較対象: `jq-1.7` (Debian)

実行モード:
- `nuq interp`: `--no-compile`、SD なし
- `nuq AOT`: `code_store/all.so` 生成済み
- `jq`: 標準 jq

## 期待値

v0 の実装は最も素直な **tree walker** で、pipe stage の境界で出力を
materialize する。jq は何年も磨かれた C 実装 + bytecode VM なので、
v0 では概ね **5–30× 遅い** と見込む。具体的にいうと:

### nuq が比較的近い (1–3×):
- `.foo.bar`、`.foo[0]` のような深さ固定の accessor 連鎖。
  ノード単位の overhead は constant per node。
- 小さい入力 (<1 KB)。割当ノイズが両者で支配的。

### nuq が大きく負ける (10×+):
- **長 stream へのパイプ**: jq は 1 値ずつ stream で処理するが、
  nuq は LHS 全 emit を一度配列に溜めてから RHS を回す。
  `inputs | ...` や巨大 array `.[]` で差が出る。
  → todo B-1。
- **深い `..` recurse**: 同じく allocation 圧で差が出る。
- **`(.a, .b) op (.c, .d)` のような fan-out 多用**: cartesian の
  ために中間配列を 2 個作る。

### nuq が勝つかもしれない (理論上):
- 入力ロードを除いた **filter 単独のホット loop** で、SD
  specialization が pipe を超えて効いた場合。今は B-1 を解決しないと
  成立しない。

## 計測のお作法

### bench スケール

`feedback_bench_sustained` (project memory) に従い、1 ファイル単発
ではなく **~1 秒スケール** で回す。short bench は INIT() / parser /
JSON parse の上り坂で支配される。

### Code Store が効いているか

```sh
# AOT 経路がエラーで落ちて interpreter に fall back している可能性
$ ./nuq -c '.[]' big.json 2>&1 | head -3
astro_cs_build: make failed (exit 512)
...

# CCACHE_DISABLE=1 で回避
$ CCACHE_DISABLE=1 ./nuq -c '.[]' big.json
```

`code_store/all.so` ができているか、`code_store/c/SD_*.c` が
生成されているかで判定。

### 比較フォーマット

`bench/*.sh` 等で:

```
| filter (~1s on jq) | jq | nuq interp | nuq AOT | nuq/jq |
|---|---:|---:|---:|---:|
```

## メモ — 入れたらすぐ効きそうなもの

これは [`todo.md`](./todo.md) と重複するが、計測の **次の一手** だけ
ピックアップ:

1. **emit_buf を per-call alloca ベース に**: 今は `c->emit_buf` が
   heap 配列。sub-eval ごとに新しい array を `GC_malloc`。pipe stage
   の hot loop に立つので、固定サイズ alloca + spill で大半は GC
   不要にできる。
2. **builtin dispatch を hash table に**: `builtin.c::table[]` を
   今は線形走査。`name_id × arity` の closed-addressing hash で 1
   probe にする。
3. **オブジェクト lookup の hash assist**: 文字列キーに事前に
   `hash_cstr` を計算して `key_hashes[]` に持たせ、線形走査で
   fingerprint 比較 → `nuq_eq`。大型オブジェクトでスケールが効く。
4. **streaming pipe** (todo B-1): 一番効くが実装コスト中〜大。
   `[.[]]` から `g` への直接転送だけ inline 化する小スコープ
   切替でも効果は出る見込み。

## 追加すべきベンチ

未着手:

- **filter 軽量ベンチ**: `.foo.bar`、`.[]`、`map(. * 2)` などを
  100k 件入力で計測 → jq との直接比較。
- **filter 重ベンチ**: `group_by` / `unique` / `sort_by` を 1M 件で。
- **構築重ベンチ**: object construction で fan-out あり / なしを比較。
- **JSON parse-only**: フィルタ `.` のみで JSON parser のスループット
  を計測。

具体テーブルはまだ無い。todo G-3 の一部としてベンチハーネスを別
PR で。
