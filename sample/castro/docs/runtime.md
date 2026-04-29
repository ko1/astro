# castro ランタイム構造

castro は ASTro フレームワーク上の C サブセットインタプリタ。tree-sitter-c で
パースした C ソースを、parse.rb が型解決済みの S 式 IR に下ろし、castro 本体
（C）が ASTro ノード木に組み立ててから実行する。本書ではノード評価器 / 値
レイアウト / 関数フレーム / 主要な高速化機構の対応を整理する。

```
*.c                      ファイル
  │ gcc -E    (parse.rb 内、NO_CPP=1 で skip 可)
  ▼
preprocessed C
  │ tree-sitter-c (parse.rb)
  ▼
typed S-expression IR  (例: (program GSIZE INIT_EXPR (func name P L T HR BODY) ...))
  │ main.c  load_program / build_expr (SX → ALLOC_node_xxx)
  ▼
NODE 木  (head + union u)
  │ EVAL(c, node) = (*n->head.dispatcher)(c, n)
  ▼
VALUE  (8-byte tagged union)
```

## 全体像

```
CTX                          (castro_invoke / EVAL の第 1 引数)
  env  ───► env_end           ローカル/引数 VALUE スロット領域
                              (CASTRO_ENV_SLOTS = 1<<20 = 8 MiB)
  fp   ───►                   現在のフレームポインタ
  globals[globals_size]       グローバル変数 (parse.rb で slot 番号を割当)
  func_set[NFUNCS]            関数テーブル ({name, body, params, locals, needs_setjmp})
                              parse.rb が振った index で直接引く (固定容量、realloc しない)
  return_buf                  関数 return 用 jmp_buf (early return ありの関数のみ)
  break_buf / continue_buf    ループ脱出 / 継続用 jmp_buf
  goto_buf  / goto_target     goto 用 jmp_buf + ラベル番号
```

## VALUE 表現

すべての値は 8-byte 共用体 1 つ。**型情報はノード側が持つ** ので、共用体には
タグを入れない。

```c
typedef union VALUE {
    int64_t  i;   // int 系 (char/short/int/long/long long/unsigned/enum)
    double   d;   // 浮動小数点 (float/double/long double)
    void    *p;   // ポインタ・配列ヘッド・関数アドレス
} VALUE;
```

* `int x` / `int a[10]` のいずれも 1 要素 = 1 VALUE スロット (= 8 byte)。
  (本物の C ABI では `int[10]` = 40 byte だが、castro 内部では 80 byte。
   `sizeof(int[10])` は 40 を返す — host C と互換)
* 文字列リテラル `"abc"` は 4 個の VALUE スロットに展開される
  (`'a' 'b' 'c' '\0'` の `.i`)。printf の `%s` / puts は実行時に連続 byte 列に
  リカバリして host libc に渡す (`castro_slot_to_cstring`)。
* ポインタ算術はすべて **slot 単位**。`p + 1` = +8 byte。`int *p` でも `char *p`
  でも同じ。ポインタ型 → byte 数の変換は parse.rb 側で吸収しない。

## 型システム (parse.rb 側のみ)

```ruby
class CType
  kind: :prim | :ptr | :array | :struct
  name / inner / size / fields
end
```

`slot_count` はストレージ確保量、`byte_size` は host C 互換の sizeof 結果を
返す。`decay` で `int[10]` → `int*` (関数引数は宣言時に decay)。

## ノード一覧 (96 nodes)

### リテラル
| node | 引数 |
|---|---|
| `node_lit_i` | `int32_t v` |
| `node_lit_i64` | `uint64_t v` |
| `node_lit_d` | `double v` |
| `node_lit_str` | `const char *v` (生 byte 列、現在は未使用) |
| `node_lit_str_array` | `const char *v` (slot 配列に展開) |

### 変数
| node | 引数 | 説明 |
|---|---|---|
| `node_lget` / `node_lset` | `uint32_t idx [, NODE *rhs]` | ローカル read/write |
| `node_gget` / `node_gset` | `uint32_t idx [, NODE *rhs]` | グローバル read/write |
| `node_addr_local` | `uint32_t idx` | `&fp[idx]` |
| `node_addr_global` | `uint32_t idx` | `&globals[idx]` |

### ポインタ
| node | 引数 | 説明 |
|---|---|---|
| `node_load_i / load_d / load_p` | `NODE *p` | `*(VALUE*)p` の `.i` / `.d` / `.p` |
| `node_store_i / store_d / store_p` | `NODE *p, NODE *v` | `*(VALUE*)p = v` |
| `node_ptr_add` | `NODE *p, NODE *off` | `(VALUE*)p + off` |
| `node_ptr_sub_i` | `NODE *p, NODE *off` | `(VALUE*)p - off` |
| `node_ptr_diff` | `NODE *a, NODE *b` | `(a - b)` (slot 単位) |

### 制御構造
| node | 説明 |
|---|---|
| `node_seq` | 2 文の連結。tail 値を返す |
| `node_nop` / `node_drop` | 無操作 / 値を捨てる |
| `node_if` / `node_ternary` | 条件分岐 |
| `node_while` / `node_do_while` / `node_for` | 高速パス: break/continue 無しのループ |
| `node_while_brk_only` / `_for_brk_only` / `_do_while_brk_only` | break のみ — 1 回 setjmp |
| `node_while_brk` / `_for_brk` / `_do_while_brk` | break + continue — 反復毎 setjmp (continue 用) |
| `node_break` / `node_continue` | longjmp で囲みループへ |
| `node_return` / `node_return_void` | longjmp で関数境界へ (early return がある関数のみ) |

### 算術・比較
`node_add_i / sub_i / mul_i / div_i / mod_i / neg_i / band / bor / bxor / bnot / shl / shr` (整数)、`node_add_d / sub_d / mul_d / div_d / neg_d` (浮動小数)、`node_lt_x / le_x / gt_x / ge_x / eq_x / neq_x` (`x ∈ {i,d}`)、`node_land / lor / lnot`、`node_cast_id` (int→double) / `node_cast_di` (double→int)。

### 関数呼び出し
| node | 引数 | 説明 |
|---|---|---|
| `node_call` | `func_idx, arg_count, arg_index` | 直接呼び出し (idx は parse 時解決) |
| `node_call_indirect` | `NODE *fn_expr, arg_count, arg_index` | 関数ポインタ経由 |
| `node_func_addr` | `func_idx` | `&c->func_set[idx]` を返す |
| `node_printf` `@noinline` / `node_putchar` / `node_puts` | | 標準出力 |
| `node_call_malloc / calloc / free` | | ヒープ |
| `node_call_strlen / strcmp / strncmp / strcpy / strncpy / strcat` | | 文字列 (slot 配列) |
| `node_call_memset / memcpy` | | メモリ |
| `node_call_atoi / abs / exit` | | その他 libc |

(以前は `node_def` を使って実行時に関数を登録 / `struct callcache` /
`struct func_addr_cache` の inline cache でルックアップを償却していたが、
C は parse 時に呼び先がわかるので全部削除した。)

### goto
| node | 説明 |
|---|---|
| `node_goto_dispatch` | 関数本体を while(1)+switch(label) で囲む |
| `node_goto label` | `c->goto_target = label; longjmp(*c->goto_buf)` |
| `node_goto_target` | `c->goto_target` を読む (case 判定用) |

> **制限**: 現実装は **関数のトップレベル seq 上にあるラベルしか取り扱えない**。for/while/if/switch の中にラベルがあると、parse.rb の `flatten_seq` が AST を平坦化できず、`_label_marker` が IR のまま残って実行時エラーになる。詳細は [todo.md](todo.md) §goto。

## 関数フレーム

VALUE スロットの一直線スタック。フレーム間のメタデータは持たない。

```
caller の locals/args  →  fp[arg_index..arg_index+N-1] に args を lset
                            ↓ castro_invoke (fp += arg_index)
callee の locals       →  fp[0..N+L-1]
                            ↓ castro_invoke リターン (fp -= arg_index)
caller に戻る
```

* **fp の移動だけでフレーム push/pop が完了する** (return address や frame
  link は不要 — ホスト C のスタック側に残す)
* 引数も locals と同じ index 空間。callee は呼び出し時点の `caller fp + arg_index` を自分の `fp[0]` として参照する
* 配列 / 構造体は連続スロットとして locals 上に居る (例えば `struct point a` は `fp[idx]` と `fp[idx+1]`)

### return の二段構え

parse.rb は関数本体に対して **tail-return lifting** を行う:

```
seq( if(c, return e1, nop), return e2 )
   ↓ 「seq の if-then が return なら、else に rest を畳み込む」
if(c, e1, e2)              ; return が消えた!
```

リフトで全部消えたら関数の `has_returns = 0`。castro 側は setjmp なしで body
を直接 EVAL する。残った場合 (loop 内 return など) は setjmp/longjmp。
fib / tak / ackermann / ループ系はリフトで消えるので 0-cost。

### break / continue

ループ内で break/continue を使うかは parse.rb が走査して判定し、4 種類の
ループノードに振り分ける:

| 構造 | break あり | continue あり | 採用ノード | 設置 setjmp |
|---|:-:|:-:|---|---|
| なし | × | × | `node_for` 等       | 0 |
| break のみ | ○ | × | `node_for_brk_only` | 1 (ループ入口) |
| continue のみ | × | ○ | `node_for_brk` | 反復毎 |
| 両方 | ○ | ○ | `node_for_brk` | 反復毎 |

普通のループ (`while (i < n) sum += i;` など) は setjmp 0 個で回る。

### goto

`uses_goto = true` の関数は、parse.rb が **関数全体を label-dispatch ループに
書き換えてから** `node_goto_dispatch` で包む。

```
while (1) {
  switch (goto_target) {
    case 0: <ラベル前>; goto_target = 1; break;
    case 1: <label1 後>; goto_target = 2; break;
    ...
    case END: break out;
  }
}
```

`node_goto label` は `goto_target = label; longjmp` で while ループの先頭に
戻る。`break` で `node_while_brk_only` を脱出して関数を抜ける。

## 関数呼び出し: parse 時に index 解決

C は呼び先が静的にわかる言語なので、IR レベルで **関数 index を直接持つ**:

```
(call FUNC_IDX nargs arg_index)
(func_addr FUNC_IDX)
```

parse.rb の `gather_signatures` が各 `function_definition` に
0..NFUNCS-1 の index を割り当て、runtime は

```c
struct function_entry *fe = &c->func_set[func_idx];
NODE *body = fe->body;
if (UNLIKELY(fe->needs_setjmp)) return castro_invoke_jmp(c, body, body->head.dispatcher, arg_index);
c->fp += arg_index;
VALUE v = (*body->head.dispatcher)(c, body);
c->fp -= arg_index;
```

で呼ぶ。strcmp も線形探索も inline cache の hit/miss も登場しない。

`c->func_set[]` は SX header (`(program GSIZE NFUNCS ...)`) で読んだ
NFUNCS の固定容量で 1 度だけ確保し、realloc しない。だから
`&c->func_set[idx]` は load_program 中ずっと安定。

### 2-pass load

呼び先が定義より前に呼び出される (前方参照、再帰など) のを許すため、
load_program は SX を 2 段階で読む:

1. **Phase 1 — signatures**: `(sig NAME P L T HR)` を NFUNCS 個読み、
   `castro_register_stub` が body=NULL の状態で `func_set[i]` を埋める。
2. **Phase 2 — bodies**: bodies を順番に読み (Phase 1 と同じ順序)、
   `func_set[i].body` をパース結果で埋める。

Phase 2 中に `(call IDX ...)` が出てきても、Phase 1 で stub が登録済み
なので body が NULL のうちは ALLOC 時には参照されない (ALLOC は idx を
そのまま保存するだけ)。実行は Phase 2 完了後に始まるので body は揃っている。

## ASTro Code Store 連携

ASTro framework 共通の `runtime/astro_code_store.{c,h}` を使う:

| フェーズ | 動作 |
|---|---|
| `astro_cs_compile(body)` | body の SD\_<hash>.c を生成 (specializer が DUMP) |
| `astro_cs_build()` | code_store/Makefile で `gcc -O3 -fPIC -fno-plt -march=native` |
| `astro_cs_reload()` | all.<N>.so を hardlink で生成 → dlopen |
| `astro_cs_load(node)` | `dlsym(SD_<hash(node)>)` で見つかれば node->dispatcher を入れ替える |

**重要**: castro 本体は `-rdynamic` でビルドされている。これを忘れると
SD 側の `castro_invoke_jmp` などの参照が dlopen で解決できず、
`astro_cs_reload` が黙って NULL を返してずっと interp モードで動く。

## ノード hash と castro 拡張

castro 側の唯一の特殊化: `uint64_t` リテラル。framework 既定の
`(VALUE)NN` 表現が castro の union 型ではコンパイル不可なので、
`castro_gen.rb` でオーバーライドして素のリテラル (`NNULL`) を出している。

```ruby
when 'uint64_t'
  arg = "    fprintf(fp, \"        %lluULL\", ...);"
```

(以前は `struct callcache *` / `struct func_addr_cache *` の inline cache
operand 用に hash/dump/specialize 全部を上書きしていたが、IR が parse 時
解決の index 方式になったので不要になった。)

## CASTRO_ENV_SLOTS とスタックサイズ

CTX の `env` は `CASTRO_ENV_SLOTS = 1<<20` 個の VALUE 領域 (= 8 MiB)。fib(35)
クラスの再帰でも 35 段 × 数スロットなので余裕。深い再帰で溢れる場合は main.c
のマクロを増やす。

## ファイル構成

```
sample/castro/
├── README.md             プロジェクト概要 + ベンチ結果
├── CLAUDE.md             (なし)
├── Makefile              ASTroGen 呼び出し + gcc -rdynamic
├── castro_gen.rb         CastroNodeDef (uint64_t literal specializer 上書きのみ)
├── parse.rb              tree-sitter-c → S 式 IR (CType / 型推論 / lift)
├── main.c                CTX 生成 / SX パーサ / 関数テーブル / printf 実装
├── node.c                generated includes + INIT/OPTIMIZE
├── node.def              全ノードの EVAL
├── node.h / context.h    型 + マクロ
├── docs/
│   ├── runtime.md        ← この文書
│   ├── done.md           実装済み機能インベントリ
│   └── todo.md           未対応 / 既知の制限
├── examples/             ベンチ用 .c (fib_big / tak / mandelbrot_count / ...)
├── tests/                feature tests + run.sh
├── testsuite/            c-testsuite (.gitignore — 自分で取得)
├── parsers/c.so          tree-sitter-c grammar (.gitignore)
└── code_store/           SD\_*.c / all.so (.gitignore)
```
