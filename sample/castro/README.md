# castro — a C subset on ASTro

ASTro 上で動く C のサブセット。tree-sitter-c でパースして型解決済みの
S 式 IR に落とし、castro 本体 (C) が ASTro ノード木に組み立ててから
インタプリタ / specialized code を実行する。

実装済み機能・未対応・既知の制限・ランタイム構造はそれぞれ分離した
ドキュメントを参照:

- [docs/done.md](docs/done.md) — 実装済み機能インベントリ + ベンチ + テスト数
- [docs/todo.md](docs/todo.md) — 未対応 / 既知の制限 (goto のネスト制約など)
- [docs/runtime.md](docs/runtime.md) — VALUE / フレーム / 関数呼び出し (parse 時 index 解決) / goto-dispatch の仕組み
- [docs/perf.md](docs/perf.md) — 性能最適化の経緯 / 採用した手法 / ボツ案 / 残るギャップ

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
| `node.def` / `castro_gen.rb` | ASTro ノード定義 (int + double 両対応、`uint64_t` literal specializer 上書き) |
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

## ベンチ結果 (gcc -O0 と -O3 比較、ms median; `BENCH_RUNS=15`)

`bench.rb` は parse.rb のスタートアップ (~110ms) を含めず純粋な実行時間を比較。
全 11 件。

| bench              | interp | AOT first | AOT cached | gcc-O0 | gcc-O3 |
|--------------------|-------:|----------:|-----------:|-------:|-------:|
| fib_big (fib 35)   |    427 |        69 |     **48** |     47 |     16 |
| fib_d              |     15 |        23 |      **4** |      3 |      1 |
| tak (18,12,6)      |      4 |        21 |      **2** |      0 |      0 |
| ackermann (3,8)    |    119 |        31 |     **12** |      8 |      1 |
| loop_sum           |    263 |        21 |   🟢 **2** | 🔴  9  |      0 |
| mandelbrot_count   |     25 |        23 |      **3** |      2 |      1 |
| sieve              |    167 |        28 |     **8**  |      8 |      2 |
| nqueens            |    582 |        89 |     **69** |     31 |     14 |
| quicksort          |   3110 |        96 |  🟢 **84** | 🔴 92  |     23 |
| crc32              |    831 |        33 |  🟢 **14** |     46 | 🔴 18  |
| matmul             |     58 |        27 |     **8**  |      2 |      0 |

**castro が gcc を上回るベンチ**:
- `crc32`: 14 ms vs gcc -O3 18 ms (gcc -O3 を 1.3× 上回る)
- `loop_sum`: 2 ms vs gcc -O0 9 ms (4.5× 速い)
- `quicksort`: 84 ms vs gcc -O0 92 ms (1.1× 速い)

採用した最適化と、その効果の内訳は [docs/perf.md](docs/perf.md) を参照。
主な高速化:
- **コンパイル時に呼び先決定**: `(call NAME ...)` → `(call FUNC_IDX ...)` で
  callcache / serial / 名前ルックアップを全廃 (1442ms → 64ms)
- **`VALUE *fp` を common_param に格上げ**: `c->fp += / -=` の memory 往復が
  消え、tight inner loop が register 化される (loop_sum -60%, crc32 が gcc -O3 超え)
- **parse 時に `node_call` / `node_call_jmp` 振り分け**: 実行時 setjmp 分岐削除
  (fib_big -27%, nqueens -16%)
- **cross-SD direct call**: `SPECIALIZE_node_call` で `extern SD_<callee_hash>`
  を baked、`-Wl,-Bsymbolic` で intra-`.so` 参照を bind local にして真の
  direct call (`addr32 call SD_xxx`) に
- **tail-return リフト** / **break/continue 振り分け**: setjmp 不要なケースで 0 個
- **`-rdynamic` ビルド**: これがないと SD が host helper を解決できず黙って
  interp に fallback する (= AOT 効果ゼロ)

## テスト結果

- **feature tests** (`make test`): **30/30** passed (interp / AOT first / AOT cached × stdout + exit code 一致)
- **c-testsuite** (`make test-cts`): **173/220** passed (78.6%)

残りの c-testsuite failure は union / bit-field / 関数を返す関数 / variadic
ユーザ関数 / `_Generic` / structured-goto と、それらに依存するテストが大半。
詳細とそれぞれの改善案は [docs/todo.md](docs/todo.md)。
