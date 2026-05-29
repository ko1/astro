# AnPy — ChocoPy on ASTro

`AnPy` は [ChocoPy](https://chocopy.org/)（UC Berkeley CS164 で使われる **静的型付き
Python 3.6 サブセット**）を ASTro フレームワーク上に実装したサンプル。型注釈・
単一継承クラス・`int`/`bool`/`str`/リスト・`None`・グローバル/ネスト関数
(`global`/`nonlocal`) を備え、**静的型検査**を行ってから木構造インタプリタで実行する。

ChocoPy の仕様上「妥当な ChocoPy プログラムは妥当な Python 3.6 でもあり、同じ
観測可能セマンティクスを持つ」（仕様 §A の少数の例外を除く）。テストはこの性質を
利用し、`python3` との **差分テスト**で正しさを担保する。

設計思想は [`../../docs/idea.md`](../../docs/idea.md)、ASTroGen の使い方は
[`../../docs/usage.md`](../../docs/usage.md)。仕様は `chocopy.org` の Language Reference。

- 言語仕様（実装した範囲）: [docs/spec.md](docs/spec.md)
- **別実装向けの完全な実装リファレンス**: [docs/chocopy_impl_spec.md](docs/chocopy_impl_spec.md)
- **任意の実装のテスト方法**（差分テストの契約・`ANPY=...` で差し込み）: [docs/testing.md](docs/testing.md)
- 実装済み / 制限: [docs/done.md](docs/done.md) / [docs/todo.md](docs/todo.md)
- 値表現・スコープ・GC: [docs/runtime.md](docs/runtime.md)
- 性能: [docs/perf.md](docs/perf.md)

## 1. ビルドと実行

```sh
sudo apt install build-essential ruby libgc-dev libreadline-dev   # readline は任意
make
./anpy program.py          # ファイルを型検査 → 実行
echo 'print(1+2*3)' | ./anpy   # 標準入力
```

ChocoPy の例（仕様 Figure 2 の動物クラス）:

```python
class animal(object):
    makes_noise:bool = False
    def make_noise(self:"animal") -> object:
        if self.makes_noise:
            print(self.sound())
        return None
    def sound(self:"animal") -> str:
        return "???"
class cow(animal):
    def __init__(self:"cow"):
        self.makes_noise = True
    def sound(self:"cow") -> str:
        return "moo"
c:animal = None
c = cow()
c.make_noise()        # "moo"
```

ChocoPy は Python より厳格な点がいくつかある（[docs/spec.md](docs/spec.md)）:

- **定義は文より前**: トップレベル/関数本体は「変数・関数・クラス定義」を全て先に置き、
  その後に文を書く。
- 全ての変数・属性・引数・戻り値に**型注釈**が必須。
- `print` の引数は `int`/`bool`/`str` のみ（リストやオブジェクトを渡すと実行時エラー）。
- 比較の連鎖（`a < b < c`）不可、`x:list = []` 以外の暗黙変換なし、など。

## 2. ASTro 的な見どころ

- **40 ノード** ([node.def](node.def)) で式・文を表現。`return` は `c->returning`/`c->retval`
  で巻き戻し、`EVAL_ARG` で子をたどるので部分評価器が dispatch チェーンを畳める。
- **タグ付き VALUE**: `int` は即値（LSB=1）、`None`/`True`/`False` は小定数、`str`/list/
  オブジェクト/クロージャ/クラスはヒープポインタ（[docs/runtime.md](docs/runtime.md)）。
  GC は **libgc**。
- **静的型検査** ([check.c](check.c)): 適合 (`<=`)・代入互換 (`<=a`)・join (`⊔`) を実装し、
  仕様 §5.2 の型規則で式・文を検査。型エラーは実行前に弾く（ChocoPy と同じ）。
- **クロージャ**: 関数値はヒープ上の closure（静的記述子 + 捕捉環境）。`nonlocal`/`global`
  はセル共有で実現。メソッドはグローバル環境を捕捉（仕様通り）。
- **AOT 特殊化**: `--aot-compile` でトップレベル文と**各関数/メソッド本体**を
  `astro_cs_compile`。本体は `EVAL(c, fn->body)` 経由の runtime dispatch なので各々を
  独立 entry として登録する（cf. usage.md「Entry nodes」）。

## 3. テスト

```sh
make check       # = ruby test/run_tests.rb
```

[test/run_tests.rb](test/run_tests.rb) は 3 種類:

- **差分テスト**: 妥当な ChocoPy プログラムを `anpy` と `python3` 双方で実行し標準出力を比較
  （ChocoPy print 制限・整数オーバーフロー UB を避けて記述）。
- **実行時エラー**: ゼロ除算・添字範囲外・`None` 操作。両者ともエラー前の出力が一致。
- **型エラー拒否**: 型不一致・arity 違反・bool でない条件等は `anpy` が**静的に拒否**し、
  同じプログラムを `python3` は実行できる（＝静的検査だけの差であることを確認）。
- フィクスチャ `test/cases/*.py`（クイックソート・二分探索木・エラトステネスの篩）。

## 4. ベンチマーク

```sh
make bench       # = ruby benchmark/run_bench.rb
```

`python3` / `anpy`(interp) / `anpy`(AOT) を ~1 秒スケールで計測し出力一致を確認。
要約は [docs/perf.md](docs/perf.md)。**ループ主体の処理では CPython より速く**
(loop/collatz/list ~0.4–0.5×)、関数呼び出し主体では呼び出しごとの環境フレーム確保の
ぶん遅い（tak/fib）。

## 5. ファイル構成

| ファイル | 役割 |
|---|---|
| `node.def` | AST ノード定義（インタプリタ） |
| `context.h` | VALUE タグ表現 / CTX / ヒープオブジェクト |
| `value.{h,c}` | 値の生成・演算・組み込み（print/len/input）・実行時エラー |
| `lexer.c` | インデント対応トークナイザ（INDENT/DEDENT） |
| `parse.{h,c}` | 再帰下降パーサ（AST + 関数/クラス記述子） |
| `check.{h,c}` | 静的型検査（型表現・適合/代入互換/join・規則検査） |
| `node.{h,c}` | ランタイム（環境・呼び出し・dispatch・GC・EVAL/INIT） |
| `main.c` / `exe_main.c` | CLI / `--build` exe ドライバ |
| `test/` `benchmark/` `docs/` | テスト・ベンチ・ドキュメント |
