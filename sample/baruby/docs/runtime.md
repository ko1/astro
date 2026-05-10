# baruby ランタイム構造

言語仕様は [spec.md](spec.md)、未対応項目は [todo.md](todo.md)、
ベンチは [perf.md](perf.md) を参照。

baruby は naruby (`sample/naruby`) のフォークで、ASTro フレームワーク
(`../../lib/astrogen.rb` + `../../runtime/`) の上に乗っている。
**主な追加点は (1) LSB-tagged VALUE / (2) libgc / (3) ヒープ型 (Array,
String) / (4) parse-time のメソッド desugar の 4 つ**。Plain / AOT /
PG の 3 モードは構造的には naruby から継承しているが、新ノードでの
AOT/PG 動作は未検証 (`--plain` のみ確認済)。

## 1. パイプライン

```
   foo.ba.rb
       │
       ▼
   Prism (libprism.so)         libprism は naruby/prism を symlink で共有
       │   pm_node_t* (CRuby と同じ Ruby AST)
       ▼
   transduce  (baruby_parse.c)
       │   PM_* → ALLOC_node_*  (未対応 PM_* は "unsupported" で exit)
       │   メソッド呼び出しは parse-time に desugar (§4)
       ▼
   NODE 木  (head + 各種 operand 構造体)
       │
       ▼
   OPTIMIZE() — code_store/all.so から SD/PGSD があれば bind
       │
       ▼
   EVAL(c, ast, fp)  →  RESULT (= VALUE + state bit)
```

`naruby_gen.rb` 相当の `baruby_gen.rb` が `node.def` から `node_eval.c`
等を生成する仕組みは naruby と同じ。コードジェネレータには手を入れて
いない。

## 2. 値表現 (LSB tag)

```c
typedef intptr_t VALUE;

// LSB == 1                → fixnum (signed int63, 算術右シフトで sign-extend)
// raw == 0                → false / nil 統一
// raw == 2                → true singleton (sub-page、ヒープアドレスにならない)
// LSB == 0, v != 0, 2     → heap object pointer
#define INT2VAL(i)    ((VALUE)(((uintptr_t)(intptr_t)(i) << 1) | 1u))
#define VAL2INT(v)    (((intptr_t)(v)) >> 1)
#define VAL_FALSE     ((VALUE)0)
#define VAL_TRUE      ((VALUE)2)
#define IS_INT(v)     ((v) & 1)
#define IS_PTR(v)     ((v) != 0 && (v) != 2 && ((v) & 1) == 0)
```

設計上の含意:

- **比較は untag 不要**: `(a_tagged < b_tagged)` は signed のまま
  正しい順序になる (両辺が同じ量だけ左シフトされているため)。
- **加減算は untag → op → tag** が必要。`(a + b - 1)` で tag を保つ
  トリックは現状未採用 (clarity 優先、`-O3` で gcc が shift pair を
  畳んでくれる場面が多い)。
- **`if cond`**: C truthy/falsy の意味で `cond != 0` 判定で済む
  (`VAL_FALSE = 0` のみが falsy、`VAL_TRUE = 2` を含めその他は非 0 の
  raw 値)。
- **`&&` / `||` 注意**: `INT2VAL(0) = 1` なので `node_num(0)` を
  「false 相当」として使えない。専用の `node_true` / `node_false`
  ノードが `VAL_TRUE` / `VAL_FALSE` シングルトンを返す。
- **`p` の表示**: `VAL_FALSE` → "false"、`VAL_TRUE` → "true"、
  それ以外は IS_INT / IS_ARY / IS_STR で分岐。`true` と Integer 1 は
  raw 値が違うので別々に表示される。
- **`==` / `!=`**: `l == r` の raw 等価で fixnum / シングルトン /
  ポインタ identity を一発カバー → 違ったら `IS_INT` を見て fast-fail
  → 残りで `baruby_value_eq` (String byte 比較 / Array 再帰)。

## 3. ヒープ型

```c
typedef struct ObjectHeader {
    uint32_t type;       // OBJ_ARRAY (= 1) | OBJ_STRING (= 2)
    uint32_t flags;      // 予約
} ObjectHeader;

typedef struct BaArray {
    ObjectHeader hdr;
    uint32_t len, capa;
    VALUE *items;        // capa 個の VALUE を別 alloc
} BaArray;

typedef struct BaString {
    ObjectHeader hdr;
    uint32_t len, capa;  // len は NUL を含まないバイト長
    char *bytes;         // NUL 終端 (printf 互換性のため)
} BaString;
```

二層オブジェクト (固定サイズ header + 別 alloc な可変長 payload) は
`docs/gc_design.md` §3 の「value.def の標準形」に揃えてある。将来
moving GC に切り替えるとき、payload だけを動かして header を pin する
道も残せる。

## 4. メソッド呼び出しの parse-time desugar

OO 機能を入れない方針 (spec.md 参照) のもと、`recv.method(args)` 形式は
**parse 時にメソッド名固定の専用ノードに変換**される。`baruby_parse.c`
の `PM_CALL_NODE` 分岐で:

```c
if (lhs != NULL) {
    if (ceq(name, "[]"))     return ALLOC_node_call_aget(lhs, idx);
    if (ceq(name, "[]="))    return ALLOC_node_call_aset(lhs, idx, val);
    if (ceq(name, "size") || ceq(name, "length"))
                             return ALLOC_node_call_size(lhs);
    if (ceq(name, "push"))   return ALLOC_node_call_push(lhs, val);
    if (ceq(name, "pop"))    return ALLOC_node_call_pop(lhs);
}
```

各 `node_call_*` は eval 時に recv の型タグ (`IS_ARY` / `IS_STR`) で
runtime branch する。例:

```c
NODE_DEF
node_call_size(... NODE *recv) {
    VALUE r = UNWRAP(EVAL_ARG(c, recv));
    if (IS_ARY(r)) return RESULT_OK(INT2VAL(VAL2ARY(r)->len));
    if (IS_STR(r)) return RESULT_OK(INT2VAL(VAL2STR(r)->len));
    fprintf(stderr, "no size for non-array/string\n");
    return RESULT_OK(INT2VAL(0));
}
```

ASTro の specialization で profile に応じて `_ary` / `_str` variant に
分岐する余地はあるが、現状は generic 1 本のみ。

`node_add` も同じ流儀で **int+int / str+str / ary+ary** を runtime
branch する (LIKELY で int+int のホットパスを優先)。`node_eq` /
`node_neq` も同型で、raw 等価チェックの後に `baruby_value_eq`
(`node.c`) で再帰的な値比較に降りる。

## 5. libgc 統合

asom と同じ「macro wrap で libc shape を維持」パターン。`context.h`
で全 system header の **後ろ** に macro を仕込むことで、libc/libgc
内部実装は plain symbol を保ち、baruby 側のソースだけが GC alloc に
リダイレクトされる:

```c
#include <gc.h>
// ... (system headers above)
#define malloc(n)      GC_MALLOC(n)
#define calloc(n, s)   GC_MALLOC((size_t)(n) * (size_t)(s))
#define realloc(p, n)  GC_REALLOC((p), (n))
#define strdup(s)      GC_STRDUP(s)
#define free(p)        ((void)(p))
```

`main.c` 冒頭で `GC_INIT()`、Makefile に `-lgc` を追加 (それだけ)。

`BARUBY_GC_STATS=1` を環境変数で渡すと、終了時に
`__GC_STATS__ alloc_bytes=... heap_bytes=... gc_count=...` を出力する
(`GC_get_total_bytes` / `GC_get_heap_size` / `GC_get_gc_no`)。bench
ランナー (`bench/run.rb`) はこれをパースして表に出す。

## 6. 文字列リテラルの fresh alloc

`node_str_lit(bytes, len)` は **eval 毎に新しい `BaString` を確保する**。
intern pool は持っていない。これは GC testbed としての feature
(同じリテラルが loop 内で大量にゴミを生む = collector が忙しくなる)。

ベンチ用途を超えて常用するなら parse-time に `BaString *` を生成して
キャッシュする intern pool が欲しい (todo.md)。

## 7. 配列リテラルのチェイン展開

`[a, b, c]` は parse 時に下記の AST に落ちる:

```
ary_push(
  ary_push(
    ary_push(ary_new(), a_expr),
    b_expr),
  c_expr)
```

`ary_new` は eval されるたびに新しい空配列を確保する。各 `ary_push`
は引数 lhs を eval して同じ配列をそのまま返すので、AST が線形チェインで
展開されてもアロケーションは leaf の `ary_new` 1 回だけ (= リテラル評価
1 回ぶん)。

## 8. ベンチ・実行モード

`make` (= `make all`) で `./baruby` ができる。Makefile target:

| target | 効果 |
|---|---|
| `make` | 通常ビルド |
| `make run` | `./baruby test.ba.rb` |
| `make c` | `./baruby -c test.ba.rb` (AOT bake & run) — 新ノードで未検証 |
| `make bench` | `bench/run.rb` で 3 ベンチを順に実行 |
| `make clean` | 生成物 + code_store を消す |

`bench/run.rb` の引数:

```
ruby bench/run.rb [--mode plain|aot|pg] [-n REPEATS] [bench/...]
```

## 9. 既知の不整合

- `nil` と `false` が同じ raw 0 — `nil`/`false` を区別したいときは別の
  シングルトン値が要る。
- AOT (`-c`) / PG (`-p`) で baruby が新ノードを正しく specialize できるかは
  未検証。少なくとも `node_str_lit(const char *, uint32_t)` の `const char *`
  オペランドは naruby の関数名と同じ扱いで HORG に取り込まれる想定だが
  動作確認は未実施。
- `node_add` の string 経路は branch predictor に依存している (int 多数の
  ループでは LIKELY のおかげで predict miss しないはず)。
