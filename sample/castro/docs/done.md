# castro 実装済み機能

未実装機能 / 既知の制限は [todo.md](todo.md) を参照。

## 言語機能

### 型
- 整数: `char`, `signed char`, `unsigned char`, `_Bool` / `bool`, `short`, `unsigned short`, `int`, `unsigned`, `long`, `unsigned long`, `long long`, `unsigned long long`, `size_t`, `ssize_t`, `ptrdiff_t`, `intptr_t`, `uintptr_t`
- 浮動小数: `float`, `double`, `long double` — すべて `double` として実行 (`.d` slot)
- `void`
- ポインタ: `T*`, `T**`, ...
- 配列: `T[N]`, `T[]` (initializer から要素数推論)
- 構造体: `struct tag { ... }`, 無名 `struct {...}`
- `typedef T name`, `typedef struct { ... } name`
- `enum`: enumerator は定数として global table に入る

> 内部表現はすべて 8-byte VALUE スロットに正規化される。`sizeof()` は host C ABI の値 (`sizeof(int)==4`, `sizeof(char)==1`) を返すが、内部メモリレイアウトは型に関わらず 1 要素 = 1 slot。

### リテラル
- 整数: `42`, `0x2a`, `052` (8 進), `0b101010`, `100000000000ULL`
- 浮動小数: `3.14`, `1e9`, `2.5f`, `1.0e-7`
- 文字: `'a'`, `'\n'`, `'\x7f'`, `'\042'`, `L'\0'` (encoding-prefix は無視)
- 文字列: `"hello\n"`, `"foo" "bar"` (concatenated_string)、`true`, `false`, `NULL`

### 変数 / スコープ
- ローカル変数 (関数スコープ)
- グローバル変数 (initializer 含む。 `int g = 5;`, `int a[5] = {1,2,3,4,5};`, `struct S g = {1,2};` など)
- ローカル配列 / 構造体 (連続 slot で確保、`{ ... }` initializer)
- designated initializer: `struct S s = {.b = 2, .a = 1};`
- string initializer: `char s[] = "abc";` → 4 slot に展開
- グローバル / ローカルの初期化は `(program GSIZE INIT_EXPR ...)` の INIT_EXPR で main 前に実行

### 演算子
- 算術: `+`, `-`, `*`, `/`, `%`, 単項 `+` `-`
- 比較: `<`, `<=`, `>`, `>=`, `==`, `!=`
- 論理: `&&`, `||`, `!` (短絡評価)
- ビット: `&`, `|`, `^`, `~`, `<<`, `>>`
- 代入: `=`, `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=`
- 後置/前置: `++`, `--` (識別子・添字・`*p`・`s.f`・`s->f` の各 lvalue 形式に対応)
- 三項: `cond ? a : b`
- カンマ: `(a, b)`
- ポインタ算術: `p+i`, `p-i`, `p1-p2`, `p++`, `*p`, `&x`
- 添字: `a[i]`, `(*p).x`, `p->x`
- キャスト: `(T)expr`, `(int*)p`
- `sizeof(type)`, `sizeof(expr)` (識別子は宣言型を尊重するので `sizeof(arr)` で配列バイト数が出る)

### 制御構造
- `if / else if / else`
- `while`
- `do { ... } while (cond);`
- `for (init; cond; upd) ...`
- `break`, `continue` — ループの body にあれば自動的に setjmp 付きノードへ振替 (`*_brk_only` / `*_brk`)
- `return` (値 / 値なし) — tail position は parse 時にリフトして setjmp 回避
- `switch / case / default` — fall-through 込み (matched フラグで lower)
- `goto label;` / `label:` — **関数のトップレベル seq にあるラベルのみ** ([todo.md](todo.md) 参照)
- `?:`

### 関数
- 関数定義 / 呼び出し (任意のアリティ)
- `void` 戻り型
- 関数ポインタ: `int (*op)(int, int) = func; op(1, 2);`
- 暗黙の関数ポインタ呼び出し (`op(...)` で `op` がローカル/グローバル変数なら間接呼び出し)
- `f(void)` 構文 (パラメータなし)
- 配列引数の decay: `int x[100]` パラメータは `int *x` として扱う (`sizeof(x)==8`)

### libc-ish ビルトイン

parse.rb 側で識別子をハードコードして対応する `node_call_*` を発行。

| 関数 | 実装ノード | 備考 |
|---|---|---|
| `printf(fmt, ...)` | `node_printf` | `%d/%i/%u/%x/%X/%o/%c/%s/%p/%f/%g/%e/%a` + flags + width + precision + length 修飾子 (`l/ll/z`) |
| `putchar(c)` | `node_putchar` | |
| `puts(s)` | `node_puts` | slot 配列 → 連続 byte に展開して書き出し |
| `malloc(n)` | `node_call_malloc` | n bytes (= ceil(n/8) slots) を calloc |
| `calloc(k, sz)` | `node_call_calloc` | |
| `free(p)` | `node_call_free` | |
| `strlen / strcmp / strncmp / strcpy / strncpy / strcat` | `node_call_*` | slot 配列前提 |
| `memset(p, v, n)` / `memcpy(d, s, n)` / `memmove` | `node_call_memset / memcpy` | |
| `atoi(s)` | `node_call_atoi` | |
| `exit(n)` / `abort()` / `_Exit(n)` | `node_call_exit` | |
| `abs / labs / llabs` | `node_call_abs` | |

文字列リテラル / 文字配列はすべて **VALUE-slot 配列** として表現される
(1 char = 1 slot in `.i`)。printf の `%s` 等は実行時に連続 byte に再構築する
(`castro_slot_to_cstring`)。

### preprocessor
- `gcc -E -P -x c <file>` を parse.rb 内で呼ぶ
- `#include <stdio.h>` / `#define` / `#if` / `#ifdef` / マクロ展開などすべて gcc に委譲
- `NO_CPP=1` 環境変数で skip 可

## 実行系 / 高速化

### 実行モード
1. **Plain interpreter** (`--no-compile`)
2. **AOT compile + run** (`--compile-all` / `-c`)
3. **AOT cached run** — 既存の `code_store/all.so` を dlopen して、各関数 body の dispatcher を SD\_<hash> に差し替えて実行

(JIT / PGC は今のところ wired up していない)

### 主な高速化

| 機構 | 効果 |
|---|---|
| **コンパイル時に呼び先決定** | parse.rb が各 `function_definition` に index を振り、`(call FUNC_IDX nargs arg_idx local_cnt)` を発行。runtime は `c->func_bodies[idx]` を直接引く。strcmp / 線形探索 / serial 検証 / inline cache 全廃 |
| **RESULT 値返し (abruby 方式)** | `return` / `break` / `continue` / `goto` を全て dispatcher 戻り値 (RESULT = VALUE + state) で伝播。setjmp / longjmp 全廃、ループの `*_brk` / `*_brk_only` 系も統一。`tail-return リフト` も不要に |
| **leaf-helper インライン化** (`node_call_static`) | 非自己再帰 callee は body NODE * を子オペランドで持つ `node_call_static` を使用。framework natural specializer が callee の SD chain を caller の TU に static inline で展開、gcc -O3 が inline。SCC 解析で recursive callee は旧 `:call` にダウングレード |
| **caller-allocated stack VLA frame** (wastro パターン) | 各 call boundary で `VALUE F[local_cnt]` を caller stack に確保し callee に渡す。SD chain がフル inline されると `&F` は escape しない → gcc SROA が F[i] を register に昇格。sieve の inner mark loop が 5 命令 → 3 命令 (gcc -O1 と同形) に。`SPECIALIZE` override で `local_cnt` を literal baked にして固定サイズ配列扱いさせるのが鍵 |
| **typed pointer load/store (TBAA)** | `node_load_*` / `node_store_*` を `int64_t *` / `double *` 経由に。`VALUE *` 経由だと strict aliasing で `fp[]` と alias 可能性ありと判定されてしまうのを回避。quicksort -27% |
| **AOT specialization** | ASTro framework が各 NODE に SD\_<hash> を生成、`-rdynamic` で host 側の helper を解決して dlopen |
| **`-rdynamic` ビルド** | code_store の SD が host 側ヘルパーを参照できる (これがないと AOT が黙って interp に fallback) |
| **cross-SD direct call** (`-Wl,-Bsymbolic`) | 自己再帰 / 関数間呼び出しで `extern SD_<callee_hash>` 直 call。GOT 経由 indirect を回避 |
| **AVX-vectorized array fill helper** | `for (i = S; i < E; i++) array[i] = K;` パターンを parse.rb peephole で `node_array_fill_i` に畳み、外部 helper `castro_fill_i` 経由で呼ぶ。-O3 単独 compile で gcc が自動 vectorize、AVX-256 stores |

### ベンチ結果 (gcc -O0/-O1/-O3 比較)

`bench.rb` は parse.rb の起動 (~110ms) を除いた純粋な実行時間で比較。
11 件、`BENCH_RUNS=10` median。

| bench              | interp | AOT first | AOT cached | gcc-O0 | gcc-O1 | gcc-O3 | castro / -O1 |
|--------------------|-------:|----------:|-----------:|-------:|-------:|-------:|---:|
| fib_big (fib 35)   |    870 |        65 |     **47** |     46 |     42 |     15 | 1.12× |
| fib_d              |     32 |        23 |      **4** |      3 |      2 |      1 | 2× |
| tak (18,12,6)      |      4 |        20 |      **2** |      0 |      0 |      0 | -- |
| ackermann (3,8)    |    175 |        34 |     **17** |      8 |      4 |      1 | 4× |
| loop_sum           |    260 |        22 |   🟢 **6** | 🔴  8  |      0 |      0 | -- |
| mandelbrot_count   |     26 |        21 |      **3** |      2 |      1 |      1 | 3× |
| sieve              |    194 |        24 |     **11** |      7 |      3 |      2 | 3.7× |
| nqueens            |    640 |        36 |  🟢 **17** | 🔴 29  |     15 |     12 | 1.13× |
| quicksort          |   3170 |        72 |  🟢 **56** | 🔴 80  |     20 |     20 | 2.8× |
| crc32              |    790 |        31 |     **20** |     46 |🟰 18   |     18 | **1.11×** |
| matmul             |     61 |        21 |      **3** |      2 |      1 |      0 | 3× |

**castro が gcc -O0 を上回るベンチ** (3件): `loop_sum`, `nqueens`, `quicksort`
**castro が gcc -O1 にほぼタイ** (1件): `crc32` (20 vs 18ms)
**残ギャップが構造的なケース**: `sieve` — inner mark loop は disasm 上 gcc -O1 と完全同形だが、AST 評価器が gcc の 3× 多い memory operation を発行する構造的差異 (詳細 [perf.md §13](perf.md#13))

採用した個別最適化と、その効果の内訳は [perf.md](perf.md) 参照。

主要マイルストーン:
- 開始時 fib_big AOT cached = 1442 ms
- tail-return リフト → 803 ms (1.8×)
- `-rdynamic` 投入で AOT が実際に効く → 217 ms (6.6×)
- call IR を idx 化 (callcache 廃止) → 64 ms (22.5×)
- `VALUE *fp` を common_param 化 + `call`/`call_jmp` の parse 時振り分け
  + cross-SD direct call (`-Wl,-Bsymbolic`) → **48 ms (30×)**

## テスト

### feature tests (`make test`)

`tests/test_*.c` を interp / AOT first / AOT cached の 3 モード × stdout + exit
code 一致でチェック。各テストにつき 3 アサーションなので 30 件 = 10 ファイル。

| ファイル | 範囲 |
|---|---|
| test_arrays.c | 配列宣言、添字 read/write、関数引数の decay、`strlen` |
| test_break.c | break / continue / nested loop のスコープ |
| test_funcptr.c | `int (*op)(int,int)` パラメータ + `apply(add, ...)` + ローカル関数ポインタ |
| test_globals.c | `int g_counter`, `int g_arr[5] = {...}`, `double g_pi`, 書き換え |
| test_goto.c | label/goto の forward + backward (トップレベルのみ) |
| test_pointers.c | `&x`, `*p`, `*p += v`, ポインタ算術, ポインタ差 |
| test_printf.c | flags / width / precision / `%lld` / `%x` / `%g` / `%%` / putchar / puts |
| test_returns.c | tail return リフトと、loop 内 return (setjmp 経路) |
| test_structs.c | フィールド read/write / 関数引数 / `s->f` / 初期化子 |
| test_switch.c | basic / `default` 中間 / fall-through |

### c-testsuite (`make test-cts`)

[c-testsuite/c-testsuite](https://github.com/c-testsuite/c-testsuite) を `testsuite/c-testsuite/single-exec/` に展開して実行。**184/220 passed (83.6%)**。
残り 47 件の落ち穂は [todo.md](todo.md) 参照。

### bench (`make bench`)

`bench.rb` (BENCH_RUNS=5)。
gcc -O0..-O3 のリファレンスバイナリと比較するテーブルを出す。
