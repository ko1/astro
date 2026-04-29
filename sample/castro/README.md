# castro — a C subset on ASTro

ASTro 上で動く C のサブセット。tree-sitter-c でパースして型解決済みの
S 式 IR に落とし、castro 本体（C）が ASTro ノード木に組み立ててから
インタプリタ／specialized code を実行する。

`VALUE` は受け側が型を選ぶ union (`int64_t .i / double .d / void *.p`)。
四則・比較・ローカル参照などのノードが int／double 別に定義されており、
`parse.rb` が暗黙変換を `cast_id` / `cast_di` で明示化する。

## 構成

| ファイル | 役割 |
|---|---|
| `node.def` / `castro_gen.rb` | ASTro ノード定義（int + double 両対応） |
| `context.h` / `node.h` / `node.c` / `main.c` | ランタイム + S 式ロード + REPL |
| `parse.rb` | tree-sitter-c → 型解決済み S 式 IR |
| `bench.rb` | castro vs gcc -O0..-O3 比較 |
| `run_tests.sh` | c-testsuite ランナー |
| `parsers/c.so` | tree-sitter-c grammar（ビルド要） |
| `examples/` | サンプル + ベンチマーク C ソース |

## 必要なもの

- Ruby 3.0+ + `ruby_tree_sitter` gem (`gem install ruby_tree_sitter`)
- libtree-sitter (apt: `libtree-sitter0`)
- gcc, make, ruby（ASTroGen 実行用）

## ビルド

```sh
# tree-sitter-c grammar を ./parsers/c.so にビルド（初回のみ）
git clone --depth=1 https://github.com/tree-sitter/tree-sitter-c tmp/ts-c
gcc -shared -fPIC -O2 -I tmp/ts-c/src -o parsers/c.so tmp/ts-c/src/parser.c

# ASTro 生成 + castro バイナリ
make
```

## 実行

```sh
./castro examples/fib.c            # specialize して実行（cache あれば）
./castro --no-compile examples/fib.c   # インタプリタ
./castro --compile-all examples/fib.c  # specialize（コード生成 + .so build）
```

## ベンチマーク

```sh
ruby bench.rb              # 6 ベンチを castro / gcc -O0..-O3 で比較
BENCH_RUNS=10 ruby bench.rb  # median を 10 回から取る
```

## c-testsuite

```sh
# テストを取得（リポジトリには含めない）
mkdir -p testsuite/c-testsuite && cd tmp \
  && gh api repos/c-testsuite/c-testsuite/tarball/master > cts.tgz \
  && tar xzf cts.tgz \
  && mv c-testsuite-c-testsuite-*/tests/single-exec ../testsuite/c-testsuite/

# 実行
./run_tests.sh
```

## Phase 1 で動くもの

- `int` / `double` 算術・比較・ビット演算・代入演算子
- 制御構文（`if`/`while`/`do-while`/`for`/`?:`）
- 関数定義・呼び出し・early `return`（setjmp/longjmp）
- ローカル変数のみ
- ASTro code store（specialized .so の dlopen 再利用）

## まだ無いもの（Phase 2 以降）

- グローバル変数 / 配列 / 構造体 / ポインタ
- `printf` / `putchar` などの FFI
- `switch` / `goto` / `break` / `continue`
- preprocessor（外部 `cpp` 通す前提）
