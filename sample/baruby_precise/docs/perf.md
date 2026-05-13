# baruby_precise 性能ノート

仕様は [spec.md](spec.md)、実装は [runtime.md](runtime.md)、
未対応・残タスクは [todo.md](todo.md) を参照。

baruby_precise は **precise mark&sweep GC の MVP 試作品** で、
姉妹サンプル `sample/baruby` (conservative libgc) と同じテスト・ベンチ
スクリプトで動かして「precise rooting のオーバーヘッドはどれくらいか」
を測ることを目的にしている。 設計の経緯は
[`docs/gc_design.md`](../../../docs/gc_design.md) を参照。

## 1. 計測環境

| 項目 | 値 |
|---|---|
| CPU | AMD Ryzen 9 5900HX |
| OS | Linux 6.8 (x86_64) |
| Compiler | gcc 13.3.0 |
| GC (precise) | 自前 mark&sweep (`gc.c`、 ~210 行)、 threshold 4 MiB |
| GC (conservative 比較対象) | Boehm libgc 8.2.6 (`sample/baruby` 由来) |
| Build flags | `-O3 -ggdb3 -march=native -fno-plt` |

**比較対象**: `sample/baruby/` (libgc 経由の conservative scanning) を
baseline にする。 ベンチスクリプト (`bench/*.ba.rb`) は両者で共通 — baruby
を copy したのでファイル単位で同一。 binary 名のみ異なる
(`./baruby` vs `./baruby_precise`)。 plain mode = AST インタプリタ
(code_store なし)、 AOT mode = `-c` で SD specialize →
`code_store/all.so` 再読み込み後の再実行 (= CCACHE_DISABLE=1 必要)。

## 2. ベンチ実測 (precise vs conservative)

| Bench | cons. plain | cons. AOT | precise plain | precise AOT | precise vs cons. |
|---|---:|---:|---:|---:|---|
| `list_alloc` (560 MB alloc) | 0.90 s | 0.38 s | 1.01 s | 0.43 s | plain **+12%**, AOT **+13%** |
| `string_concat` (1.2 GB alloc) | 0.86 s | 0.66 s | 1.14 s | 0.98 s | plain **+32%**, AOT **+48%** |
| `binary_trees` | 0.78 s | 0.52 s | 🐛 0.31 s | 🐛 0.20 s | **計算結果が壊れる** ← 要 debug |
| `test.ba.rb` (fib(20), fixnum-only) | — | — | 0.0004 s | — | GC 不発火、 影響なし |

GC count / alloc 量:

| Bench | cons. GC count | precise GC count | precise alloc 総量 |
|---|---:|---:|---:|
| `list_alloc` | 1134 | 133 (1/8.5) | 560 MB |
| `string_concat` | 1691 | 177 (1/9.5) | 745 MB |
| `binary_trees` | 12 | 55 | 235 MB |

**観察**:

- precise の GC 回数は conservative の 1/8〜1/9。 sweep が linked-list
  走査だけなので少回数で大量回収する形。 threshold 4 MiB が比較的大きいことも要因
- **AOT も precise で動く** — SD specialize で `(c, n, fp, sp)` の 4 引数
  dispatcher が正しく通る (`dlopen` 後の dispatcher binding 含む)
- plain mode の overhead +12〜32%、 AOT mode の overhead +13〜48% は **sp[]
  への spill memory write、 callee frame の zero-init、 alloc API 経由の
  間接化** の合計
- string_concat の overhead が最大なのは alloc 密度 (1.2 GB) + 短命 String
  が多いため。 list_alloc は alloc 後に長生きする object が多く差が小さい

## 3. オーバーヘッドの内訳 (推定)

| 要因 | 影響 |
|---|---|
| `sp[i] = ...` spill memory write | `node_call_<N>` の callee frame 初期化、 引数評価結果の置き場で alloc 1 回ごとに store が乗る |
| callee frame の zero-init (`for i < locals_cnt: sp[i] = 0`) | call 1 回あたり locals_cnt 回 store。 関数呼出し頻度に比例 |
| `c->sp = sp_top` 更新 (`baruby_gc_alloc` 内) | alloc 1 回ごとに 1 store。 alloc 頻度に比例 |
| sp の register pressure | 4 引数 dispatcher で fp + sp の 2 本 register を消費。 inlined SD で他の値が spill しやすくなる |
| mark&sweep の sweep 時間 | linked-list 全走査。 オブジェクト数に比例 |

`docs/gc_design.md` §1.3.6 で議論した「spill 1 store/root + alloc 時に
c->sp 更新 1 store」 のコストモデルが、 実測でほぼそのまま観察された形。

## 4. 既知の問題

- **`bench/binary_trees` の計算結果が壊れる** (期待 4194303 → 実際 1)。
  再帰木構造での root 漏れの可能性が高い。 木のノードを return しながら
  さらに alloc する経路で、 in-flight な subtree pointer が sp[] に
  spill されていない。 ASTroGen の自動 spill が無い (= 手書きで書き漏れがある)
  ことの具体例
- **node.def 内の arithmetic node**: 例えば `node_add` は `VALUE l =
  EVAL_ARG(c, lv)` を C local に保持するが、 `r` 評価で String concat や
  Array plus が走ると heap allocate して `l` の指す object が回収される
  恐れがある。 binary_trees 失敗の有力な原因
- **toplevel sp が 64 で hardcode** (`main.c::create_context`)。 大きな
  toplevel フレームを持つプログラムでは scratch 領域不足
- **`heap_bytes` 統計が unsigned underflow**: `baruby_gc_realloc_payload`
  が old/new size 差分を tracking していないため。 表示だけの問題、 GC
  動作には影響なし

## 5. 次の段階で試したいこと

- `binary_trees` 修正: 木構造 alloc 経路の root を node.def で明示 spill する
- toplevel locals_cnt を parser から取って main.c で正しい sp を設定
- arithmetic / comparison node の heap VALUE rooting (`node_add` 等の修正)
- `astrogen.rb` 拡張で `@locals` を機械化 (手書きの error-prone を減らす)
- moving GC backend (semi-space) を同じ interface に乗せて perf 比較
- `string_concat` の +32〜48% overhead を perf record で内訳を確認 (どれが
  bottle neck か = spill / sp 更新 / sweep のどれが効くか)
