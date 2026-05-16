# baruby_precise 性能ノート

仕様は [spec.md](spec.md)、実装は [runtime.md](runtime.md)、
未対応・残タスクは [todo.md](todo.md) を参照。

baruby_precise は **precise *moving* GC (semi-space) の testbed** で、
姉妹サンプル `sample/baruby` (conservative libgc) と同じテスト・ベンチ
スクリプトで動かして「precise rooting + 移動 GC のオーバーヘッドは
どれくらいか」を測ることを目的にしている。 設計の経緯は
[`docs/gc_design.md`](../../../docs/gc_design.md) を参照。

## 1. 計測環境

| 項目 | 値 |
|---|---|
| CPU | AMD Ryzen 9 5900HX |
| OS | Linux 6.8 (x86_64) |
| Compiler | gcc 13.3.0 |
| GC (precise) | 自前 semi-space (`gc.c`、 ~310 行)、 region 512 MiB |
| GC (conservative 比較対象) | Boehm libgc 8.2.6 (`sample/baruby` 由来) |
| Build flags | `-O3 -flto=auto -ggdb3 -march=native -fno-plt -DASTRO_DEBUG=1` |
| GC backend  | `make GC=<name>` で選択。 default = `copy` (semispace Cheney) |

**比較対象**: `sample/baruby/` (libgc 経由の conservative scanning) を
baseline にする。 ベンチスクリプト (`bench/*.ba.rb`) は両者で共通 — baruby
を copy したのでファイル単位で同一。 binary 名のみ異なる
(`./baruby` vs `./baruby_precise`)。 plain mode = AST インタプリタ
(code_store なし)。 AOT mode は moving GC 移行後に未再検証。

## 2. 全 GC backend のベンチ実測 (plain mode, 3-run 中央値, 11 bench)

10 種類の backend × 11 ベンチ (`hash_chain` `nqueens` 追加、 各 3-run
中央値)。 単位: 秒。 行ごとの最速に `**` 印。

| Bench         | none | mark | mark\_gen | mark\_gen\_inc | copy | copy\_gen | copy\_gen\_inc | mark\_compact | mark\_compact\_gen | bump |
|---------------|------:|------:|------:|------:|------:|------:|------:|------:|------:|------:|
| binary_trees  | 0.69 | 0.97 | 1.38 | 1.44 | 0.58 | 0.83 | 0.83 | 0.58 | 0.84 | **0.51** |
| cons_list     | 1.37 | 1.14 | 1.31 | 1.36 | 1.12 | 0.89 | **0.86** | 1.13 | 0.88 | 1.04 |
| fib_pair      | 1.73 | 1.53 | 1.65 | 1.73 | 1.34 | 0.94 | **0.88** | 1.49 | 0.94 | 1.34 |
| gc_combined   | 1.52 | 1.25 | 1.44 | 1.51 | 1.26 | 1.05 | **0.99** | 1.25 | 1.02 | 1.24 |
| hash_chain    | 1.54 | 2.44 | 2.27 | 2.29 | **1.22** | 1.25 | **1.21** | 1.34 | 1.22 | 1.50 |
| interp_calc   | 1.36 | 1.53 | 1.54 | 1.55 | 1.27 | 1.07 | **0.98** | 1.36 | 1.08 | 1.25 |
| life          | 1.39 | 1.32 | 1.44 | 1.36 | 1.43 | 1.37 | 1.32 | **1.24** | 1.35 | 1.31 |
| list_alloc    | 1.40 | 1.22 | 1.36 | 1.34 | 1.20 | 0.94 | 0.94 | 1.32 | **0.92** | 1.20 |
| list_sort     | 1.27 | 1.31 | 1.37 | 1.32 | 1.26 | 1.14 | **1.04** | 1.28 | 1.13 | 1.28 |
| nqueens       | 1.04 | 1.04 | 1.07 | **0.98** | 1.05 | 1.04 | 0.96 | 0.96 | 1.00 | 1.07 |
| string_concat | 1.79 | 2.41 | 1.67 | 1.57 | 0.99 | 0.57 | **0.52** | 1.16 | 0.60 | 1.01 |
| substr_churn  | 1.85 | 1.53 | 1.74 | 1.59 | 1.34 | 0.97 | **0.87** | 1.59 | 0.97 | 1.23 |

**勝者分布**: `copy_gen_inc` が 8 bench で最速、 `mark_compact_gen` が 1
(list_alloc) / `copy` が 1 (hash_chain と tied) / `bump` が 1 (binary_trees
で no-GC ベースライン) を取る。 `copy_gen_inc` の優勢は 2026-05-16 (8) の
`baruby_gc_realloc_payload` 修正 (alloc-first / fwd-aware memcpy) で
realloc-heavy パスの malloc/free が消えたのが大きい。 `copy_gen` と
`copy_gen_inc` は ABI 同一だが、 inc 側は SATB flag check (現状は STW
fallback パスのみ) の最適化ヒントで 3-10% リード。

**2026-05-16 (10) 改善**: `mark` の binary_trees が 7.54 s → **0.97 s
(7.8×)** に劇的改善。 原因は major threshold を fixed 4 MiB → 適応的
(`max(MIN, 2 × live_bytes_post_sweep)`) に変更。 binary_trees の 200 MiB
live heap では旧 threshold が 50 回 GC を発火していたが、 適応版は 4 回
で済む (各 sweep が O(heap) なのでこの削減が直接効く)。 同じ修正を
`mark_gen` / `mark_gen_inc` の major threshold (64 MiB → 適応的) にも
適用、 binary_trees で 10-13% 改善。 short-lived workload では heap が
MIN を超えないので動作は不変。

**2026-05-16 (12) 改善**: parser バグ修正により `n = n + foo(a, b, c, d, e)`
形 (binop の RHS に >3-arg call) が壊れず動くようになった
([done.md](done.md) (12) 参照)。 `bench/life.ba.rb` で 8 個の隣接セル
取得を inline 化できるようになり 1.54 s → 1.30 s に縮んだ (workaround
撤去の bonus)。

- **`none`** は GC を全く行わない (= leak)。 sp[] rooting / WB / alloc API
  間接化のオーバーヘッド単体が見える baseline
- **`mark`** は per-object malloc + linked list 走査の sweep。 オブジェクト数
  に比例して binary_trees で爆死 (7.2s)
- **`mark_gen` / `mark_gen_inc`** は nursery / tenured 分離 + 明示
  remembered set (dirty list)。 過去版の lazy dirty scan (O(|old|)) を
  解消して binary_trees / interp_calc が ~30〜50% 改善
- **`copy`** (semispace Cheney) は default。 small heap でも binary_trees
  でも安定して速い
- **`copy_gen`** は string-heavy で大勝 (string_concat 0.57s = libgc の
  0.60×)。 短命 string の churn が nursery で完結。 binary_trees も
  remset 導入で 0.79s に
- **`copy_gen_inc`** は infra のみ用意 (incremental marking の SATB
  barrier + gray queue)。 stack-WB が無いため STW で運用
- **`mark_compact`** は単一 region bump alloc + Lisp-2 sliding compactor。
  per-object malloc を回避しつつ非 moving (compact 時のみ移動)。
  binary_trees で mark の 7.06s → 0.61s (12×) — region 化の威力
- **`mark_compact_gen`** は nursery (copy) + tenured (mark+compact) の hybrid。
  short-lived alloc は nursery で完結、 long-lived のみ tenured へ promote。
  tenured は single region (vs copy_gen の 2×) で in-place compact。
  **8 bench 中 6 つで全 backend 最速** (libgc 含む全体トップ)
- **`bump`** は bump alloc only (no GC, leak)。 baseline floor として、
  「rooting + WB + dispatch + alloc」の最小コストを示す。 binary_trees が
  0.53s — copy より速い (GC 自体が無いので)。 OOM 時 abort

### マクロベンチ

- **`interp_calc`**: 12 段の AST を構築 → 再帰評価 → 合計。 1000 回。
  AST 構築の alloc burst → 評価中 alloc なし、 という generational
  benefit が出やすいパターン
- **`list_sort`**: 2000 要素の整数配列を merge sort。 350 回。 各 merge
  が中規模 alloc を burst → 完了時に全部死ぬ pattern
- **`cons_list`**: 5000 セルの cons-list を build & walk × 2000 回。
  各セル = `[value, next]` (2-要素配列)。 deep alloc chain → walk →
  discard の典型 (1 iter で 5000 セル全部死ぬ)。 binary_trees と違い
  iterative walk なので C stack 浅いまま深い chain を作れる
- **`hash_chain`** (2026-05-16 追加): 2048 buckets の chained hash table
  を Array on Array で実装。 150k keys × 3 rounds で long-lived
  buckets + medium-lived chains + short-lived [k, v] pairs の 3 層
  lifetime を踏む。 chain.items grow が高頻度で、 旧来の
  `baruby_gc_realloc_payload` の stale-ptr バグを発掘した
- **`nqueens`** (2026-05-16 追加): N=11 で 2680 solutions を backtrack
  探索。 deep recursion + per-frame Array alloc (column set を
  functional コピーで pass-down)。 LIFO で短命なので nursery 完結の
  benefit が顕著

### Backend 選択ガイド

ワークロードの性質ごとの推奨:

| パターン | 推奨 backend | 理由 |
|---|---|---|
| 短命 alloc 多 (大半が捨てられる) | `mark_compact_gen` または `copy_gen` | nursery で完結、 tenure cost 最小 |
| 長寿命 heap が大半 (binary_trees 等) | `copy` または `mark_compact` | gen 無しで in-place / semispace の単純さ勝ち |
| string-heavy (concat / slice 多) | `copy_gen_inc` または `mark_compact_gen` | nursery + sp ref pattern の組合せ |
| 仮想空間を節約したい | `mark_compact_gen` | tenured 1× region (vs copy_gen の 2×) |
| GC レイテンシ最小化 | (現状) `bump` (no GC) または gen 系 minor | major のみ stop-the-world |
| 純粋な alloc コスト測定 | `bump` (leak base) または `none` (libc malloc) | rooting + dispatch のみ |

10 backend のうち default は `copy` (semispace Cheney) で、 全 backend
は plain mode と stress mode (`BARUBY_GC_STRESS=1`) の test 3 種を PASS、
bench 11 種が全完走。

### GC 時間計測 (`gc_seconds` / `gc_pct`)

`BARUBY_GC_STATS=1` で各 backend がミューテータ時間と GC 時間を分けて
出す。 8 backends (none / bump を除く) の collect entry を
`baruby_gc_time_begin/end` で挟み、 `CLOCK_MONOTONIC` で累計。
minor→major の re-entrant ケースは depth guard で最外側だけ計測。

例 (binary_trees, plain):

```
backend=mark_gen_inc      gc_count=56 gc_seconds=0.2577 gc_pct=16.9
backend=mark_compact_gen  gc_count=26 gc_seconds=0.4076 gc_pct=49.3
backend=copy_gen          gc_count=24 gc_seconds=0.0X   gc_pct=X.X
```

GC vs mutator の振り分けが定量化できるので、 mark_compact_gen の compact
コストが効いてくる workload や、 mark_gen の sweep が支配的になる
workload を実測ベースで区別できる。

## 3. ベンチ実測 (precise default(copy) vs conservative, plain, 5 run 中央値)

| Bench | conservative | precise | precise vs cons. |
|---|---:|---:|---|
| `binary_trees` | 0.907 s | **0.576 s** | **0.63×** ⬇37% (precise が速い) |
| `list_alloc` (560 MB alloc) | 1.085 s | 1.175 s | 1.08× ⬆8% |
| `string_concat` (745 MB alloc) | 0.968 s | **0.961 s** | **0.99×** (parity) |
| `fib_pair` | 1.127 s | 1.285 s | 1.14× ⬆14% |
| `substr_churn` | 1.361 s | **1.354 s** | **1.00×** (parity) |
| `gc_combined` | 1.079 s | 1.244 s | 1.15× ⬆15% |
| `test.ba.rb` (fib(20), fixnum-only) | — | 0.0004 s | GC 不発火、 影響なし |

geomean ≈ 0.98× (precise の方が 2% 速い)。

**観察**:

- **binary_trees は precise の方が 37% 速い** — libgc の conservative scan
  が小オブジェクト大量生成シナリオで重い (stack / data segment 全走査)。
  Cheney の bump alloc + Active swap だけのモデルが勝つ
- **string_concat / substr_churn は libgc とほぼ同等** — `baruby_str_concat`
  の sp ref pattern 化 (mallocバッファ撤去) と `KIND_PAYLOAD_BYTE` の memset
  スキップが効いた。 詳細 §4
- list_alloc / fib_pair / gc_combined は +8〜15% で precise が遅い。
  これは:
  - sp[] への spill memory write
  - callee frame の zero-init
  - alloc API 経由による間接化
  - sp の register pressure
  - copy collector のコピーコスト
  の合計

`docs/gc_design.md` §1.3.6 で議論した「spill 1 store/root + alloc 時に
c->sp 更新 1 store」 のコストモデルが、 ほぼ実測で観察された形。

## 4. Stress mode

`BARUBY_GC_STRESS=1` で「毎 alloc で GC」 + 「古い from-space を恒久
PROT_NONE + MADV_DONTNEED」 のデバッグモードに切替。 stale pointer を
deref した瞬間 SIGSEGV するので moving GC 特有のバグ (rooting 漏れ、
helper 内 C local の更新漏れ) が即発覚する。 詳細は
[runtime.md §5.3](runtime.md)。

開発中に表面化した代表的バグ:

- `baruby_ary_push(VALUE av, VALUE x, ...)` の `x` が realloc トリガで
  stale → 新 items[len] に旧アドレス書込 → 子 GC で再 free 失敗で破綻
  → `VALUE *x_ref` に変更して post-GC 再 read
- `node_eq` / `_neq` / `_lt` / `_le` / `_gt` / `_ge` / `_mul` /
  `_spaceship` / `_call_aget` / `_call_aget2` の `VALUE l = EVAL_ARG`
  パターンを sp[] spill に直す
- `baruby_str_concat` / `_repeat` / `_append`, `baruby_ary_plus` /
  `_repeat` の helper を VALUE による値引数から `VALUE *ref` に変更
- 特に `baruby_str_concat` は malloc/memcpy/free で source bytes を
  バッファリングしていたのを ref pattern に切り替え、 **1.468 s → 1.160 s
  (-21%)** に短縮

## 5. 効いた最適化 (履歴)

### 4.1 `baruby_str_concat` を ref pattern に
旧版: 内部 alloc 前に source bytes を libc malloc 領域に退避してから
alloc 後にコピー (helper が VALUE 値受けの制約)。
新版: `VALUE *av_ref` / `*bv_ref` で受け、 alloc 後に `*ref` 再 deref
で post-GC アドレスを取り直す。 malloc/memcpy/free を 2 回ずつ削減。
→ string_concat 1.468 s → 1.160 s (-21%)。

### 4.2 `baruby_str_new` の malloc バッファ撤去
旧版: source bytes が heap interior pointer の場合に備えて毎回 malloc
バッファコピー。 実際の呼び出し元の大半は rodata 文字列 (literal) で、
無駄。
新版: `baruby_str_new` は source が survive することを前提とし、
malloc を撤去。 heap source 用には `baruby_str_slice(*src_ref, off, len)`
を新設して `node_call_aget` / `_aget2` の substring 経路を分離。
→ string_concat の malloc/free が消えて 1.160 s → 0.961 s (-17%)、
   substr_churn は 1.594 s → 1.354 s (-15%)。

### 4.3 `KIND_PAYLOAD_BYTE` の memset スキップ
String の bytes ペイロードは GC が pointer として読まないので、 alloc
直後の memset は不要。 `baruby_gc_alloc_byte` を新設して分岐。 caller
(`baruby_str_*` 系) は即座に bytes を書き込む。

### 4.4 LTO (`-flto=auto`) 有効化
`baruby_gc_alloc` を含む小関数がコールサイトに inline され、 size 引数
が定数畳み込みされる。 fib_pair 等の小型 alloc が多いベンチで効く。

## 6. 既知の問題

- **toplevel sp が 64 で hardcode** (`main.c::create_context`)。 大きな
  toplevel フレームを持つプログラムでは scratch 領域不足
- **REGION_BYTES = 512 MiB が固定**。 live set がこれを超えると OOM
- **AOT mode は moving GC 移行後に未検証** — SD bake された経路で
  precise rooting が成立しているかは要再 audit (`-c` 動作含む)

## 7. 次の段階で試したいこと

- AOT mode の再検証 (`make CCACHE_DISABLE=1` で `-c` 経路を回す)
- toplevel locals_cnt を parser から取って main.c で正しい sp を設定
- `astrogen.rb` 拡張で `@locals` を機械化 (手書きの error-prone を減らす)
- list_alloc / fib_pair / gc_combined に残る +8〜15% overhead を perf
  record で内訳分析 (sp[] spill / callee frame zero-init / copy cost
  のどれが効くか)
- region size adaptive 化 (live set に応じて grow)
- 世代別 GC backend (`gc_combined` ベンチで効くはず) を同 interface に乗せる
