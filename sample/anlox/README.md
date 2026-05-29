# AnLox — Lox on ASTro

**AnLox** は **An\* シリーズ**（*ASTro **N**utshell* = 定番の題材言語を ASTro 上に
「一口サイズ」で実装するシリーズ。[AnPy](../anpy/) = ChocoPy、[AnCaml](../ancaml/) =
MinCaml、AnLox = Lox、…）の一員。バイナリ名・ディレクトリは小文字 `anlox`。

AnLox は [Lox](https://craftinginterpreters.com/)（Robert Nystrom『Crafting
Interpreters』の題材言語）を ASTro フレームワーク上に実装した**木構造インタプリタ**
（書籍の "jlox" 相当）。**動的型付け**で、nil / bool / 数値（double）/ 文字列、
**第一級クロージャ**、**単一継承クラス**（`this` / `super` / 動的ディスパッチ）を持つ。

設計思想は [`../../docs/idea.md`](../../docs/idea.md)、ASTroGen の使い方は
[`../../docs/usage.md`](../../docs/usage.md)。

- 言語仕様（実装範囲）: [docs/spec.md](docs/spec.md)
- **別実装向けリファレンス**: [docs/lox_impl_spec.md](docs/lox_impl_spec.md)
- テスト方法（`// expect:` 形式・`ANLOX_REF=` で本家差分）: [docs/testing.md](docs/testing.md)
- 値表現・スコープ・GC: [docs/runtime.md](docs/runtime.md)
- 実装済み / 制限: [docs/done.md](docs/done.md) / [docs/todo.md](docs/todo.md)
- 性能: [docs/perf.md](docs/perf.md)

## 1. ビルドと実行

```sh
sudo apt install build-essential ruby libgc-dev
make
./anlox program.lox
echo 'print "Hello, " + "Lox!";' | ./anlox
```

例（クラス + 継承 + super）:

```lox
class Animal {
  init(name) { this.name = name; }
  speak() { print this.name + " makes a sound."; }
}
class Dog < Animal {
  speak() { super.speak(); print this.name + " barks."; }
}
Dog("Rex").speak();   // Rex makes a sound. / Rex barks.
```

## 2. ASTro 的な見どころ

- **38 ノード** ([node.def](node.def))。子は `EVAL_ARG` で辿るので部分評価器が dispatch
  チェーンを 1 つの SD に畳める。可変長の子（文リスト・呼び出し引数・メソッド集合）は
  side-table（`LOX_BLOCK_STMTS` / `LOX_CALL_ARGS` / `LOX_FUNDEFS`）に置き、各々を AOT entry
  として登録する。
- **タグ付き VALUE**: nil / true / false は小定数、数値（double）/ 文字列 / クロージャ /
  クラス / インスタンスはヒープポインタ（[docs/runtime.md](docs/runtime.md)）。GC は **libgc**。
- **resolver**: 本家同様、ローカル変数をパース時に `(depth, slot)` フレーム座標に解決
  （`node_local`）。global は遅延束縛の名前テーブル。`this` / `super` も local として解決。
- **クラス**: メソッドはクロージャ。プロパティ参照時に `this` を束縛した新クロージャを返す。
  `super` は class 定義時に push する `super` フレーム経由で解決。
- **AOT 特殊化**: `--aot-compile` でトップレベル・各関数本体・side-table の各子を
  `astro_cs_compile`（関数本体や文ノードは runtime dispatch なので独立 entry）。

## 3. テスト

```sh
make check       # = ruby test/run_tests.rb
```

Lox には system 実装が無いので、本家公式の **`// expect:` 注釈形式**（期待出力をソースに
埋め込む自己完結テスト）を使う（[docs/testing.md](docs/testing.md)）。`ANLOX_REF=/path/to/jlox`
を渡せば本家との差分テストも追加できる。

## 4. ベンチマーク

```sh
make bench       # interp vs AOT, ~0.5s スケール
```

要約は [docs/perf.md](docs/perf.md)。

## 5. ファイル構成

| ファイル | 役割 |
|---|---|
| `node.def` | AST ノード定義（インタプリタ） |
| `context.h` | VALUE タグ表現 / CTX / ヒープオブジェクト / フレーム |
| `value.c` | 値の生成・適用・クラス/プロパティ・等価・組み込み（clock）・エラー |
| `lexer.c` | トークナイザ |
| `parse.{h,c}` | 再帰下降 + Pratt パーサ + resolver（スコープ・this/super・side-table） |
| `node.{h,c}` | ランタイム（node 割当・code store 配線・EVAL/INIT/AOT） |
| `main.c` | CLI ドライバ |
| `test/` `benchmark/` `docs/` | テスト・ベンチ・ドキュメント |
