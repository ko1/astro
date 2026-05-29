# AnCaml — MinCaml on ASTro

**AnCaml** は **An\* シリーズ**（*ASTro **N**utshell* = 定番の題材言語を ASTro 上に
「一口サイズ」で実装するシリーズ。[AnPy](../anpy/) = ChocoPy、AnCaml = MinCaml、…）の一員。
バイナリ名・ディレクトリは小文字 `ancaml`。

AnCaml は [MinCaml](https://esumii.github.io/min-caml/)（東北大の住井英二郎による教育用
最適化コンパイラの題材言語）を ASTro フレームワーク上に実装したサンプル。MinCaml は
**単相（モノモーフィック）の極小 ML サブセット**で、値は unit / bool / int / float と
ヒープ上の closure / tuple / array の 5 種類だけ。文字列もリストも多相もない。プログラムは
**ただ 1 つの式**。

`ancaml` は MinCaml のオリジナル実装と同じく **Hindley–Milner 型推論**（破壊的単一化）で
静的に型検査してから、木構造インタプリタで評価する。MinCaml は OCaml の（ほぼ）サブセット
なので、テストは `ocaml` との **差分テスト**で正しさを担保する。

設計思想は [`../../docs/idea.md`](../../docs/idea.md)、ASTroGen の使い方は
[`../../docs/usage.md`](../../docs/usage.md)。

- 言語仕様（実装した範囲）: [docs/spec.md](docs/spec.md)
- **別実装向けの完全な実装リファレンス**: [docs/mincac_impl_spec.md](docs/mincac_impl_spec.md)
- **任意の実装のテスト方法**（差分テストの契約・`ANCAML=...` で差し込み）: [docs/testing.md](docs/testing.md)
- 値表現・スコープ・GC: [docs/runtime.md](docs/runtime.md)
- 実装済み / 制限: [docs/done.md](docs/done.md) / [docs/todo.md](docs/todo.md)
- 性能: [docs/perf.md](docs/perf.md)

## 1. ビルドと実行

```sh
sudo apt install build-essential ruby libgc-dev   # 差分テストには ocaml も
make
./ancaml program.ml                 # ファイルを型検査 → 実行
echo 'print_int (1 + 2)' | ./ancaml  # 標準入力
```

MinCaml の例（フィボナッチ）:

```ocaml
let rec fib n = if n <= 1 then n else fib (n - 1) + fib (n - 2) in
print_int (fib 30); print_newline ()
```

MinCaml は OCaml より厳しい / 異なる点がいくつかある（[docs/spec.md](docs/spec.md)）:

- **整数の乗除算がない**。`+ - (単項 -)` のみ。乗算は float の `*.`、除算は `/.`。
- **文字列もリストもない**。配列は `Array.create n init`（`Array.make` も可）、
  `a.(i)`、`a.(i) <- v`。
- **多相がない**（単相 HM）。残った型変数は int に default（MinCaml と同じ）。
- 比較は `=`, `<>`, `<`, `>`, `<=`, `>=` を `Eq` / `LE` / `Not` に desugar する
  （MinCaml のパーサと同じ）。
- 外部関数は `print_int` / `print_newline` / `print_char` / `print_float` /
  `read_int` / `read_float` / `float_of_int` / `int_of_float` / `truncate` /
  `sqrt` / `sin` / `cos` / `atan` / `floor` / `abs_float` のみ。

## 2. ASTro 的な見どころ

- **38 ノード**（[node.def](node.def)）で MinCaml の式を表現。子は `EVAL_ARG` で辿るので
  部分評価器が dispatch チェーンを 1 つの SD に畳める。
- **タグ付き VALUE**: `int` は即値（LSB=1）、`unit`/`true`/`false` は小定数、
  `float`/closure/tuple/array はヒープポインタ（[docs/runtime.md](docs/runtime.md)）。GC は **libgc**。
- **de Bruijn フレーム**: 変数はパーサが `(depth, idx)` 座標に解決し、実行時は
  フレーム鎖を辿って slot を読むだけ（名前ハッシュなし）。`node_lref` がこれ。
- **Hindley–Milner 型推論** ([type.c](type.c)): `Type.Var` セルを使った破壊的単一化。
  let は単相。最初の型エラーで停止し行番号付きで報告（MinCaml と同じ）。
- **末尾呼び出し最適化**: パース時の tail-marking パスが末尾位置の適用を `node_tail_*`
  に書き換え、`ac_apply` のトランポリンで O(1) スタックで回す（数百万回の末尾ループも可）。
  in-place な kind+dispatcher 差し替えなので `--build` の AST 埋め込み・AOT も追従する。
- **leaf フレームの alloca**: 本体がクロージャを作らない関数（`is_leaf`）は引数フレームを
  GC ではなく C スタックに置く。整数再帰で interp 比 ~3×（[docs/perf.md](docs/perf.md)）。
- **AOT 特殊化**: `--aot-compile` でトップレベル式と**各関数本体**を `astro_cs_compile`。
  関数本体は `ac_apply` 内の `(*body->head.dispatcher)(c, body)` 経由で呼ばれる
  runtime dispatch なので、各々を独立 entry として登録する（cf. usage.md「Entry nodes」）。

## 3. テスト

```sh
make check       # = ruby test/run_tests.rb （ocaml が必要）
```

[test/run_tests.rb](test/run_tests.rb) は 3 種類:

- **差分テスト**: 妥当な MinCaml プログラムを `ancaml` と `ocaml` 双方で実行し標準出力を比較。
  MinCaml と OCaml の 2 つの表層差（`print_char` が int を取る点、`Array.create` が
  現行 OCaml から削除された点）は小さな prelude で吸収する。
- **型エラー拒否**: 型不一致・arity 違反・bool でない条件・未束縛変数等は `ancaml` が
  **静的に拒否**する（同じテキストを `ocaml` は受理することがある＝静的検査だけの差）。
- フィクスチャ `test/cases/*.ml`。

## 4. ベンチマーク

```sh
make bench       # = ruby benchmark/run_bench.rb
```

`ancaml`(interp) / `ancaml`(AOT) / `ocaml`(bytecode) / `ocamlopt`(native) を ~1 秒スケールで
計測し出力一致を確認する。要約は [docs/perf.md](docs/perf.md)。

## 5. ファイル構成

| ファイル | 役割 |
|---|---|
| `node.def` | AST ノード定義（インタプリタ） |
| `context.h` | VALUE タグ表現 / CTX / ヒープオブジェクト / フレーム |
| `value.c` | 値の生成・適用・外部関数・構造的等価/順序・実行時エラー |
| `lexer.c` | トークナイザ（ネスト可能 `(* *)` コメント） |
| `parse.{h,c}` | 再帰下降パーサ（スコープ解決・比較の desugar・可変長ノードの side-table） |
| `type.{h,c}` | Hindley–Milner 型推論（単一化・外部関数シグネチャ） |
| `node.{h,c}` | ランタイム（node 割当・code store 配線・EVAL/INIT/AOT） |
| `main.c` / `exe_main.c` | CLI / `--build` exe ドライバ |
| `test/` `benchmark/` `docs/` | テスト・ベンチ・ドキュメント |
