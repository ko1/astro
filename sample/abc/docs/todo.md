# abc 既知の制限 / TODO

実装済みは [done.md](done.md)。

## 言語機能の未実装

- **数学ライブラリ (`bc -l` の `a c e l s j`)**: 未実装。`-l` は現状 `scale=20` を
  設定するだけ (`sqrt` は標準で利用可)。Taylor 級数等を abc 自身で書いて embed する
  のが筋だが、bc との最終桁一致 (truncation) を合わせ込むのが手間で保留。`main.c` の
  `abc_math_lib()` に embed する想定で枠だけ用意してある。
- **`read()`**: 標準入力から数を読む組み込みは未対応 (現状は未定義関数扱い)。
- **配列の引数渡し**: 仮引数はスカラのみ。`define f(a[])` の配列値渡しは未対応
  (auto 配列・グローバル配列は可)。
- **`ibase != 10` の小数リテラル**: 整数部は厳密。小数部は
  `trunc(frac · 10^k / ibase^k)` 近似で、bc と最終桁が食い違う場合がある
  (10 進入力は厳密)。
- **`quit` の即時性**: bc は字句解析時点で終了するが、abc は実行時 (`node_halt`) に
  終了する。`if (0) quit` の扱いが bc と異なる。
- **エラーメッセージの文面**: bc と非互換 (差分テストは「両者とも stdout 空」で検証)。

## フレームワーク連携

- **`--build` と関数定義**: `define` はパース時にシンボル表へ登録する設計のため、
  AST だけを埋め込む `--build` 単体 exe では関数が失われる。関数定義を含まない
  プログラムのみ `--build` 対応。
  - 解決案: `define` を実行時ノード化し、関数メタデータ (名前・引数・auto・本体) を
    operand として直列化する。`bc_func *` 用のカスタム operand emitter
    (`build_emit_ast`) が必要 (cf. usage.md「Embedder hooks」, naruby の例)。
- **PG (profile-guided) モード**: 未配線 (`HOPT == HORG == HASH`)。

## 性能 (詳細は [perf.md](perf.md))

- ~~小整数ループが bc の ~2 倍遅い~~ → **fixnum 即値で解決** (全ベンチで bc より速く
  geomean ~6x)。
- **算術 helper の inline 化 (未着手)**: AOT 特殊化が今もほぼ横ばい (geomean ~1.05x)。
  `bc_add`/`bc_mul` 等が out-of-line helper 呼び出しとして SD に残るのが原因。
  fast-path を node.def に展開 (static-inline / statement-expr macro 化して EVAL_ARG
  経由で SD に取り込む) すれば dispatch 除去 + 即値演算 inline の取り分が出る見込み。
- **bcnum プール / アリーナ (未着手)**: 大きな scale の中間値の確保削減。

## 出てきたら追記

作業中に見つけたバグ・回避・未達はここに即追記する (ルート方針に従う)。
