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
| Build flags | `-O3 -ggdb3 -march=native -fno-plt -DASTRO_DEBUG=1` |

**比較対象**: `sample/baruby/` (libgc 経由の conservative scanning) を
baseline にする。 ベンチスクリプト (`bench/*.ba.rb`) は両者で共通 — baruby
を copy したのでファイル単位で同一。 binary 名のみ異なる
(`./baruby` vs `./baruby_precise`)。 plain mode = AST インタプリタ
(code_store なし)。 AOT mode は moving GC 移行後に未再検証。

## 2. ベンチ実測 (precise vs conservative, plain, 3 run 中央値)

| Bench | conservative | precise | precise vs cons. |
|---|---:|---:|---|
| `binary_trees` | 0.907 s | **0.544 s** | **0.60×** ⬇40% (precise が速い) |
| `list_alloc` (560 MB alloc) | 1.085 s | 1.152 s | 1.06× ⬆6% |
| `string_concat` (745 MB alloc) | 0.968 s | 1.160 s | 1.20× ⬆20% |
| `fib_pair` | 1.127 s | 1.271 s | 1.13× ⬆13% |
| `substr_churn` | 1.361 s | 1.594 s | 1.17× ⬆17% |
| `gc_combined` | 1.079 s | 1.231 s | 1.14× ⬆14% |
| `test.ba.rb` (fib(20), fixnum-only) | — | 0.0004 s | GC 不発火、 影響なし |

**観察**:

- **binary_trees は precise の方が 40% 速い** — libgc の conservative scan
  が小オブジェクト大量生成シナリオで重い (stack / data segment 全走査)。
  Cheney の bump alloc + Active swap だけのモデルが勝つ
- string_concat / list_alloc / fib_pair / substr_churn / gc_combined は
  +6〜20% で precise が遅い。 これは:
  - sp[] への spill memory write
  - callee frame の zero-init
  - alloc API 経由による間接化
  - sp の register pressure
  - copy collector のコピーコスト (sweep より重い)
  の合計

`docs/gc_design.md` §1.3.6 で議論した「spill 1 store/root + alloc 時に
c->sp 更新 1 store」 のコストモデルが、 ほぼ実測で観察された形。
全体 geomean ~ +7% (binary_trees の大勝で打ち消されて control)。

## 3. Stress mode

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

## 4. 既知の問題

- **toplevel sp が 64 で hardcode** (`main.c::create_context`)。 大きな
  toplevel フレームを持つプログラムでは scratch 領域不足
- **REGION_BYTES = 512 MiB が固定**。 live set がこれを超えると OOM
- **AOT mode は moving GC 移行後に未検証** — SD bake された経路で
  precise rooting が成立しているかは要再 audit (`-c` 動作含む)

## 5. 次の段階で試したいこと

- AOT mode の再検証 (`make CCACHE_DISABLE=1` で `-c` 経路を回す)
- toplevel locals_cnt を parser から取って main.c で正しい sp を設定
- `astrogen.rb` 拡張で `@locals` を機械化 (手書きの error-prone を減らす)
- string_concat の残存 +20% overhead を perf record で内訳分析
- region size adaptive 化 (live set に応じて grow)
- 世代別 GC backend (`gc_combined` ベンチで効くはず) を同 interface に乗せる
