# castro 言語仕様

`castro` は **C 言語のサブセット**インタプリタ。tree-sitter-c でパースし
型を解決した上で AST に落とし、ASTro のツリーウォーカで実行する。
プリプロセッサは host の `gcc -E` を呼んで通すので、`#include <stdio.h>` 等
普通に書ける。

C 仕様 (C99/C11) は膨大なので、本書は **castro で動くサブセット** を端的に
示す。完全な C 仕様は ISO/IEC 9899 を参照。

## 値とメモリモデル

castro の `VALUE` は **8 byte の union**:

```c
union { int64_t i; double d; void *p; }
```

すべてのデータ — `int` / `char` / `double` / ポインタ / 配列要素 /
構造体フィールド — が **1 要素 = 1 slot (8 byte)** で配置される。
ポインタ算術も slot 単位 (`p+1` は 8 byte 進む)。

`sizeof(T)` は host C と互換のバイト数を返すが、メモリレイアウト自体は
slot 単位なので、struct の field offset は host C ABI と同じではない。
(これにより castro 内同士の I/O は問題なく、`printf("%s", s)` のような
host libc 呼び出しでは内部的にバイト列に再構築する。)

## 型

| 型 | 内部表現 | 備考 |
|---|---|---|
| `int` / `long` / `long long` | `int64_t` | 全部 64bit に揃える |
| `short` / `char` | `int64_t` (sign-extended) | |
| `unsigned int` 等 | `int64_t` (符号なし扱い) | |
| `float` / `double` | `double` | float も double 精度に昇格 |
| `T *` | `void *` | 任意のポインタ |
| `T[N]` | 連続 N slot | 配列 |
| `struct { ... }` | 連続 slot | フィールドごとに 1 slot |
| `enum { ... }` | `int64_t` | 整数定数 |
| `void` | (戻り値専用) | |

## リテラル

```c
42                  // int
0xff  0b1010  077   // 16/2/8 進
3.14  1e10  0.5f    // double (f サフィックスは無視)
'a'  '\n'  '\\'     // 文字定数
"hello"             // 文字列リテラル → slot 列に展開
```

## 演算子

おおむね C と同じ:

| カテゴリ | 演算子 |
|---|---|
| 算術 | `+ - * / %` (int / double 別ノードに parse 時分裂) |
| 比較 | `< > <= >= == !=` |
| 論理 | `&& \|\| !` (短絡評価) |
| ビット | `& \| ^ ~ << >>` |
| 代入 | `= += -= *= /= %= &= \|= ^= <<= >>=` |
| インクリ | `++ --` (前置・後置) |
| 三項 | `? :` |
| ポインタ | `* & -> .` |
| 添字 | `a[i]` |
| キャスト | `(int)x` `(double)y` |
| sizeof | `sizeof(T)` `sizeof(expr)` |

## 制御構文

```c
if (cond) stmt              // else 省略可
if (cond) stmt else stmt
while (cond) stmt
do stmt while (cond);
for (init; cond; step) stmt
switch (e) {
    case 1: ... break;
    case 2: case 3: ...; break;
    default: ...;
}
break;     // 直近のループ / switch から抜ける
continue;  // ループの次の反復へ
return expr;
return;    // void 関数
```

`goto` はサポートするが、**関数のトップレベル seq にあるラベルのみ** 対応。
`for`/`while`/`if`/`switch` の中のラベルへ飛ぶケースは未対応。
([`todo.md`](todo.md) 参照)

## 関数定義

```c
int add(int a, int b) {
    return a + b;
}

void print_n_times(const char *s, int n) {
    for (int i = 0; i < n; i++) {
        printf("%s\n", s);
    }
}

int main(int argc, char *argv[]) {
    print_n_times("hi", 3);
    return 0;
}
```

- 戻り値なしは `void`。
- 可変長引数 (`...`) は `printf` のような builtin に限る。ユーザ定義 `va_list` は未対応。
- 関数ポインタも使える: `int (*op)(int, int) = add;`、`(*op)(1, 2)`。

## 変数

```c
int x = 10;             // ローカル
static int counter = 0; // ファイルスコープ静的 (常駐)
extern int g;           // 外部参照 (castro 内では同じ翻訳単位扱い)

int g = 42;             // グローバル initializer

const int MAX = 100;
const char *name = "alice";
```

トップレベルでも `int`/`double`/配列/構造体の初期化子を書ける:

```c
int primes[] = { 2, 3, 5, 7, 11 };
struct point { int x; int y; } origin = { 0, 0 };
```

## ポインタ・配列

```c
int a[5] = {1, 2, 3, 4, 5};
int *p = a;            // 配列名は先頭要素のポインタに decay
int *q = &a[2];        // = a + 2
int diff = q - p;      // 2 (slot 単位)

*p = 10;               // dereference
p[1] = 20;             // = *(p+1) = 20
p++;                   // p を 1 slot 進める
```

ポインタ算術は **slot 単位** (`p + 1` = 8 byte 進む)。host C ABI とは違うので、
バイト数を期待するコード (`(char *)p + 1` で 1 byte 進める等) は注意。

## 構造体・union・enum

```c
struct point {
    int x;
    int y;
};

struct point p1 = { 1, 2 };
struct point p2 = { .x = 3, .y = 4 };       // designated initializer
p1.x = 10;
struct point *pp = &p1;
pp->y = 20;

typedef struct { int r, g, b; } color;
color red = { 255, 0, 0 };

enum { RED, GREEN, BLUE };       // RED=0, GREEN=1, BLUE=2
enum direction { N=1, E, S, W };  // N=1, E=2, ...
```

## 文字列

```c
const char *s = "hello";
printf("%s, length=%zu\n", s, strlen(s));

char buf[100];
strcpy(buf, "world");
strcat(buf, "!");
```

文字列リテラルは slot 列に展開される。`printf`/`puts`/`strlen` 等の libc
関数は内部でバイト列に再構築して host libc に渡す。

## プリプロセッサ

`gcc -E` を呼んで通すので、以下が利用可能:

```c
#include <stdio.h>
#include "myheader.h"

#define MAX 100
#define SQR(x) ((x) * (x))

#ifdef DEBUG
    fprintf(stderr, "x = %d\n", x);
#endif
```

`NO_CPP=1` でプリプロセッサを無効化できる。

## サポートする libc 関数

I/O: `printf` / `fprintf` (stderr/stdout) / `putchar` / `puts` / `getchar`

文字列: `strlen` / `strcmp` / `strncmp` / `strcpy` / `strncpy` / `strcat` /
`strchr` / `memset` / `memcpy` / `memcmp`

メモリ: `malloc` / `free` / `calloc` / `realloc`

その他: `atoi` / `exit` / `abs` / `srand` / `rand` / `time`

## 例

```c
#include <stdio.h>

int fib(int n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

int main(void) {
    for (int i = 0; i < 10; i++) {
        printf("fib(%d) = %d\n", i, fib(i));
    }
    return 0;
}
```

```c
#include <stdio.h>
#include <stdlib.h>

struct list {
    int val;
    struct list *next;
};

struct list *cons(int v, struct list *t) {
    struct list *n = malloc(sizeof(*n));
    n->val = v;
    n->next = t;
    return n;
}

int main(void) {
    struct list *l = cons(1, cons(2, cons(3, NULL)));
    int sum = 0;
    for (struct list *p = l; p; p = p->next) sum += p->val;
    printf("sum = %d\n", sum);
    return 0;
}
```

## 持たない / 制限

- 構造化されていない `goto` (関数トップレベル seq のラベルのみ)
- ユーザ定義可変長引数 (`va_list` / `va_start` / `va_arg`)
- ビットフィールド (`unsigned int x : 5`)
- `volatile` / `restrict` の本格的サポート (構文受理のみ)
- `_Atomic` / C11 atomics
- 関数ポインタの caller-side ABI 互換 (host C と直接やりとりはしない)
- インラインアセンブラ
- 完全なフォーマット指定子 (`printf` の `%n` 等は未対応)
- `setjmp` / `longjmp`

詳細: [`done.md`](done.md) / [`todo.md`](todo.md)。
