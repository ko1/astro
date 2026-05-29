# abc ランタイム / 値表現

## 値: `bcnum *`

EVAL を貫く `VALUE` (= `bcnum *`) は **タグ付きポインタ**:

```c
typedef struct bcnum {
    mpz_t m;     // 符号つき仮数 (GMP)
    long scale;  // 小数桁数 >= 0
} bcnum;         // 値 = m × 10^(-scale)
```

- **LSB=1 → 即値 fixnum**: scale 0・62bit に収まる整数を `(intptr_t)v >> 1` で持つ。
  ヒープも GMP も GC も触らない。`bc_mkfix` / `BC_IS_FIX` / `BC_FIX_VAL` (bcnum.h)。
- **LSB=0 → ヒープ `bcnum *`** (8byte aligned): scale>0 か範囲外の値。
- `+ - * / % == < …` は両辺が即値なら native long で計算 (`__builtin_*_overflow` で
  桁あふれ検出)、あふれ/小数が絡むときだけヒープへ materialize して GMP。
- 即値はポインタを含まないので、GC は scan 中に見つけた奇数ワードを単に無視する
  (誤検出なし)。ヒープ `bcnum *` は 8byte aligned なので通常通り辿られる。
- 値は **immutable**: 各演算は新しい値を返す。共有しても安全なので変数代入・
  インクリメント・関数引数渡しは単純なポインタ(または即値)差し替え。`bc_rescale` は
  scale 一致時にコピーせず引数を共有する。
- 文字列は第一級の値ではない。`print` 項と裸文字列文の中だけに現れ、専用ノードが
  直接出力する。

## GMP × libgc の配線

GMP の仮数 limb はポインタを含まない整数列なので、**GC の atomic オブジェクト**に
できる。`INIT()` で:

```c
GC_INIT();
mp_set_memory_functions(gmp_alloc, gmp_realloc, gmp_free);
//   gmp_alloc   = GC_MALLOC_ATOMIC   (limb はスキャン不要)
//   gmp_realloc = GC_REALLOC         (atomic 属性を保つ)
//   gmp_free    = no-op              (GC が回収)
```

- `bcnum` 構造体自体は `GC_MALLOC` (= scanned)。中の `mpz_t` が持つ limb ポインタを
  GC がたどれる。
- limb は atomic なので GC が中身をポインタと誤認しない (false retention 回避)。
- どこからも `mpz_clear` を呼ばない。limb は GC が回収する。

## ルートの可達性

- `CTX` は `GC_MALLOC` で確保し、`main` のローカル `c` (= スタックルート) から到達。
  GC は `CTX` を走査するので `last` / `retval` / シンボル表が生きる。
- シンボル表 (`bc_symtab` / `var_slot` / `bc_array`) もすべて `GC_MALLOC`。
  変数が持つ `bcnum *` が辿れる。
- EVAL 中の中間 `bcnum *` は C スタック・レジスタ上にあり、Boehm GC が
  conservative にスキャンする。
- **AST ノードは `calloc`** (GC 外, 永続)。AST は `bcnum` を一切持たない (リテラルは
  文字列で保持) ので GC 管理不要。関数本体 (`bc_func.body`) も AST なので永続。

## シンボル表

スカラ・配列・関数を 1 つの名前空間スロットにまとめる (bc は別名前空間だが、
1 スロットに 3 フィールドを持たせれば衝突しない):

```c
struct var_slot { const char *name; bcnum *scalar; struct bc_array *arr; struct bc_func *func; ... };
```

`scale` / `ibase` / `obase` / `last` は表に入れず、get/set で `CTX` のフィールドへ
ルーティングする (特殊変数)。

## 関数呼び出し (動的スコープ)

bc の auto は動的スコープ。`bc_call` は:

1. 実引数を**呼び出し元スコープで**左から評価し配列に退避。
2. 仮引数・auto の現在スロット値を退避し、新値 (引数のコピー / 0 / 空配列) を束縛。
3. 本体を `EVAL(c, f->body)` で実行。`return` は `c->flow = FLOW_RETURN` で巻き戻る。
4. スロットを復元し、戻り値を返す。

再帰は退避用配列が C スタックに載るので自然に動く。

## 非局所制御

`return` / `break` / `continue` は `c->flow` (列挙) で伝播する。`node_seq` と
ループノードが各サブ評価後に `c->flow` を見て巻き戻し、ループ境界・関数境界で
捕捉して `FLOW_NORMAL` に戻す。これにより EVAL の戻り値型を `bcnum *` 1 本に保てる
(RESULT 構造体を導入しない)。

## 実行時エラー

`bc_runtime_error` は stderr に出力し `longjmp` でトップレベルへ戻る。`main` の
`run_source` は **トップレベル文ごとに** `setjmp` で囲むので、ゼロ除算等は当該文だけ
中断して次文へ進む (bc と同じリカバリ)。

## AOT 特殊化

`--aot-compile` 実行時:

1. 各トップレベル文と**各関数本体**を `astro_cs_compile` で SD 化。関数本体は
   `EVAL(c, f->body)` という runtime ポインタ越し dispatch なので、各々を独立 entry
   として登録する必要がある (cf. usage.md「Entry nodes」)。
2. `astro_cs_build` → `astro_cs_reload` で `code_store/all.so` を構築・ロード。
3. 各 entry に `astro_cs_load` して dispatcher を SD に差し替え。

SD は `bc_add` 等の helper を外部参照するので、ホスト exe は `-rdynamic` でリンクして
シンボルを公開している (dlopen 時に解決させるため)。性能特性は [perf.md](perf.md)。
