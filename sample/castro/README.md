# castro — a C subset on ASTro

ASTro 上で動く C のサブセット。tree-sitter-c でパースして型解決済みの
S 式 IR に落とし、castro 本体 (C) が ASTro ノード木に組み立ててから
インタプリタ / specialized code を実行する。

実装済み機能・未対応・既知の制限・ランタイム構造はそれぞれ分離した
ドキュメントを参照:

- [docs/done.md](docs/done.md) — 実装済み機能インベントリ + ベンチ + テスト数
- [docs/todo.md](docs/todo.md) — 未対応 / 既知の制限 (goto のネスト制約など)
- [docs/runtime.md](docs/runtime.md) — VALUE / フレーム / callcache / goto-dispatch の仕組み

## 値表現の概要

`VALUE` は受け側が型を選ぶ 8-byte union (`int64_t .i / double .d / void *.p`)。
四則・比較・ローカル参照などのノードが int / double 別に定義されており、
`parse.rb` が暗黙変換を `cast_id` / `cast_di` で明示化する。

すべてのデータ (int / char / double / pointer / 配列要素 / 構造体フィールド)
は **1 要素 = 1 VALUE slot** で配置され、ポインタ算術も slot 単位 (`p+1` = +8 byte)。
`sizeof(T)` は host C 互換の ABI バイト数を返すが、メモリレイアウト自体は
slot 単位。文字列リテラル `"hello"` も 6 個の slot に展開され、`printf("%s", s)`
などは実行時に連続 byte 列に再構築して host libc に渡す。詳細は
[docs/runtime.md](docs/runtime.md)。

## 構成

| ファイル | 役割 |
|---|---|
| `node.def` / `castro_gen.rb` | ASTro ノード定義 (int + double 両対応、`@ref` callcache、`uint64` specializer 上書き) |
| `context.h` / `node.h` / `node.c` / `main.c` | ランタイム + S 式ロード + 関数テーブル + printf 実装 |
| `parse.rb` | tree-sitter-c → 型解決済み S 式 IR (CType / 型推論 / tail-return リフト / structured break/continue / 簡易 goto) |
| `bench.rb` | castro vs gcc -O0..-O3 比較 (parse.rb 起動時間を除く実行時間で比較) |
| `run_tests.sh` | c-testsuite ランナー |
| `tests/run.sh` | feature tests ランナー (interp + AOT first + AOT cached × stdout/exit 一致) |
| `parsers/c.so` | tree-sitter-c grammar (ビルド要) |
| `examples/` | ベンチマーク C ソース |
| `tests/test_*.c` | 機能ごとの動作確認 |
| `docs/` | done / todo / runtime |

## 必要なもの

- Ruby 3.0+ + `ruby_tree_sitter` gem (`gem install ruby_tree_sitter`)
- libtree-sitter (apt: `libtree-sitter0`)
- gcc, make, ruby (ASTroGen 実行用)

## ビルド

```sh
# tree-sitter-c grammar を ./parsers/c.so にビルド (初回のみ)
git clone --depth=1 https://github.com/tree-sitter/tree-sitter-c tmp/ts-c
gcc -shared -fPIC -O2 -I tmp/ts-c/src -o parsers/c.so tmp/ts-c/src/parser.c

# ASTro 生成 + castro バイナリ
make
```

## 実行

```sh
./castro examples/fib.c              # cache を読み込んで実行 (なければ interp と同等)
./castro --no-compile examples/fib.c # 純 interp
./castro --compile-all examples/fib.c # SD\_<hash>.c → all.so 生成 + 実行
```

## テスト・ベンチ

```sh
make           # ビルド
make test      # tests/test_*.c × {interp, AOT first, AOT cached}
make test-cts  # c-testsuite (要 testsuite/c-testsuite/single-exec/)
make bench     # ベンチマーク (BENCH_RUNS=N で median 件数調整)
```

c-testsuite は別途取得 (リポジトリ非同梱):

```sh
mkdir -p testsuite tmp && cd tmp \
  && gh api repos/c-testsuite/c-testsuite/tarball/master > cts.tgz \
  && tar xzf cts.tgz \
  && mv c-testsuite-c-testsuite-*/tests/single-exec ../testsuite/c-testsuite/
```

## 主要な動作 (詳細は [docs/done.md](docs/done.md))

- `int` / `double` 算術・比較・ビット演算・代入演算子
- 制御構文 (`if` / `while` / `do-while` / `for` / `?:` / `switch` / `break` / `continue` / `goto`*)
- 関数定義・呼び出し・early `return` (tail return は parse 時にリフトして setjmp 回避)
- グローバル変数 (initializer 含む)、ローカル変数
- ポインタ・配列 (添字、ポインタ算術、ポインタ差)
- 構造体 (フィールドアクセス `s.f` / `s->f`、初期化子、designated initializer `.field = val`)
- 関数ポインタ (`int (*op)(int,int)` 経由の間接呼び出し)
- 文字列リテラル (VALUE-slot に展開)、`char[]` バッファ
- `typedef`、`enum`
- libc-ish: `printf` / `putchar` / `puts` / `malloc` / `free` / `calloc` / `strlen` / `strcmp` / `strncmp` / `strcpy` / `strncpy` / `strcat` / `memset` / `memcpy` / `atoi` / `exit` / `abs`
- preprocessor: parse.rb 側で `gcc -E` を呼ぶ (`NO_CPP=1` で無効化可)
- ASTro code store (specialized .so の dlopen 再利用)

\* `goto` は **関数のトップレベル seq にあるラベルのみ** 対応。for/while/if/switch
の内側のラベルへ飛ぶケースは未対応 (`unknown op: _label_marker` で落ちる)。
詳細と改善案は [docs/todo.md](docs/todo.md) §goto。

## ベンチ結果 (gcc -O0 比、ms median; `BENCH_RUNS=5`)

`bench.rb` は parse.rb のスタートアップ (~110ms) を含めず純粋な実行時間を比較。

| bench              | interp | AOT first | AOT cached | gcc-O0 | gcc-O3 |
|--------------------|-------:|----------:|-----------:|-------:|-------:|
| fib_big (fib 35)   |    492 |        83 |     **64** |     46 |     16 |
| fib_d              |     17 |        23 |      **4** |      3 |      1 |
| tak (18,12,6)      |      3 |        21 |      **2** |      0 |      0 |
| ackermann (3,8)    |    120 |        31 |     **11** |      8 |      1 |
| loop_sum           |    252 |        23 |    **4** 🟢| 🔴  9  |      0 |
| mandelbrot_count   |     26 |        22 |      **3** |      2 |      1 |

- AOT cached が gcc -O0 を上回るのは loop_sum (4ms vs 9ms、2.25× 速)
- 残り 5 ベンチも gcc -O0 の 1.3〜1.5× 圏内 (fib_big 1.39×、ackermann 1.38×)
- 主な高速化:
  - **コンパイル時に呼び先決定**: `(call NAME ...)` を `(call FUNC_IDX ...)` に変更し、parse 時に解決した index で `c->func_set[idx]` を直接引く。`@ref` callcache + serial 検証 + dispatcher snapshot を全部削除
  - tail-return リフト (parse.rb)
  - break/continue 振り分け (parse.rb)
  - `-rdynamic` ビルド (これがないと SD が黙って interp に fallback する)

## テスト結果

- **feature tests** (`make test`): **30/30** passed (interp / AOT first / AOT cached × stdout + exit code 一致)
- **c-testsuite** (`make test-cts`): **173/220** passed (78.6%)

残りの c-testsuite failure は union / bit-field / 関数を返す関数 / variadic
ユーザ関数 / `_Generic` / structured-goto と、それらに依存するテストが大半。
詳細とそれぞれの改善案は [docs/todo.md](docs/todo.md)。
