# astr done

スカラー + ベクタ + 文字列までカバーする R サブセット。AST tree-walking で
fib / ack に対し AOT で ~4× の高速化を出す。

## 言語

- リテラル: 整数 (`123L` 整数リテラル含む)、浮動小数 (`1.5`, `1e3`)、文字列
  (`"..."` / `'...'`)、`TRUE` / `FALSE` / `T` / `F` / `NA` / `NULL`
- 代入: `<-`、`=`（statement scope のみ）、関数代入の `name <- function(...) body`
  形は `node_def` に直接 lower
- 制御: `if`/`else`、`while`、`for (i in 1:n)`、`for (v in c(...))`、`return(x)`
- 演算子: `+ - * / %% %/% ^`、比較 `< <= > >= == !=`、論理 `&& || !`、
  `:` レンジ、`v[i]` / `v[i] <- val` 添字
- ユーザ関数: `function(...)` (引数 0..N、デフォルト引数は parser が認識して
  式を捨てるだけ)、`return` 経由 RESULT_RETURN による非局所脱出
- ビルトイン: `print`, `cat`, `length`, `c`, `paste`, `paste0`, `nchar`,
  `substr`, `sum`, `floor`, `ceiling`, `sqrt`, `abs`, `log`, `exp`, `sin`,
  `cos`, `tan`, `round`, `as.integer`, `as.numeric`, `is.numeric`,
  `is.character`

## ランタイム

- `VALUE` = `int64_t`、low-bit が 1 なら fixnum (signed 63-bit)、0 なら
  `struct astr_obj *` (8-byte aligned ヒープオブジェクト)
- `astr_obj` 種別: `FLOAT`, `STRING`, `NUM_VEC`, `INT_VEC`, `STR_VEC`, `LIST`,
  `NA` / `NULL` (singleton)
- 算術 fast path: fixnum × fixnum はインライン展開（`__builtin_*_overflow`
  でチェックして範囲内なら fixnum、はみ出したら double に昇格）。
  非 fixnum は `astr_*_slow` に外出し、ベクタ/スカラ broadcast を実装。
- フレーム: ユーザ関数呼び出しは `VALUE F[ASTR_FRAME_MAX]` の VLA フレームに
  引数を書き込み、callee に `fp` レジスタとして渡す。
- 関数解決: 各 call site は `struct astr_callcache *cc@ref` のインライン
  キャッシュを持つ。初回は `astr_resolve_body(name)` で `c->func_set` を
  線形検索 → `cc->body` に保存。以降は分岐 1 本 + 直接 dispatch。
- 4 引数以上は `node_call_n` / `node_call_builtin_n` で `ASTR_NODE_TABLE`
  経由のバリアディック（pystro 流）。引数評価は callee フレームに直接書く。
- メモリ: Boehm-Demers-Weiser GC (`-lgc`) を全ヒープ確保に適用。文字列の
  char バッファと double 配列は `GC_malloc_atomic`（pointer-free）。

## AOT bake

- `make c` で `-c` 起動 → `astro_cs_compile(ast)` + 各登録関数 body を bake
  → `code_store/Makefile` で `make -j` → dlopen
- リンクには `-rdynamic` が必須（all.so が host の `astr_resolve_body` と
  ビルトイン C 関数を参照する）。`-rdynamic` 無しだと dlopen 時に
  「undefined symbol: astr_resolve_body」で落ちる。
- `CCACHE_DISABLE=1` を `setenv` で立ててから `astro_cs_build` を呼ぶ。
  ccache のキャッシュディレクトリはサンドボックスから書けないことがあり、
  off にしないと build が exit 512 で死ぬ。

## ベンチ (Linux x86_64, gcc -O3)

| bench       | astr -i | astr -c (cached) | speedup |
|-------------|---------|------------------|---------|
| fib(36)     | 0.81 s  | 0.20 s           | 4.1×    |
| loop(50M)   | 0.91 s  | 0.90 s           | 1.0×    |
| ack(3,9)    | 0.49 s  | 0.13 s           | 3.8×    |

`fib` / `ack` は recursive call が SD chain 内で inline されて効く。
`loop` は `while + lset + add` の tight loop で interpreter dispatch コスト
が元々小さく、AOT の差が出ない。
