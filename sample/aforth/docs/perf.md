# perf.md — aforth 性能改善の記録

本書は **どんな最適化を試したか / その結果** を一覧する。
クロスサンプルの一般則は root の [docs/perf.md](../../../docs/perf.md) を参照。

## ベンチマーク環境

- CPU: x86_64
- OS: Linux 6.8 (Ubuntu 24.04)
- コンパイラ: gcc 13.3 (-O2 / -O3)
- AOT 側: `code_store/Makefile` のデフォルト (`-O3 -fPIC -fno-plt -fno-semantic-interposition -march=native`)
- 計測: `ruby benchmark/run.rb -n 3` (best-of-3, warmup あり)
- VALUE = `int64_t`、stack は 64K cell

## 2026-05-04 — initial baseline

aforth 初版の interp / AOT 比較。aforth HEAD initial commit。

| bench         | interp (s) | aot (s) | speedup |
|---------------|-----------:|--------:|--------:|
| ack           | 1.746      | 0.521   | 3.4×    |
| array_sum     | 1.200      | 0.152   | 7.9×    |
| collatz       | 1.324      | 0.074   | 17.9×   |
| factorial     | 2.506      | 0.656   | 3.8×    |
| fib           | 1.110      | 0.322   | 3.4×    |
| gcd           | 1.797      | 0.277   | 6.5×    |
| nested_loop   | 1.143      | 0.390   | 2.9×    |
| sieve         | 0.902      | 0.081   | 11.1×   |
| tak           | 0.572      | 0.055   | 10.4×   |

**読み方**:
- 内側ループが SD 1 本に収まる bench (collatz / sieve / tak / array_sum) で
  10× 級。gcc が basic block 全体を見渡して unroll / hoist できる。
- 再帰主体の bench (fib / ack / factorial — factorial も実は word call が
  全工程を占める) は 3× 程度。`node_call` が `@noinline` で table-load
  + 間接 dispatch なので、call ごとの最低コストでサチる。

## 取り入れた最適化 (採用済み)

### `@always_inline` を hot control-flow に
`node_seq`, `node_if`, `node_if_only`, `node_begin_*`, `node_do_loop`,
`node_do_plus_loop` を `@always_inline` 指定。これがないと SD 内で各
control-flow ノードが個別の `static inline` 関数になり、register
allocation がループ全体を見れない。`@always_inline` で SD 1 本にひとつ
の C 関数として畳まれ、tak / collatz が大きく速くなる。

`+, -, *, =, <, >, ...` 等の **算術プリミティブ** も `@always_inline`
にしてある。SD の中で見ると 1 命令〜数命令しかないので、関数呼び出し
オーバーヘッドより遥かにコードサイズが小さい。

### `node_call` の `@noinline` + word_id table 方式
詳細は [runtime.md](./runtime.md) を参照。要点:

- 全 word call は `aforth_word_table[word_id]` 経由
- caller の SD には `EVAL_node_call(c, n)` の固定 stub しか乗らない
- 副作用: 再帰 / 相互再帰の cycle break が **不要** (実装が単純)
- 副作用: 非再帰の word でも inline できない (3× 程度の固定ロス)

これは「cycle 検出 + 直接 NODE * 操作」より単純で正しい代わりに、性能の
天井を作っている。`@ref` で `NODE *body` を扱える ASTroGen 拡張を入れ
れば、非再帰 word は inline、再帰 word は cycle break で no_inline、と
ハイブリッド化できる。todo.md 参照。

## 試したが見送った/未着手

### data stack を CTX フィールド ↔ レジスタ常駐
`CTX *c->dsp` を `register VALUE *dsp asm("rbp")` のような特殊 alloc
で固定する案。castro の経験で同等の試みが LTO/SROA を阻害して逆効果
だったので、まず measurement を取るまで保留。

### `c->dsp` を `restrict` 経由で SD に渡す
現状 `CTX *` まるごと渡しているが、SD ローカルでは dsp / rsp / dop
だけ使う。`restrict` 化した薄いラッパーを SD entry に挟めば SCEV が
`dsp += k` を一定に追えて、tight loop で +5-10% 期待。**未測定**。

### `node_lit` を operand-baked にして constant fold
SD が `n->u.node_lit.v` を `int32_t` operand として埋め込むので、現状
でも constant です。さらに上位の演算 (`2 *` のような後続パターン) を
parse-time に折り込んで `node_lit2*` のような二重ノードにする手がある
が、実装コストに見合うかは要検討。

### PGC モード
koruby 並みの `--pg-compile` 二段ビルド (HORG → 採取 → HOPT) は未着手。
現状 `HORG == HOPT == HASH` の単段。aforth はベンチが小さいので gain
は限定的だが、`sieve` のような profile-依存 inner loop には効く可能性。

## 引っかかったところ

### ccache + sandbox
`benchmark/run.rb` の AOT setup で `cc` が ccache 経由になると
`/home/ko1/.cache/ccache/...` が sandbox で read-only になりビルド失敗
する。`run.rb` 側で `CCACHE_DISABLE=1` を `astro_cs_build` に渡して
回避。Makefile から直接 build するときは環境変数で同様にしないと
落ちる。

### `aforth_word_table` の動的拡張
parse 中に realloc が走ると、過去にアロケートした NODE が掴んでいる
word_id は安全 (id は値; ポインタは介在しない) だが、ASTroGen の内部
で `aforth_word_table[i]` を SD 内で直接読む形にする最適化を入れた
ときに realloc 後の base アドレスが invalidate されてヒットしなく
なる罠がある。`-fno-plt` でも `dlopen` 越しにグローバル参照を間接
化するから「baked address」にはならないが、最適化を強めるときの落と
し穴として記録しておく。

### CELLS = 1 で済ましたら array indexing が壊れた
初版で `node_cells` を恒等にしたが、`@ ! +!` がアドレスを byte 単位で
扱う実装だったので `1 CELLS arr +` が `arr+1` (byte) を指して途中の
バイトを読み書きしていた。`CELLS = * sizeof(VALUE)` (= `* 8`) に修正。
test_var.fs で発覚。
