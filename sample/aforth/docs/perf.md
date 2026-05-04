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

## 2026-05-04 — `-flto` 全部入り (post-restrict)

`aforth_aot_compile_all` に `setenv("ASTRO_EXTRA_CFLAGS","-flto",0)` /
`setenv("ASTRO_EXTRA_LDFLAGS","-flto",0)` を仕込んで、`code_store/all.so`
を LTO 付きでビルド。SD 1 本ごとに別 .c → .o → .so というレイアウトな
ので、LTO で **cross-TU の inline + LTO 全体の register allocation** が
効く。前章の selective restrict round からの差分:

| bench       | restrict only | +LTO  | diff   |
|-------------|--------------:|------:|-------:|
| ack         | 0.573         | 0.513 | -10 %  |
| array_sum   | 0.098         | 0.088 | -10 %  |
| collatz     | 0.073         | 0.069 |  -5 %  |
| factorial   | 0.294         | 0.286 |  -3 %  |
| fib         | 0.332         | 0.314 |  -5 %  |
| gcd         | 0.057         | 0.052 |  -9 %  |
| nested_loop | 0.090         | 0.086 |  -4 %  |
| sieve       | 0.084         | 0.077 |  -8 %  |
| tak         | 0.057         | 0.055 |  -4 %  |

全部の bench で 3-10 % win、平均 ~6 %。restrict tuning (節ごとに 1.6×〜
4.7× の wins) と違って "薄く全体に効く" タイプ。両方積むと initial
baseline からの累積:

| bench       | initial aot | restrict + LTO | total speedup |
|-------------|------------:|---------------:|--------------:|
| factorial   | 0.644       | 0.286          | **2.25×**     |
| gcd         | 0.267       | 0.052          | **5.13×**     |
| nested_loop | 0.384       | 0.086          | **4.47×**     |
| array_sum   | 0.154       | 0.088          | **1.75×**     |
| collatz     | 0.071       | 0.069          | 1.03×         |
| sieve       | 0.079       | 0.077          | 1.03×         |
| fib         | 0.322       | 0.314          | 1.03×         |
| ack         | 0.509       | 0.513          | 0.99×         |
| tak         | 0.053       | 0.055          | 0.96×         |

aforth+aot は 9 ベンチ中 8 で gforth を上回る (唯一の同点 ack 0.96×)。
最大: gcd **14.8×** / factorial 7.5× / collatz 7.2× / array_sum 6.2×。

## 2026-05-04 — selective `restrict c` round (post-baseline)

NODE_DEF の `CTX *c` パラメータに **選択的に** `restrict` を入れて再計測。
ルール: control-flow / 比較 / メモリ / I/O は restrict、算術プリミティブと
スタック移動と return-stack 系は restrict なし。下の "なぜ選択的か" 参照。

| bench         | interp (s) | aot (s) | gforth (s) | aot vs interp | aot vs gforth |
|---------------|-----------:|--------:|-----------:|--------------:|--------------:|
| ack           | 1.726      | 0.573   | 0.545      | 3.0×          | 0.95×         |
| array_sum     | 1.391      | 0.098   | 0.526      | 14.2×         | **5.37×**     |
| collatz       | 1.053      | 0.073   | 0.522      | 14.4×         | **7.15×**     |
| factorial     | 2.477      | 0.294   | 2.384      | 8.4×          | **8.11×**     |
| fib           | 0.868      | 0.332   | 0.815      | 2.6×          | **2.45×**     |
| gcd           | 1.882      | 0.057   | 0.790      | 33.0×         | **13.86×**    |
| nested_loop   | 1.167      | 0.090   | 0.367      | 13.0×         | **4.08×**     |
| sieve         | 0.817      | 0.084   | 0.425      | 9.7×          | **5.06×**     |
| tak           | 0.569      | 0.057   | 0.142      | 10.0×         | **2.49×**     |

`ruby benchmark/run.rb -n 5`、gforth は `--return-stack-size=1M`。

initial baseline からの差分:

| bench       | initial aot | selective-restrict aot | speedup |
|-------------|------------:|-----------------------:|--------:|
| factorial   | 0.644       | 0.294                  | **2.19×** |
| gcd         | 0.267       | 0.057                  | **4.68×** |
| nested_loop | 0.384       | 0.090                  | **4.27×** |
| array_sum   | 0.154       | 0.098                  | **1.57×** |
| (others)    | within noise (±5 %) |          |        |

aforth+aot は **9 ベンチ中 8 で gforth を上回る** (前回は 7/9)。
gcd で 13.9× / factorial で 8.1× / collatz で 7.1× / array_sum で 5.4×。
唯一の負け ack (0.95×) は深い RECURSE の indirect-dispatch 床。

### なぜ選択的に `restrict` か

最初は全 `NODE_DEF` の `CTX *c` を `restrict` にしてみたところ、ベンチが
バラバラに反応した:

- 一部 (gcd, array_sum, nested_loop, factorial, sieve) は **大幅に高速化**
- 一部 (factorial の hot inner loop) は **逆に 25% 遅く**

原因を SD のディスアセンブリで追うと、`@always_inline` で SD 1 本に
畳まれた状態では `c->dsp` が常時 register に乗るのが理想だが、`restrict`
が `c` に効くと gcc は `c->dsp` の更新ごとに register からの spill / 再
load を avoid しようと aggressive に最適化する。これは control-flow / 比較
ノードでは正解 — ループ全体で dsp を keep できる。

ところが算術プリミティブ (`+`, `-`, `*`, `1+`, `I` など) では逆効果。
gcc が「dsp は他の何ともエイリアスしない」と知ると、隣接する書き込みを
SSE の 16-byte store にマージしようとする最適化が trigger され、**死んだ
書き込みも含めて vector store** を出してしまう (`vmovdqu xmm, -0x8(rcx)`
が dsp[-1] と dsp[0] をペアで書く — dsp[0] は次の `*` で即上書きされる
死域なのに)。その latency が tight inner loop を直撃する。

採用ルール:

| カテゴリ | restrict | 理由 |
|---------|:-:|------|
| `node_seq`, `node_if*`, `node_begin_*`, `node_do_*` | ✓ | dsp/dop/leave_flag を SD 全体で register-keep したい |
| 比較 (`=`, `<>`, `<`, `>`, `0=`, ...) | ✓ | gcd の WHILE 条件で dsp ロードを hoist できる |
| メモリ (`@`, `!`, `+!`) | ✓ | array_sum / sieve の dsp と vars[] が別アドレスと知らせる |
| I/O (`.`, `EMIT`, ...), `node_dot_quote` | ✓ | hot path ではないが副作用のみ |
| 算術 (`+`, `-`, `*`, `/`, `1+`, `1-`, ...) | × | SSE store-merging で dead store を含めたペアが出る |
| スタック移動 (`DUP`, `SWAP`, `OVER`, ...) | × | nested_loop の hot inner で SSE merge が出るので外す |
| `>R`, `R>`, `R@`, `I`, `J` | × | 同上、small store 操作が SSE 化されると逆効果 |
| `node_lit`, `node_const`, `node_var_ref`, `node_call` | × | leaf ロード、SSE 化のメリットなし |

**読み方**:
- aforth+aot は **9 ベンチ中 7 で gforth に勝利**。collatz は 7× 差。
- 同等 / 負けている 2 つ (ack 0.97× / nested_loop 0.85×) は call または
  loop dispatch の indirect-dispatch 床に張り付いた bench。gforth の
  DTC NEXT も `node_call` の table-load + indirect も同程度の下限。
- aforth interp と gforth interp は 1.5–3× 範囲で競っている (gforth が
  概ね速いが fib / factorial はほぼ同じ)。aforth interp は AST 探索なので
  threaded code に対して構造的に不利。AOT で逆転する。
- 内側ループが SD 1 本に収まる bench (collatz / sieve / tak / array_sum) で
  AOT は 10× 級。gcc が basic block 全体を見渡して unroll / hoist できる。
- 再帰主体の bench (fib / ack / factorial) は 3× 程度。`node_call` が
  `@noinline` で table-load + 間接 dispatch なので、call ごとの最低コスト
  でサチる。

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
