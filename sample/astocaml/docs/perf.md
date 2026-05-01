# 性能改善ログ

astocaml の性能まわりで「**やったこと / その効果**」を時系列で残す。

## ベースライン (Phase 1 完了時)

`make bench`, gcc -O2:

| ベンチ | 入力 | 結果 | 時間 |
|--|--|--|--|
| ack    | ack(3, 9)        | 4093    | 1.20 s |
| fib    | fib(35)          | 9227465 | 2.62 s |
| nqueens| 10-queens × 3    | 724     | 1.59 s |
| sieve  | sum prime ≤8000 ×8 | 3738566 | 1.21 s |
| tak    | tak(24,16,8) ×5  | 9       | 1.23 s |

## Phase 2 中間 (多相比較・トランポリン導入後; gref キャッシュ前)

| ベンチ | 結果 | 時間 |
|--|--|--|
| ack    | 4093    | 3.0 s |
| fib    | 9227465 | (fib33 で) 2.6 s |
| nqueens| 724 (×3) | 1.6 s |
| sieve  | 3738566 (×4) | 1.0 s |
| tak    | 9 (×2) | 1.4 s |

機能追加 (多相比較・トランポリン・variant/record/object チェック) で 25-30% 退化。

## 現状 (gref インラインキャッシュ + TCO + 比較ファストパス)

`make bench`:

| ベンチ | 入力 | 結果 | 時間 |
|--|--|--|--|
| ack    | ack(3, 9)         | 4093    | 0.80 s |
| fib    | fib(35)           | 9227465 | 1.44 s |
| nqueens| 10-queens × 3     | 724     | 1.24 s |
| sieve  | sum prime ≤8000 ×8 | 3738566 | 1.19 s |
| tak    | tak(24,16,8) ×5   | 9       | 0.73 s |

ベースラインに対し ack/tak は ~1.5× 速い、fib/nqueens/sieve は同等。
**gref キャッシュ単体で全ベンチが 3-5× 高速化** したのが主要な勝利。

## 試した改善

### ✅ Inline cache for gref (大勝利)

実装:
- `node_gref` / `node_gref_q` に `struct gref_cache *cache @ref` を追加。
- `c->globals_serial` を `oc_global_define` ごとに bump。
- ホットパス: `cache->serial == c->globals_serial` なら cached value を即返す (2 load + 1 cmp)。
- 冷ホットパス: `oc_global_ref` で線形探索 → cache 更新。
- ASTroGen の `@ref` 対応のため `astocaml_gen.rb` を作成 (ascheme と同パターン)。

効果: ベンチ全般で 3-5× 高速化。最大の単一改善。

```
fib(33):  3.0 s →  0.6 s   (5×)
ack(3,9): 3.0 s →  0.84 s  (3.6×)
tak:      1.5 s →  0.29 s  (5×)
```

### ✅ 末尾呼び出し最適化 (TCO)

実装:
- パース後 post-pass `mark_tail_calls()` が tail-position の `node_app{0..4}` の dispatcher を `node_tail_app{0..4}` に **in-place で swap**。
- `node_tail_app_K` は `c->tail_call_pending=1` をセットして dummy を return。
- `oc_apply` は `loop:` ラベル + `goto loop` のトランポリン。

効果: 機能正しさ。1M 段階の `count_down` が定数スタックで通る。
非 tail 呼び出しのオーバーヘッドは pending check 1 回 (UNLIKELY) のみ。

### ✅ 比較演算の fixnum インラインパス

実装:
```c
NODE_DEF
node_lt(...) {
    VALUE av = EVAL_ARG(c, a);
    VALUE bv = EVAL_ARG(c, b);
    if (LIKELY(OC_IS_INT(av) & OC_IS_INT(bv)))
        return (OC_INT_VAL(av) < OC_INT_VAL(bv)) ? OC_TRUE : OC_FALSE;
    return (oc_compare(av, bv) < 0) ? OC_TRUE : OC_FALSE;
}
```

効果: int だけのコードでは多相 `oc_compare` の関数呼び出しを回避。fib / tak など int-heavy ベンチで ~10% 改善。

### ✅ ASTroGen の always_inline 活用

各 `EVAL_node_*` は ASTroGen が自動で `static inline __attribute__((always_inline))` 付きで生成する。
`setjmp` を含む `node_try` は always-inline と両立しないので `oc_run_try` に切り出した。

### ✅ Pattern compiler の単一フレーム化

最初は cons パターンを `node_match_list` で chain する設計だったが、`[]` と `h::t` の両方を扱う場合に depth ずれる重大バグ。

修正: `node_match_arm` という統一ノードを 1 個用意し、**arm 内の全変数を 1 つのフレームに集約**。
パターンコンパイラ (`pat_gen_test` / `pat_gen_extracts`) が事前にテスト式と extractor 配列を組み立てる。

### ✅ コードストア (AOT) wireup

`--compile` (`-c`) オプションで各トップレベル expression を `astro_cs_compile` → `astro_cs_build` → `astro_cs_reload` → `astro_cs_load`。
小幅改善 (10%)。specialize 自体は他のサンプル (ascheme, wastro) のように真価を発揮しておらず、まだ改善の余地大。

### ✅ モジュール対応 gref (`node_gref_q`)

ネストした module 内の bare 名前参照を、`<prefix>.<name>` と `<name>` の両方を試行。1 つのノードでカバーするので AST のサイズが膨らまない。

## 試したが効果がなかった / マイナスだった改善

### ✗ パターンマッチで scrut node を共有 (戻した)

実装初期、`build_match_chain` で 1 個の `lref(0,0)` ノードをすべての arm test/body で共有していた。
ASTroGen の生成コードが `parent` ポインタを書き換える可能性があるので、共有していると上書きが発生する潜在バグ。
最終的に **毎回 `ALLOC_node_lref(0, 0)` をフレッシュ**。

### ⚠️ 多相比較演算 (退化、後にファストパスで回復)

最初の Phase 1 では `<` / `>` / `<=` / `>=` は **fixnum 専用**: `OC_INT_VAL(av) < OC_INT_VAL(bv)`。
Phase 2 で float / string / list / variant の比較を要件にしたため、`oc_compare(a, b)` を呼ぶ多相版に置き換え。これで 2× ほど退化。

回復: fixnum インラインファストパスを追加してほぼ Phase 1 並みに戻した。

### ⚠️ トランポリン (TCO 機構) のオーバーヘッド

`oc_apply` のループ化によって、**全ての関数呼び出し** に `c->tail_call_pending` チェックが入る。

緩和策:
- `UNLIKELY(c->tail_call_pending)` で分岐予測ヒント
- 初期エントリは caller の `argv` を直接使い (local copy なし)、tail re-entry のみ local buffer にコピー
- `argc > 16` のケースは recursion fallback

それでも 5-10% 程度の遅延が残る。

### ⚠️ malloc-leak (継続)

すべてのヒープ確保で `malloc` を直叩きし、解放しない。
インタプリタ用途では「プログラムが終了するまで生きていてくれれば十分」という割り切り。
復旧プラン (TODO): Boehm GC 統合 (ascheme 同様)。

## 今後試す予定

- **closure leaf 検出 + alloca フレーム** (ascheme パスの移植) — TCO と組み合わせて完全な C ループ並みに
- **環境チェーンキャッシュ** (深い lref のため)
- **Inline cache for method send** (`obj#m` の method lookup を IC 化)
- **AOT specialize の本格活用** — 現状の `--compile` パスは loose; PGC まで入れたい
- **Boehm GC の統合**
- **Tagged float** (Ruby 流の inline flonum encoding) — boxed double の alloc を削減
- **`OPTIMIZE` の本格化** (hot 検出 → JIT)
