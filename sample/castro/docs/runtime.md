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

C は呼び先が静的にわかる言語なので、IR レベルで **関数 index または body
NODE * を直接持つ** 2 つの形がある:

```
(call FUNC_IDX nargs arg_index)         ; 自己/相互再帰用
(call_static FUNC_IDX nargs arg_index)  ; 非再帰用 (parse 後 body を patch)
(func_addr FUNC_IDX)
```

parse.rb の `gather_signatures` が各 `function_definition` に
0..NFUNCS-1 の index を割り当てる。compile_call は optimistic に全
call を `:call_static` で emit、後段で call graph を Tarjan SCC で
解析、再帰 SCC に属する callee への `:call_static` を `:call` に
ダウングレードする。

### `:call` (再帰用) の runtime

```c
NODE *body = c->func_bodies[func_idx];
RESULT r = (*body->head.dispatcher)(c, body, fp + arg_index);
return RESULT_OK(r.value);  // RETURN は callee 内で完結、捨てる
```

SPECIALIZE は `extern RESULT SD_<callee_hash>(...)` 宣言と直接呼びを
emit、`-Wl,-Bsymbolic` で intra-`.so` 解決 → `addr32 call SD_<self>`
(BTB-perfect)。

### `:call_static` (非再帰用) の runtime

callee body を `NODE *callee` 子オペランドとして AST に持つ。framework
の natural specializer が子オペランドとして walk → callee body の SD
chain を caller の同 TU に `static inline` で展開 → gcc -O3 が inline。

`callee` は phase-2 SX load 時には NULL (forward reference 対応)、
phase-3 で `c->func_bodies[idx]` の値を書き戻す。

```c
node_call_static(CTX *c, NODE *n, VALUE *fp, NODE *callee, ...) {
    fp = fp + arg_index;
    RESULT r = EVAL_ARG(c, callee);   // framework が callee_disp を baked
    return RESULT_OK(r.value);
}
```

`c->func_bodies[]` は SX header (`(program GSIZE NFUNCS ...)`) で読んだ
NFUNCS の固定容量で 1 度だけ確保し、realloc しない。だから
`c->func_bodies[idx]` も `:call_static` の patched callee も load_program
中ずっと安定。

### 3-pass load

呼び先が定義より前に呼び出される (前方参照、再帰など) のを許すため、
`load_program` は SX を 3 段階で読む:

1. **Phase 1 — names**: `(sig NAME)` を NFUNCS 個読み、`c->func_names[i]`
   を埋める (dump 用と main 検索用)。`c->func_bodies[i]` はこの時点で
   NULL。
2. **Phase 2 — bodies**: bodies を順番に読み、`c->func_bodies[i]` を
   パース結果で埋める。`(call IDX ...)` は idx をそのまま保存するだけ
   なので、callee body が未 build でも問題なし。`(call_static IDX ...)`
   も callee=NULL の placeholder で ALLOC、`(NODE, idx)` を call_patch
   テーブルに記録。
3. **Phase 3 — call_patch**: 全 body が揃った状態で call_patch テーブル
   を walk し、各 `node_call_static.callee` に `c->func_bodies[idx]` を
   書き戻す。これで AST が tree から DAG に (recursive 関数を含む場合
   は循環参照がある DAG)。HASH / SPECIALIZE は両方 cycle break 経路を
   持つので循環があっても well-defined。

実行は Phase 3 完了後に始まるので body / callee 共に揃っている。

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

### `parsing_phase` ガード — OPTIMIZE per-ALLOC を抑止

ASTroGen が ALLOC\_<kind> 内に自動挿入する `OPTIMIZE(_n)` フックは、
新しく作られた NODE 1 つずつに対し `astro_cs_load` を呼んで SD を
当てに行く。これが parse 中の `node_call_static` (callee=NULL) に
当たると **未 patch の callee で hash が cache** されて、Phase 3 の
patch 後に `dispatcher_name` が stale 化する → `load_all_funcs` で
`SD_<wrong_hash>` を引いて見つからず interp に落ちる。

`node.c::OPTIMIZE` は `parsing_phase == true` のとき早期 return する。
`load_program` が頭で `parsing_phase = true` を立て、Phase 3 後に
`false` に戻すので、parse 中は cs_load が走らず、hash cache も汚れ
ない。`load_all_funcs` で初めて全 body を一括 cs_load するので、
hash は callee patch 済の正しい状態で計算される。

cross-sample な詳細解説は `docs/perf.md §4.5.1` (root)。

## SD 内 inline / extern 判断アルゴリズム

`compile_all_funcs` が各 entry を SD に展開するとき、`call_static` の
callee を **親の SD に inline 展開する** か **extern SD として外に
出す** かを決める必要がある。inline 展開は `castro_gen.rb` の
`castro_build_call_static_specializer` が `SPECIALIZE(fp, callee)` を
recursive に呼ぶことで実現される (callee の body が同じ .c に
`static inline` で書き出される)。extern 展開は callee の
`dispatcher_name` (= `SD_<hash>`) を `extern` 宣言して call するだけ。

判断は `body->head.flags.no_inline` フラグを見る。立っていれば extern、
落ちていれば inline。

このフラグを **どの関数に立てるか** が parse.rb の責務:

```ruby
no_inline_threshold = (ENV['CASTRO_NO_INLINE_THRESHOLD'] || 500).to_i

funcs.each do |name, _, body, _|
  nc = count_nodes(body)
  fp_loop_kernel = max_loop_depth(body) >= 3 && count_fp_ops(body) >= 5
  no_inline = nc > no_inline_threshold || fp_loop_kernel
  emit_sig(name, no_inline ? 'no_inline' : '')
end
```

つまり 2 種類のルールの OR:

1. **サイズ閾値** (`nc > 500`): N 個の callsite から呼ばれる helper を
   inline すると SD source が N×|body| に膨らむ blowup を防ぐ。
   コンパイル時間と icache pressure 両方の対策。
2. **register-pressure 防御** (`depth >= 3 && fp >= 5`): 深ネスト +
   SIMD heavy な kernel を親 SD に inline すると x86-64 の YMM 16本を
   親側 outer-loop と取り合って spill が出る。inner loop の machine
   code は inline しても変わらないが、その周辺の bookkeeping spill
   traffic で IPC が落ちる (gemm: full-inline 480 ms / IPC 2.51 / 24
   spills → depth-3 hoist 340 ms / IPC 3.06 / 10 spills)。

`max_loop_depth` / `count_fp_ops` の実装は parse.rb の
sx 木 walk:

```ruby
fp_ops = %w[mul_d add_d sub_d div_d store_d load_d ge_d le_d lt_d gt_d eq_d neg_d]

max_loop_depth = lambda do |sx, depth = 0|
  next depth unless sx.is_a?(Array)
  d2 = (sx[0] == :for || sx[0] == :while) ? depth + 1 : depth
  ([d2] + sx[1..].map { |x| max_loop_depth.call(x, d2) }).max
end

count_fp_ops = lambda do |sx|
  next 0 unless sx.is_a?(Array)
  c = fp_ops.include?(sx[0].to_s) ? 1 : 0
  c + sx[1..].sum { |x| count_fp_ops.call(x) }
end
```

整数 bit ops 系 (md5、CRC) は `count_fp_ops == 0` で trigger しない →
inline で OK (GPR 16 本で余裕)。浅 loop kernel (nbody: depth=2) も
trigger しない → inline で function call overhead を消した方が速い。

(実装は `parse.rb` 末尾の sig emit ループ。`docs/perf.md §4.8` (root)
にクロスサンプル原則と castro での実測表。)

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
