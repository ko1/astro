# abc — 任意精度 bc 電卓 on ASTro

`abc` は POSIX/GNU `bc` 互換の任意精度電卓を ASTro フレームワーク上に実装した
サンプル。`bc` の核 — **任意精度の十進固定小数点演算 (`scale`)**、変数・配列・
ユーザ定義関数、`if`/`while`/`for`、`ibase`/`obase` による基数変換 — をひと通り
備える。値表現には **GMP** (多倍長整数)、メモリ管理には **libgc** (Boehm GC) を
使う。

設計思想は [`../../docs/idea.md`](../../docs/idea.md)、ASTroGen の使い方は
[`../../docs/usage.md`](../../docs/usage.md) を参照。`calc` (6 ノードの電卓) の次に
読む「言語実装としてひと回り大きいサンプル」という位置づけ。

- 言語仕様: [docs/spec.md](docs/spec.md)
- 実装済み機能 / 制限: [docs/done.md](docs/done.md) / [docs/todo.md](docs/todo.md)
- 値表現・GC・ランタイム: [docs/runtime.md](docs/runtime.md)
- 性能: [docs/perf.md](docs/perf.md)

## 1. ビルドと実行

### 前提パッケージ (Ubuntu/Debian)

```sh
sudo apt install build-essential ruby libgmp-dev libgc-dev libreadline-dev
```

`ruby` (3.x) は ASTroGen 呼び出しに必須。`libgmp-dev` / `libgc-dev` は必須。
`libreadline-dev` は任意 (REPL の行編集・履歴。無くても `fgets` にフォールバック)。

### ビルド

```sh
$ make                  # ASTroGen で node_*.c を生成 → ./abc を build
$ echo 'scale=20; 4*a(1)' | bc -l    # 参考: 本物の bc
$ ./abc -e '2^100'
1267650600228229401496703205376
```

### 使い方 (本物の bc と同じ)

```sh
$ ./abc file.bc                 # ファイルを実行
$ echo '1/3' | ./abc            # 標準入力を実行
$ ./abc                         # tty なら REPL
$ ./abc -e 'scale=10; 1/7'      # ワンライナー (abc 独自の便宜オプション)
$ ./abc -l ...                  # 数学ライブラリ (scale=20。詳細は todo.md)
```

REPL の例:

```
$ ./abc
scale = 10
1/3
.3333333333
define f(n) { if (n<2) return(1); return(n*f(n-1)) }
f(20)
2432902008176640000
```

## 2. bc 言語のさわり

```bc
scale = 30
4 * a(1)            # bc -l なら π。abc は sqrt のみ標準装備

obase = 16          # 16 進出力
255                 # => FF
ibase = 16          # 16 進入力
FF                  # => 255 (出力は obase=10 に戻すと)

a[0] = 1; a[1] = 1
for (i = 2; i < 10; i++) a[i] = a[i-1] + a[i-2]
a[9]                # => 34 (Fibonacci)

define gcd(x, y) {
    auto t
    while (y) { t = x % y; x = y; y = t }
    return(x)
}
gcd(1071, 462)      # => 21
```

bc の肝は「数が `scale` (小数桁数) を持ち歩く」こと。`1.50` は桁を保持して
`1.50` と表示され、除算は `scale` 桁で切り捨てられる。詳しい scale 規則は
[docs/spec.md](docs/spec.md) を参照。

## 3. ASTro 的な見どころ

- **42 種類の AST ノード** ([node.def](node.def)) で bc の式・文を表現。算術ノードは
  `EVAL_ARG` で子をたどり、部分評価器が dispatch チェーンを 1 つの基本ブロックへ
  畳み込める。
- **値は `bcnum *`** (`mpz_t` 仮数 + `scale`)。GMP のアロケータを GC 経由に差し替え
  (`mp_set_memory_functions`)、仮数 limb もろとも Boehm GC が管理する。さらに scale 0 の
  小整数は **LSB タグの即値 (fixnum)** にして GMP/GC を回避する
  ([docs/runtime.md](docs/runtime.md))。
- **AOT 特殊化**: `--aot-compile` で各文・各関数本体を `astro_cs_compile` →
  `astro_cs_build` → `astro_cs_reload` で特殊化 SD に差し替える。関数本体は
  `EVAL(c, f->body)` という runtime ポインタ越しの dispatch なので、各々を独立した
  entry として登録している (cf. usage.md「Entry nodes」)。
- **`--build OUT`** で AST 埋め込み単体実行ファイルを生成 (関数定義を含むプログラムは
  非対応。理由は [docs/todo.md](docs/todo.md))。

## 4. テスト

```sh
$ make check        # = ruby test/run_tests.rb
```

[test/run_tests.rb](test/run_tests.rb) は **差分テスト**: 各プログラムを `abc` と
システムの `bc` 双方に食わせ、**標準出力をバイト単位で比較**する。エラーメッセージの
文面は実装依存なので、エラー系は「双方とも標準出力が空」で検証する。

- 厳選コーナーケース (scale 規則、基数変換、行折り返し、再帰、配列、制御フロー…)
- `test/cases/*.bc` のフィクスチャ (素数・GCD・大きな階乗 等)
- **シード固定ファザ**: 乱数で算術式・比較・基数出力を ~5,000 ケース生成し `bc` と照合

合計で 5,500 ケース超を `bc` と突き合わせている。

## 5. ベンチマーク

```sh
$ make bench        # = ruby benchmark/run_bench.rb
```

`bc` / `abc` (インタプリタ) / `abc` (AOT) を ~1 秒スケールで計測し、出力一致を
確認してから比を出す。要約と解釈は [docs/perf.md](docs/perf.md)。
ざっくり言うと **全ベンチで `bc` より速く (geomean ~6x)**、多倍長が効く処理
(階乗・sqrt) では一桁以上引き離す。小整数は **LSB タグの即値 (fixnum)** で GMP/GC を
回避している。

## 6. ファイル構成

| ファイル | 役割 |
|---|---|
| `node.def` | AST ノード定義 (評価器本体) |
| `context.h` | `VALUE` (`bcnum *`)、`CTX`、オプション |
| `bcnum.{h,c}` | GMP ベースの任意精度十進演算・基数変換・出力 |
| `parse.{h,c}` | トークナイザ + 再帰下降パーサ |
| `node.{h,c}` | ランタイム (シンボル表・関数呼び出し・GC/GMP 配線・EVAL) |
| `main.c` | CLI / REPL / ファイル実行 / `--build` |
| `exe_main.c` | `--build` 生成 exe 用ドライバ |
| `test/` | 差分テスト一式 |
| `benchmark/` | ベンチマーク一式 |
