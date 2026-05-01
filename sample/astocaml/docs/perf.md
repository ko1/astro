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

## 現状 (gref IC + TCO + 比較ファストパス + method send IC + closure leaf alloca)

`make bench`:

| ベンチ | 入力 | 結果 | 時間 |
|--|--|--|--|
| ack    | ack(3, 9)         | 4093    | 0.62 s |
| fib    | fib(35)           | 9227465 | 0.57 s |
| nqueens| 10-queens × 3     | 724     | 1.12 s |
| sieve  | sum prime ≤8000 ×8 | 3738566 | 0.97 s |
| tak    | tak(24,16,8) ×5   | 9       | 0.46 s |
| method_call | counter#incr 10M | 10000000 | 0.99 s |

ベースラインからの累積効果:
- fib: 2.62 s → 0.57 s (**4.6× 高速化**)
- ack: 1.20 s → 0.62 s (1.9×)
- tak: 1.23 s → 0.46 s (2.7×)
- nqueens: 1.59 s → 1.12 s (1.4×)
- sieve: 1.21 s → 0.97 s (1.2×)

**直近の二大勝利**:
1. **gref インラインキャッシュ** — 全ベンチ 3-5× 高速化
2. **closure leaf alloca** — fib などの再帰で frame の malloc を完全排除し追加で 2-2.5× 高速化

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

### ✅ Method send IC (`obj#m`)

実装:
- `node_send` に `struct send_cache *cache @ref` を追加 (gref と同パターン)。
- ホットパス: `cache->names_ptr == oo->obj.method_names` ならキャッシュ済み closure を即取得、`strcmp` ループを完全スキップ。
- 各オブジェクトインスタンスは独自の `method_names` 配列を持つので、IC は **同一インスタンスを繰り返し呼び出すループ** で効果が大きい。
- 冷ホットパス: `oc_object_lookup_method` で線形探索 → cache 更新。
- フィールド読み出し / `set_X` のフォールバックは従来の `oc_object_send` に丸投げ。

効果: method_call ベンチで 2.1 s → 1.66 s (**1.34× 高速化**)。
注: より大きな勝利はクラス間で method table を共有する版だが、現状では各 `new` がフレッシュな配列を確保するため per-instance に留まる。

### ✅ Closure leaf alloca

実装:
- パース時に各 `node_fun` 本体を walk して、内部に `(node_fun ` または `(node_lazy ` が出現しなければ "leaf" とマーク (dump-grep 方式: 冗長だがミスは安全側に倒れる)。
- `oc_make_closure_ex` で leaf フラグを closure 値に保存。
- `oc_apply` の最初の iteration が leaf closure を呼ぶときは、frame を `alloca` で確保 (oc_apply の C スタック上に乗る)。leaf なので body は新しい closure を作らず、frame は escape しない。
- TCO トランポリン経由 (`goto loop`) の二回目以降の iteration は `first_iter = false` で `malloc` フォールバック (同じ C frame に alloca を積み続けるとスタック爆発)。

効果: fib(35) が 1.65 s → 0.57 s (**2.7× 高速化**)。tak / method_call も 1.5× 程度。
ベンチ全般で frame 確保の `malloc` が完全に消えた点が大きい。

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

- **環境チェーンキャッシュ** (深い lref のため)
- **クラス間で method table 共有** — `__class_build` を改造して同じシグネチャの methods/fields 配列を共有 (method send IC が cross-instance で効く)
- **AOT specialize の本格活用** — 現状の `--compile` パスは loose; PGC まで入れたい
- **Boehm GC の統合** (frame 以外の malloc リーク対策)
- **Tagged float** (Ruby 流の inline flonum encoding) — boxed double の alloc を削減 (※ 値表現の変更は要相談)
- **`OPTIMIZE` の本格化** (hot 検出 → JIT)
