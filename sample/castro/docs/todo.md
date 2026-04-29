# castro 未対応 / 既知の制限

実装済み機能は [done.md](done.md) を参照。

## 大物 (構造的制約があるもの)

### goto: ネスト構造の中のラベル ⚠

現状の `lower_goto` は parse.rb の `flatten_seq` で **関数のトップレベル seq
だけを平坦化** して label-dispatch ループに変換する。そのため:

✅ 動く例 (ラベルがトップレベル seq 上):
```c
int main() {
    int i = 0;
    goto start;
again:
    i++;
start:
    if (i < 5) goto again;
    return i;
}
```

❌ 動かない例 (ラベルが for/while/if/switch の内側):
```c
int main() {
    for (int i = 0; i < 5; i++) {
        if (i == 3) {
inside:                     // ← if の中にあるので、parse.rb から見えない
            return i * 10;
        }
        if (i == 1) goto inside;
    }
    return -1;
}
// 実行時: "unknown op: _label_marker"
```

#### なぜ難しい

C の goto は「ループの途中に飛び込む」「ループの初期化や条件チェックを
飛び越す」が許されている。これは構造化制御フロー (`while`/`for`/`if` の
ネスト) では一般には表現できない (Bohm-Jacopini 関連の古典的結果。補助変数
を入れたり、コードを膨張させなければ等価な表現が得られない)。

#### 改善案 (実装するなら)

`uses_goto` の関数だけ、parse.rb に **構造化解除パス** を入れる:

| 元の構造 | 下ろし先 |
|---|---|
| `for (init; cond; upd) body` | `init; goto _LT; _LT: if (!cond) goto _LE; body; upd; goto _LT; _LE:` |
| `while (cond) body` | `_LT: if (!cond) goto _LE; body; goto _LT; _LE:` |
| `do body while (cond)` | `_LT: body; if (cond) goto _LT;` |
| `if (cond) t else e` | `if (!cond) goto _ELSE; t; goto _END; _ELSE: e; _END:` |
| `break` | `goto _LE` (ネストの一番内側のループ脱出ラベル) |
| `continue` | `goto _LT` (一番内側のループ条件ラベル) |

ここまで下ろせば全部 label/conditional-goto/seq だけになり、現行の
flatten + dispatch loop で動く。コスト:

- パス自体の実装は中程度 (~300 行程度)
- ループ内に label を持つ関数は dispatch loop を 1 回経由するので、speculation
  が効きにくくなり多少遅くなる。`uses_goto = false` の関数 (= 大半) は無コスト

c-testsuite だと `00010` (forward goto into for body) / `00204` / `00213` が
これで救える見込み。

### 他の "loop に変換できない" 系

- `setjmp` / `longjmp` 自体 (ユーザコード側): 一切非対応
- ネストした `switch` での外側 break: 現行は内側 break で外側にも戻ってしまう (matched フラグの設計で結合してしまっている。`switch` 専用ラベルが必要)

## 言語機能

### union
無対応。`struct { int x; union { int y; int z; }; }` のような無名 union や
正規の `union { ... } u;` パターンが c-testsuite で 5 件ほど落ちている (`00046`, `00050`, `00064`, …)

### bit-field
無対応。`struct { int a:4; int b:12; };` 等。

### ユーザ関数の variadic (`...`)
無対応。`int f(int n, ...) { va_list ap; ... }` は parse 時点でアウト。
printf だけは parse.rb の特例で動く。

### 関数を返す関数の宣言子
`int (*foo(int))(int);` のような関数を返す関数。tree-sitter 自体は parse する
が、castro の declarator decoder で対応していない。

### 関数ポインタ配列
`int (*tab[N])(int);`。 declarator のパスで `array_declarator` の中の
`pointer_declarator` の中の `function_declarator` のように、複合した宣言子の
正規化が未実装。

### 多次元配列の "本物の" レイアウト
`int m[3][4]` は現状 12 個の VALUE slot として扱われ、`m[1][2]` は
`*((m+1*4)+2)` として展開できる... が、parse.rb 側で多次元 subscript の対応が
入っていないので外側だけポインタを取って内側はうまく動かない。

### `static` / `extern` 修飾の意味
語彙的にスキップしている。複数翻訳単位 (TU) に分かれたソースは想定外。
`static` 関数が module-private にならず公開関数と同じ扱いになる。

### `_Generic` (C11)
無対応。c-testsuite の 1 件 (`00216`) が落ちている。

### `__builtin_*` 系
`__builtin_bswap16` 等。これは個別に追加すれば動くが現状ハードコードしていない。

### `restrict` / `_Atomic` / `_Alignas` / `_Noreturn` / `inline`
type-spec / declarator から **テキストとして** 取り除いているだけで、意味論は
無視。`inline` 関数は普通の関数として処理。`_Atomic int` も普通の int。

### `wchar_t` / `wint_t` / wide string `L"..."`
encoding-prefix `L` / `u` / `U` / `u8` は char 寄りだけ受理して、widechar 解釈は
していない (`L'\0'` は `'\0'` と同じ)。

### 浮動小数細部
- `float` / `double` / `long double` をすべて `double` で実行 — 精度低下が
  起きるテストが 2 件ある
- `inf` / `nan` のリテラルは `INFINITY` / `NAN` (math.h) 経由で動くが、
  hex-float (`0x1.fp+3`) はテスト未確認

## 内部表現の制約

### 1 要素 = 1 VALUE slot

すべてのデータ (int / char / double / pointer) を 8-byte slot 1 個で表現
しているので:

- ✅ host との sizeof 互換 (parse.rb 側で host C と同じ `sizeof(int) == 4` を
  返している)
- ❌ `read(fd, buf, n)` 等で OS API に直接 buffer を渡すと壊れる (1 byte
  単位ではなく 8 byte 単位の格納のため)
- ❌ memcpy で構造体ごとコピーするケース、構造体パディング、構造体の中での
  `char` 配列など、ABI 互換が問われる場面で不一致

#### バイト寄せレイアウトに切り替えるなら

VALUE slot を放棄して、すべてを `char fp[]` に直接配置する。各読み書きは
型に応じたサイズ (`*(int*)&fp[off]`)。これは castro の根本的な再設計に
近く、現状の interp / AOT specializer の効率と引き換え。

## ランタイム / 高速化

### JIT モード未対応
ASTro framework としては JIT があるが、castro は AOT (`--compile-all`) の
タイミングで一気にビルドして dlopen するだけ。実行時動的特殊化はしていない。

### Profile-Guided Compilation (PGC) 未対応
hopt_index / `PGSD_<hopt>` は ASTro framework 側にあるが castro は使っていない。
hot 関数だけを再特殊化、のような最適化は未実装。

### 関数ポインタ呼び出しの間接ジャンプ
`node_call_indirect` は `function_entry *` (call_indirect の引数式から
得られる) → `fe->body->head.dispatcher` の 3 段ロード + indirect call。
直接呼び出し (`node_call`) と異なり、ポインタが実行時に変わりうるので
inline cache を足す余地あり。今のところ c-testsuite のホットパスでは
未観測。

### 大量の関数登録
`castro_register_stub` は load_program で NFUNCS 回呼ばれるだけで
線形探索はしない (parse.rb 側で振った index に直接書き込む)。strdup
だけ残るが load 1 回限りなので問題にならない。仮に数万関数規模に
なれば parse.rb 側の `GLOBAL_FUNCS` ハッシュテーブル化が先。

## c-testsuite 残り failure (47 件)

| 分類 | 件数 | 主な例 |
|---|:-:|---|
| union / 無名 union | 7 | 00046, 00050, 00064 |
| `initializer_list` のネスト深 | 4 | 00091, 00147, 00148 |
| `initializer_pair` (designated 関連の細部) | 3 | 00092, 00149 |
| 関数を返す関数 / 関数ポインタ配列 | 2 | 00124, 00140 |
| variadic ユーザ関数 | 2 | 00100, 00141 |
| `_Generic` | 1 | 00216 |
| ネスト goto | 3 | 00010, 00204, 00213 |
| typedef + recursive struct | 1 | 00197 |
| `__builtin_*` | 3 | 00040 (`__builtin_bswap16`) |
| `sprintf` / `fprintf` 系 | 1 | 00170 |
| stdout 微差 (浮動小数表示) | 3 | 00154, 00170, 00211 |
| 残り (個別) | 17 | 00077 (sizeof と decay), 00115, 00136, 00179 (fancy strchr), ... |

ざっくり「union + ネスト initializer + structured-goto」を片付ければ +25
件くらい救える見込み。
