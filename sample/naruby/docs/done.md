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
| | `node_call` | `name, argc, arg_index, callcache *cc@ref` | 動的解決 + インラインキャッシュ。argc > 3 fallback |
| | `node_call_0/1/2/3` | `name, arg_index, locals_cnt, callcache *cc@ref [, a0..aN-1]` | plain/AOT 用 arity-N specialized。explicit arg operands + fresh frame、cc->body indirect |
| | `node_call2` | `name, argc, arg_index, callcache *cc@ref, NODE *sp_body` | `-p` の argc > 3 fallback。2 段ガード (cc 鮮度 + cc->body == sp_body) で SD baked-direct を gate |
| | `node_pg_call_0/1/2/3` | `name, arg_index, locals_cnt, callcache *cc@ref, NODE *sp_body [, a0..aN-1]` | `-p` 用 arity-N specialized。同じく 2 段ガード。SD で `SD_<HASH(sp_body)>` を direct call |
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
| `-p` | Profile-Guided | `node_pg_call_<N>` (argc ≤ 3) / `node_call2` (argc > 3) を使う |
| `-a` | Record-all | すべての ALLOC を `code_repo_add` で記録 (specialize 候補を増やす) |
| `-j` | JIT | `astro_jit_*` (UDS 経由で別プロセスの L1 に compile を投げる) |
| `-q` | quiet | 余計な情報を出さない |
| `-h` | help | オプション一覧 |

## HOPT / PGSD 投機チェイン (Profile-Guided Specialization)

ASTro framework の HOPT 機構を有効化。`hash_node()` (HORG, sp_body 除外)
に加えて `hash_node_opt()` (HOPT, sp_body 込み + cycle break) を持つ。

- `node_def` 実行時に `astro_cs_load(body, name)` で body を PGSD に wire up
- top-level AST も `cs_compile(ast, source_file)` + `cs_load(ast, source_file)`
  で PGSD として bake / load
- bodies は AOT(SD_<HORG>) と PGC(PGSD_<HOPT>) の両形を bake
- `hopt_index.txt` に `(Horg, file, line) → Hopt` のマッピングを永続化

非再帰 call chain で **LTO 併用時 25–69% 高速化** (PG → PG+LTO 比、
chain40 が最大 -69%、compose -25%、call -51%、chain20 -44%、chain_add -36%、
deep_const -50%)。再帰関数は cycle break で HOPT が HORG に潰れるため
効果なし (fib/ackermann/tak ±0)。実測は [perf.md](perf.md) の主要観察
セクション参照。

実装: `naruby_gen.rb` の `:hopt` gen_task、`node.def` の `node_def`
拡張、`node.h` の `HOPT` / `hash_node_opt` 宣言、`node.c` の実装、
`naruby_parse.c` の `naruby_current_source_file` グローバル、`main.c` の
`build_code_store` 拡張。生成: `node_hopt.c`。

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
