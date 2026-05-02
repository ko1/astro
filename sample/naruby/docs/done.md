# naruby 実装済み機能

言語仕様は [spec.md](spec.md)、未対応項目は [todo.md](todo.md)、
ランタイム構造は [runtime.md](runtime.md) を参照。

## ノード一覧

`node.def` で定義されている AST ノード — 22 種。

| カテゴリ | ノード | オペランド | 概要 |
|---|---|---|---|
| 値 | `node_num` | `int32_t num` | 整数リテラル |
| ローカル変数 | `node_lget` | `uint32_t index` | `fp[index]` を読む |
| | `node_lset` | `uint32_t index, NODE *rhs` | rhs を eval して `fp[index]` に書く |
| 制御構造 | `node_seq` | `NODE *head, NODE *tail` | head; tail。値は tail |
| | `node_if` | `NODE *cond, NODE *then, NODE *else` | cond が 0 でなければ then |
| | `node_while` | `NODE *cond, NODE *body` | cond が 0 になるまで body を回す。値は 0 |
| | `node_scope` | `uint32_t envsize, NODE *body` | `fp + envsize` で body を eval |
| | `node_return` | `NODE *value` | `RESULT { value, RESULT_RETURN }` を返す |
| 関数 | `node_def` `@noinline` | `name, body, params_cnt, locals_cnt` | function_entry を登録、`c->serial++` |
| | `node_call` | `name, argc, arg_index, callcache *cc@ref` | 動的解決 + インラインキャッシュ |
| | `node_call2` | `name, argc, arg_index, callcache *cc@ref, NODE *sp_body` | `-p`: sp_body を parse 時に link、SD で `SD_<HASH(sp_body)>` を direct call |
| | `node_call_static` | `NODE *body, arg_index` | `-s`: parse 時に body 解決済 |
| | `node_call_builtin` | `bf, builtin_func_ptr, params_cnt` | C 関数を直接呼ぶ |
| 算術 | `node_add` `node_sub` `node_mul` `node_div` `node_mod` | `NODE *lhs, NODE *rhs` | 整数演算 |
| 比較 | `node_lt` `node_le` `node_gt` `node_ge` `node_eq` `node_neq` | `NODE *lhs, NODE *rhs` | 整数比較、結果は 0 / 1 |

すべてのノードは `EVAL_*` (純粋な評価ロジック) と `DISPATCH_*`
(ASTroGen が `EVAL_*` を呼ぶ薄いラッパ) を持ち、specialize 時には
`SD_<hash>` 関数として code_store に書き出される。

## 言語機能

### リテラル
- ✅ 整数 (`0`, `42`, `0xff`, `0b1010`)
- ❌ 浮動小数 / 文字列 / Symbol / Array / Hash / Range / Regexp / nil / true / false

### 変数
- ✅ ローカル変数 (read / write)
- ❌ グローバル変数 / インスタンス変数 / クラス変数 / 定数

### 演算子
- ✅ 算術: `+` `-` `*` `/` `%`
- ✅ 比較: `<` `<=` `>` `>=` `==` `!=`
- ❌ 論理 (`&&` `||` `!`) / ビット演算 / 文字列連結 / Range 演算子 / `<=>` / `**`

### 制御構造
- ✅ `if` / `elsif` / `else`
- ✅ `while`
- ✅ `return` (RESULT 型による非局所脱出 — 詳細は [runtime.md](runtime.md))
- ❌ `unless` / `until` / `for` / `case` / `break` / `next` / `redo`

### 関数
- ✅ `def name(args)` (positional のみ)
- ✅ 関数呼び出し `name(args)` (self-call)
- ✅ 組み込み関数 (`p`, `zero`, `bf_add`)
- ❌ デフォルト引数 / kwargs / `&block` / splat / レシーバ付き呼び出し
- ❌ ブロック / proc / lambda / yield

### オブジェクト指向
- ❌ class / module / 継承 / mixin / メタクラス — まったくなし

## 実行モード

`./naruby` 1 バイナリで全モードを切り替える。詳細は [runtime.md](runtime.md)
の §5。

| フラグ | モード | 動作 |
|---|---|---|
| (なし) | Plain + AOT bake | インタプリタで実行し、終了時に `code_store/all.so` に SD_ をベイクする |
| `-i` | Plain | AOT load も bake もしない |
| `-b` | benchmark | bake をスキップ (load は行う)。タイミング測定用 |
| `-c` | Compile-only | 実行せず SD_ をベイク |
| `-s` | Static-lang | `node_call_static` を使う (parse 時に body 解決) |
| `-p` | Profile-Guided | `node_call2` を使う |
| `-a` | Record-all | すべての ALLOC を `code_repo_add` で記録 (specialize 候補を増やす) |
| `-j` | JIT | `astro_jit_*` (UDS 経由で別プロセスの L1 に compile を投げる) |
| `-q` | quiet | 余計な情報を出さない |
| `-h` | help | オプション一覧 |

## ASTro framework との接続

- ノード生成: ASTroGen (`../../lib/astrogen.rb`) が `node.def` から
  `node_eval.c` / `node_dispatch.c` / `node_dump.c` / `node_hash.c` /
  `node_specialize.c` / `node_replace.c` / `node_alloc.c` /
  `node_head.h` を生成。
- ハッシュ / DUMP / 名前割当: `runtime/astro_node.c` を `node.c` から
  `#include` して共有。`hash_builtin_func` だけ naruby 側で追加。
- AOT コードストア: `runtime/astro_code_store.c` を `node.c` から
  `#include`。`astro_cs_init / load / compile / build / reload` の
  5 関数で完結する dlopen ベースのキャッシュ。
- JIT: naruby 専用の `astro_jit.c` (UDS で L1 サーバに送信、`astrojit/`
  下にビルドされる別 .so を `dlopen`)。astro_code_store とは独立して
  動くが、内部のコード生成 (SPECIALIZED_SRC) は同じ specializer を共有。

## ビルド・実行

```sh
make                    # ./naruby を作る (libprism + node.def 生成 + コンパイル)
make run                # ./naruby test.na.rb を時間付きで実行
make crun               # make clean && make run
make c                  # ./naruby -c test.na.rb (compile-only)
make ctrain             # make clean → make → ./naruby -c → 再 make
make ptrain             # 同じく -p で
make bench BITEM=fib    # bench/fib.na.rb で AOT/PG/インタプリタ/gcc を比較
```

ccache が `~/.cache/ccache` に書けない sandbox 環境では `CCACHE_DISABLE=1`
を環境変数で渡す。
