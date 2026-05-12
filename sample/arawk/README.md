# arawk — POSIX awk subset on ASTro

ASTro 上に乗せた **POSIX awk クローン**。 入力レコードをパターン-アクション
規則で処理する awk スクリプトを実行する CLI。 awk 1003.1-2017 仕様の
正規表現を除く範囲をほぼカバーし、 性能は **gawk の 0.93× geomean**
(goawk 1.07× を上回り gawk 並み、 mawk 1.93× には届かず)。

- 互換: BEGIN / END / pattern-action / `$N` / NR / NF / 連想配列 /
  for-in / user-defined functions / printf+sprintf 全書式 /
  getline 6 形態 / pipe + output redirect (print / printf) /
  close / fflush / system / ENVIRON / ARGC / ARGV / do-while / nextfile
- **UTF-8 対応**: gawk と同じ LC_CTYPE 自動判定 (`length` / `substr` /
  `index` が codepoint 単位)。 `--byte` / `--posix` / `LC_ALL=C` で
  byte mode (mawk 互換)
- 実装: AST 木ウォーカー + ASTroGen の SD specializer + lazy field
  strnum (`$N` 読み時に allocate) + chunked input (fread 64 KB +
  memchr) + for-in bucket walker。 値は 1-bit fixnum タグ +
  `struct arawk_obj` 判別共用体、 ヒープは libgc。
- 最終目標: 姉妹 `sample/astrogre` (regex / are grep CLI) との
  **AST traversal interpreter 統合実験** (`docs/todo.md` Phase 2)。
  正規表現は astrogre 側を library として呼ぶ (Level 1) → 単一
  interpreter で awk+regex 実行 (Level 3, 本命) の段階を想定。

詳細: [`docs/spec.md`](./docs/spec.md) (言語仕様),
[`docs/runtime.md`](./docs/runtime.md) (実装詳解),
[`docs/perf.md`](./docs/perf.md) (ベンチ + 最適化履歴),
[`docs/todo.md`](./docs/todo.md) (残作業 / Phase 2 計画)。
ASTro 本体は [`../../docs/idea.md`](../../docs/idea.md)。

## インストール

### 前提パッケージ (Ubuntu/Debian)

```sh
sudo apt install build-essential ruby libgc-dev gawk mawk
```

- `build-essential` / `ruby` — gcc + ASTroGen (`make` から呼ぶ)
- `libgc-dev` — Boehm-Demers-Weiser conservative GC
- `gawk` / `mawk` — `make bench` の比較対象 (goawk は submodule
  でリポ内に取り込み、 `go build` で binary 生成)

### goawk submodule

```sh
git submodule update --init sample/arawk/goawk
(cd sample/arawk/goawk && go build -o ../goawk-bin .)
```

`benchmark/bench.rb` が gawk / mawk / goawk / arawk-plain / arawk-aot
の 5 実装で goawk の `testdata/tt.*` を回す。

## 試す

```sh
make            # arawk バイナリ
make test       # smoke 218 + tt.* 36 = 254 cases (~11s)
make bench      # gawk / mawk / goawk と速度比較 (~数分)

# awk スクリプトを直接
echo 'a b c
d e f' | ./arawk '{ print $2, NR }'

# 関数 + 配列
./arawk 'function fib(n) { return n < 2 ? n : fib(n-1) + fib(n-2) }
BEGIN { print fib(20) }'

# pipe redirect
echo -e 'b\na\nc' | ./arawk '{ print | "sort" }'

# getline from file
./arawk 'BEGIN { while ((getline line < "/etc/hosts") > 0) print line; close("/etc/hosts") }'
```

## CLI

| flag | 意味 |
|------|------|
| `-e PROG` | プログラムテキスト指定 |
| `-f FILE` | プログラムをファイルから読む |
| `-i` / `--plain` | AOT を使わずインタプリタのみ |
| `-c` / `--aot` | SD を AOT bake してから run |
| `--byte` / `--posix` | LC_CTYPE を無視して byte mode (mawk 互換) |
| `--ccs` | `code_store/` を消してから run |
| `--dump-ast` | parse 結果を dump して run |
| `-b` / `--skip-bake` | (内部) bake step を skip |
| `--aot-compile` | bake のみで run しない |

## ベンチ要約

```
              arawk-plain  arawk-aot   gawk    mawk   goawk
geomean         0.89        0.93       1.00    1.91   1.00
```

| Test | arawk-aot | 備考 |
|---|---|---|
| **tt.14_function_call** | **2.56×** | 1M 回 abs() 呼び (for-in walker で 5×) |
| **tt.x2_sum_loop** | **1.92×** | BEGIN の 10M 回 fixnum 加算 (AOT specialize) |
| **tt.02_print_NR_NF** | **1.45×** | NR/NF/$0 (lazy strnum で field allocate 不要) |
| **tt.07_even_fields** | **1.41×** | `NF % 2 == 0` (field 値読まない) |
| **tt.13a_array_printf** | **1.40×** | 配列 + printf |
| **tt.x1_mandelbrot** | **0.88×** | float ループ |
| tt.01_print | 0.57× | `arawk_split_fields` 47% (まだ改善余地あり) |
| tt.03 系 | 0.65-0.66× | field 走査 + 算術 |

詳細は [`docs/perf.md`](./docs/perf.md)。

## なぜ ASTro 上?

- 単独で awk が欲しいわけではなく、 `sample/astrogre` (regex エンジン)
  との **2 つの AST traversal interpreter を 1 つに統合できるか**
  の実験素材
- ASTro framework の SD specializer + Code Store reload が、 
  awk のような「pattern-action ループ + field 取得 + 算術」が hot な
  ワークロードに効くかを検証する
- 結果として fixnum 算術ループ (tt.x2) / 配列ops (tt.13) / 関数呼出
  (tt.14) で gawk より速くなった。 一方で `split` / I/O のような
  runtime helper が支配的な処では PLT call が固定費で残るので
  AOT の旨味は薄い (`docs/perf.md` 末尾の考察)
