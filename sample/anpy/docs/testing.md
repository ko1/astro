# ChocoPy 実装のテスト方法

実装リファレンスは [chocopy_impl_spec.md](chocopy_impl_spec.md)。本書は、AnPy だけ
でなく **任意の ChocoPy 実装**を同じテストで検証する方法をまとめる。

## 1. 根拠 — なぜ `python3` と比較できるか

ChocoPy 仕様より「妥当な ChocoPy プログラムはほぼ全て妥当な Python 3.6 でもあり、
エラーなく走るなら同じ観測可能セマンティクスを持つ」（§A の例外を除く）。
したがって**正常系は `python3` を黄金基準**にできる（CPython 3.6〜3.12 で本テスト
範囲の挙動は同一）。

§A の非互換は次の3点に注意して回避する:
- **前方参照**: クラスを定義前に型注釈で使うなら引用形 `x:"C" = None`。
- **整数オーバーフロー**: ChocoPy は UB。値を 32bit 範囲内に収める。
- **`print` の引数**: `int`/`bool`/`str` のみ（リスト/オブジェクトを渡すと ChocoPy は
  実行時エラー、Python は印字して食い違う）。

## 2. 実装が満たすべき CLI 契約

テストハーネスは実装バイナリに対し次を仮定する:

- **標準入力から ChocoPy プログラムを読み、実行し、結果を標準出力へ書く。**
- **静的エラー（型/スコープ）時**: 終了コード非0、かつ**標準出力に何も出さない**
  （= プログラムを実行しない）。
- **実行時エラー時**: そこまでに出した出力を残してから中断する（終了コードは不問）。
- `print` は値を印字して**改行**を付ける（`bool`→`True`/`False`、`int`→10進、`str`→そのまま）。

ファイル引数や独自オプションがあってもよいが、上記の **stdin→stdout** 経路は必須。
（AnPy は `anpy <file>` でも `anpy < file` でも動く。）

## 3. 走らせ方

```sh
# AnPy 自身（デフォルト）
make check
#   または
ruby test/run_tests.rb

# 別実装を検証（バイナリへのパスを ANPY に渡す）
ANPY=/path/to/your_chocopy ruby test/run_tests.rb

# 黄金基準の Python を変える場合
PYTHON=python3.6 ruby test/run_tests.rb
```

終了コードは全 pass で 0、失敗があれば非0。失敗時は最初の数件について
`anpy` と `python` の出力（または期待）を表示する。

## 4. テストの 4 カテゴリ（[test/run_tests.rb](../test/run_tests.rb)）

1. **正常系（差分）** `diff(name, prog)`:
   実装と `python3` の**標準出力をバイト単位で比較**。`print` 制限・32bit・前方参照を
   守った妥当 ChocoPy プログラムのみ。
2. **実行時エラー（差分）** `diff("rt:…", prog)`:
   ゼロ除算・添字範囲外・`None` 操作など。両者ともエラー前の標準出力が一致すればよい
   （エラーメッセージ自体は stderr で比較しない）。
3. **型エラー拒否** `reject(name, prog)`:
   実装が**静的に拒否**（終了コード非0 かつ標準出力が空）すること。各プログラムは
   「もし実行されたら出力する」形にしてあるので、型検査をしない実装は出力を出して
   失敗する＝判別力がある。
4. **フィクスチャ** `test/cases/*.py`:
   実プログラム（quicksort / 二分探索木 / エラトステネスの篩）。カテゴリ1と同じく
   `python3` と差分比較。

## 5. ケースの足し方

- 正常系: `VALID` ハッシュに `'名前' => "プログラム\n..."` を足す。**妥当 ChocoPy** で
  あり、§1 の注意を守ること（`print` は int/bool/str、32bit、前方参照は引用）。
- 実行時エラー: `RUNTIME` ハッシュへ。エラー前に何か `print` しておくと、出力プレフィクス
  一致で挙動を確認できる。
- 型エラー: `reject('名前', "プログラム")`。**実行されれば出力する**ように書く
  （例: 末尾に `print(...)`）。型検査しない実装を炙り出せる。
- 大きめのプログラム: `test/cases/<名前>.py` に置けば自動で差分テストに加わる。

## 6. 自前テストへの最小スモーク

新実装の最初の確認に便利な一行群（全て `python3` と一致するはず）:

```sh
printf 'print(1 + 2 * 3)\n'                  | ./your_chocopy   # 7
printf 'print(-7 // 2)\nprint(-7 %% 3)\n'    | ./your_chocopy   # -4 / 1
printf 'def f(n:int)->int:\n if n<2:\n  return n\n return f(n-1)+f(n-2)\nprint(f(10))\n' | ./your_chocopy  # 55
```

`make check`（48 ケース）が全部通れば、AnPy と同等の適合度に達していると見なせる。
カバレッジを上げたい場合は [chocopy_impl_spec.md](chocopy_impl_spec.md) §3.6 の構造規則
（全経路 return、オーバーライド一致、`[<None>]` 多重代入禁止 等）に対応する `reject`
ケースを足すとよい。
