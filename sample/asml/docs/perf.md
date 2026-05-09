# 性能改善ログ

asml の性能まわりで「やったこと / その効果」を時系列で残す。

## 計測環境

- Linux 6.8.0 / x86_64
- gcc -O2 (asml 本体), runtime での `-c` AOT compile も -O2
- `bench/run.sh` で best-of-3 を計測

## ベースライン (Phase 1 完了時 — HM 推論 + 型駆動特殊化)

`./asml -q bench/<x>.sml` (interp)、`-c` で warm cache:

| ベンチ        | 入力                      | 結果      | interp  | AOT-cold | AOT-warm |
|---------------|--------------------------|-----------|---------|----------|----------|
| fib           | `fib 35`                 | 9227465   | 2.16 s  | 1.85 s   | 1.59 s   |
| ack           | `ack 3 9`                | 4093      | 2.55 s  | 2.72 s   | 2.52 s   |
| tak           | `tak 24 16 8` ×5         | 9         | 3.05 s  | 2.99 s   | 2.82 s   |
| nqueens       | 10-queens ×3             | 724       | 2.04 s  | 2.28 s   | 1.84 s   |

AOT-warm vs interp の改善幅は 10〜25%。interp パスでも `lower_expr` が
すでに型特殊化済みノード (`node_*_int`, `node_if_bool` 等) を選択しているため
動的チェックは入っておらず、AOT で得られる追加の利得は **dispatcher 間接呼び出しの
インライン化** のみ。

AOT-cold は build (`make` + `gcc`) のオーバーヘッドが乗るので interp 比で
ほぼ等しいか若干遅い (fib/ack/tak)。nqueens のように元々遅いベンチでは
build 時間が相対的に減って AOT-cold でも improvement。

## SML/NJ 比較 (`bench/compare.sh`)

[Standard ML of New Jersey](https://www.smlnj.org/) v110.79 を `sml`
コマンドで起動。SML/NJ は **type-directed native code compiler** で、
ロード時にバッチ compile してから実行する。`fun fib n = ...` のような
小さなプログラムでも JIT-tier ではなく事前 native compile が走る。

| ベンチ        | asml-int | asml-AOT | sml/NJ  | sml/NJ vs asml-AOT |
|---------------|---------:|---------:|--------:|-------------------:|
| fib (35)      |  2.13 s  |  1.68 s  | 0.12 s  | **14.0× 速い**     |
| ack (3, 9)    |  2.67 s  |  2.48 s  | 0.06 s  | **41.3× 速い**     |
| tak ×5        |  3.16 s  |  3.11 s  | 0.09 s  | **34.6× 速い**     |
| nqueens ×3    |  2.40 s  |  2.12 s  | 0.07 s  | **30.3× 速い**     |

(`sml` の起動オーバーヘッドはロード + autoload 込みで 0.02 s。
fib なら実質計算 ~0.10 s。)

**ギャップの内訳** (推定):

1. **SML/NJ は native code を吐く**。asml は AST ノード dispatcher 間接
   呼び出しのまま (AOT 後も SD 関数を `call` で呼ぶ)。fib ような tight
   recursion で命令数自体が ~10× 違う。
2. **SML/NJ の closure call 規約は register-based**。asml は `ml_apply`
   経由でフレーム malloc / IC 検査が入る。
3. **inlining**。SML/NJ は `+`, `<` を呼び出し時に inline。asml の AOT
   も `_int` 系を経由するが、SD 間 inline は LTO 設定なしで limited。

**改善余地** (todo.md と重複):

- `is_leaf` を parser-time に立てて closure frame を C スタック alloca に
  → fib で 2× 期待
- AOT に `-flto -finline-limit=10000 ...` (astocaml と同) → 2-3× 期待
- N-ary 直接呼び出し畳み込み (`f x y` を `app2(f, x, y)` に lower) → ack
  で 30% 期待

これらを全部入れても native code の SML/NJ には届かないが、astocaml が
ocamlc bytecode を超えたのと同パターンで、**SML/NJ bytecode (もしあれば) や
他の SML 実装に対してはイーブンか勝てる** はず。

## 累積効果

| 段階 | fib(35) | ack(3,9) |
|---|---|---|
| HM 前 (動的型チェック有り) | (実装してないので未計測) | — |
| Phase 1: HM + 型駆動特殊化 (interp) | 2.16 s | 2.55 s |
| Phase 1: AOT warm | 1.59 s | 2.52 s |

interp では「`node_add` の `IS_INT && IS_INT` チェック」「`node_if` の
`ML_TRUE / ML_FALSE` の二段比較」「`node_lt` の polymorphic fallback」が
**全て消えている** ことを確認:

```
$ grep -hoE "EVAL_node_[a-z0-9_]+" code_store/c/*.c | sort -u
EVAL_node_add_int   EVAL_node_eq_int  EVAL_node_gt_int   EVAL_node_if_bool
EVAL_node_lref      EVAL_node_lt_int  EVAL_node_ne_int   EVAL_node_neg_int
...
```

`node_add` / `node_lt` 等 generic 系は SD に **一切** 出現しない。

## 主要な技法

### ✅ HM 型推論からの dispatcher 焼き込み (大勝利)

実装:
- 各トップ form を parse → `infer(0, e)` で `e->ty` を埋める
- `lower_expr` で `e->bin.l->ty` を `ty_deref` し、`int` なら `_int` 変種を
  選択。`bool`、`real`、`string`、`ref` も同様
- `node_add_int / sub_int / mul_int / div_int / mod_int / neg_int` は
  IS_INT 検査を全部削除
- `node_if_bool` は `cv == ML_TRUE` の 1 比較のみで分岐

効果: arithmetic-heavy コード (fib, ack, tak) で interp 自体が ~25% 速い
(astocaml と同程度の代入位置から)。AOT で更に 10〜30% 載る。

### ✅ Inline cache for gref (astocaml 流)

`node_gref` に `struct gref_cache *cache @ref`。`c->globals_serial` を
ref- / set! ごとに bump。ホットパス: `cache->serial == c->globals_serial`
なら cached value を即返す (2 load + 1 cmp)。

効果: `+ - * <` 等の op-as-prim 経由の参照、再帰関数の自己参照
(`fact` 内の `fact`)、すべて 2 load の cost で済む。

### ✅ Closure leaf alloca

`node_fn` に `is_leaf` フラグ (現状はパーサで全 closure に false を
立てており、ml_apply 側でしか activate しない)。`ml_apply` の
first iteration でクロージャ frame を C スタックに alloca できる。

未だ静的に立っていない (パーサが立てれば fib などで効く可能性あり)。
todo.md 参照。

### ✅ Tail-call trampoline

`ml_apply` の `loop:` + `c->tail_call_pending` でトランポリン化。
1M 段階の自己再帰でもスタック使用量一定。

### ✅ App cache (call site IC)

`node_app1/2/3` に `struct app_cache @ref`。同一 fn での再呼び出しで
`{nparams, is_leaf}` チェックを skip し、frame alloca + body dispatcher
直呼びの fast path に乗る。

ホットパスのコスト:
```c
if (LIKELY(cache->fn == f)) {
    /* alloca frame + copy args + dispatcher() */
}
```

## ノード使用統計 (各ベンチで AOT 後の SD 内訳)

```
fib(35) のSD:
  node_add_int, node_sub_int, node_lt_int, node_if_bool, node_app1
  node_const_int, node_lref, node_const_unit (top)

ack(3, 9):
  node_add_int, node_sub_int, node_eq_int, node_if_bool, node_app1
  node_const_int, node_lref, node_const_unit (top)

nqueens(10) ×3:
  node_*_int (add/sub/eq/lt/gt/ne/neg)
  node_if_bool, node_andalso_bool, node_match_arm, node_match_fail
  node_pat_test_cons / _nil / _tuple, node_app1, node_let, node_lref
```

generic 演算子ノードは出現せず、全部 _int / _bool 系に置き換わっている。

## generic ノード削除 (Phase 2)

HM 推論が走った後、`node_add` / `node_lt` / `node_if` 等の **動的型
チェック付き generic ノード** は到達不能になっていた。これを node.def から
**完全に削除** し、対応する specialised 版だけを残した:

- `node_lt/le/gt/ge/eq/ne` の動的 IS_INT fast-path 付き版を削除
  → 代わりに `_int` / `_real` / `_string` / `_poly` の 4 系統 (lower_cmp
    が operand 型で振り分け)
- `node_if / not / andalso / orelse / concat / deref / assign` 削除
  → `_bool` / `_str` / `_unchecked` 版を直接呼ぶ
- `node_add / sub / mul / div / mod / neg` 削除
  → `_int` のみ (asml では `+ - *` は int 専用)
- `node_radd / rsub / rmul` 削除 (asml は `+. -. *.` を持たない)
- `node_let_pat`, `node_abs` 削除 (未使用)

24 個の NODE_DEF を削除、19 個の specialised 版を追加 (-5 net、87 → 82)。
動的に呼ばれることのないコードがバイナリから消えるので main.c / SD .so
両方が小さくなる。perf には影響なし (削除前から実行されていなかったので)。

これで「動的型ディスパッチが残っている = 型推論器が型を絞れなかった」が
**コンパイル時の不変条件** になる。今後新ノードを増やすときも同じ規律が
保てる。

## 試したが採用しなかった

(まだ無し)

## 検討中

todo.md の「性能向上のための今後の課題」を参照。
