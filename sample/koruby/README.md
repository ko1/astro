# koruby — *kind of Ruby*

ASTro framework を用いた **CRuby とは独立した** Ruby 処理系。
naruby (整数のみ Ruby サブセット) と abruby (CRuby C extension) を踏まえ、**スタンドアロンで動く全機能 Ruby 処理系** を目指して実装中。

## 概要

- **VALUE 表現は CRuby x86_64 互換。** `Qfalse=0`, `Qnil=8`, `Qtrue=0x14`, FIXNUM=低位ビット 1, FLONUM=低位 2 ビット 0b10, SYMBOL=低位 8 ビット 0x0c, ヒープオブジェクトは `RBasic { flags, klass }` ヘッダで開始。これにより CRuby のソースコードを将来流用しやすくしている (例: `array.c` の Array 実装を持ってくれば `RARRAY_LEN`/`RARRAY_PTR` 系マクロが意味を持つ)。
- **GC: Boehm GC (libgc).** Conservative GC のためルート登録不要。マーク関数を書く必要がない代わりに、long-running ワークロードで pause が naruby の `malloc`-and-leak より遅くなる場合あり。
- **Bignum: GMP (libgmp).** Fixnum オーバフロー時に透過的に `mpz_t` 経由のヒープ Bignum へ昇格。
- **パーサ: Prism.** CRuby と同じ `prism` を使用 (`prism/` は naruby の build へ symlink)。
- **AST: ASTro.** `node.def` で各ノードの evaluator を C で書き、`koruby_gen.rb` (ASTroGen サブクラス) が `ID` / `intptr_t` / `struct method_cache *` などの koruby 固有型を扱うハッシュ・特化サポートを追加。
- **クロージャ: 共有 fp 方式.** `yield` で呼ばれるブロックは親フレームと **同じ fp を共有** する (escape しない前提)。`param_base` でブロック自身のローカル開始位置を記録。escape 可能な Proc では env を heap 化する必要があるが、現状未対応。
- **例外伝搬: state propagation.** `setjmp/longjmp` を使わず、`CTX::state` に `KORB_NORMAL/RAISE/RETURN/BREAK/NEXT` を持たせ、各 `EVAL_ARG` の後に分岐 (`UNLIKELY` 付き)。`node_rescue`/`node_ensure` で state をクリア。詳細は [docs/runtime.md](./docs/runtime.md)。

## 現状 (Status)

### 動くもの

- 整数 (Fixnum/Bignum)、Float、文字列、Symbol、true/false/nil、Array、Hash、Range
- ローカル変数 (closure depth 対応)、ivar、gvar、lexical 定数 (cref チェイン)
- `if`/`unless`/`while`/`until`/`break`/`next`/`return`、`&&`/`||`/`!`
- `case`/`when` (内部で if-chain に lower)
- 多重代入 `a, b, c = expr`
- 算術 `+ - * / %` ＋ 比較 ＋ ビット演算 (Fixnum 高速パス、オーバフロー時 Bignum)
- `def`/`class`/`module`、メソッド継承、メソッド呼出 (インラインキャッシュ)
- `yield`/Proc/`->`/`{|x|...}`、`Proc#call`
- `begin`/`rescue`/`ensure`/`raise`、Exception クラス階層
- `super`/`super(args)`/`super` (引数なし forward)
- attr_reader/writer/accessor、include、private/public/protected (no-op)
- `Struct.new`、`File.read`/`File.join`/`File.exist?`、`STDOUT`/`STDERR`/`$stdout`/`$stderr`
- `require`/`require_relative`/`load` (循環防止)
- 多数の組込メソッド (Kernel, Integer, Float, String, Array, Hash, Range, Symbol, Proc, Class, Module)
- ARGV/ENV (top-level 定数)

### 動かないもの

- splat 引数 (`*args`)、kwargs (`**opts`)、ブロック引数 (`&blk`) のメソッド受け側
- `Comparable`/`Enumerable` の真の mixin (現在は flatten copy)
- `define_method`、特異クラス (singleton class)
- `Object#method`、`Method`/`UnboundMethod` クラス
- 真の Symbol#to_proc
- 多重代入の splat / nested patterns
- 真の正規表現 (Regexp は文字列スタブ)
- `Fiber`、Thread
- IO の本格実装、Encoding、Float の細かい挙動 (Infinity, NaN)

詳細は [docs/done.md](./docs/done.md) と [docs/todo.md](./docs/todo.md) を参照。

## ベンチマーク (fib(35), x86_64 Linux)

| 構成 | 時間 |
|---|---|
| ruby (no JIT) | 0.90s |
| **ruby --yjit** | **0.15s** |
| koruby (interp, -O2) | 0.55s |
| koruby (AOT 特化, -O3) | 0.24s |

- インタプリタ単体で CRuby (no JIT) の 1.6× 速い
- AOT 特化で 3.6× 速い
- YJIT には 1.6× 負け (ASTro の特化はノードグラフのインライン化までで、メソッド呼出ディスパッチ自体は完全には消せない。PG-baked call_static で詰めれば近づく見込み — 詳細は [docs/perf.md](./docs/perf.md))

## ビルド & 実行

```sh
make                                # 1回目ビルド
./koruby fib.ko.rb                  # 実行
./koruby -e 'p 1+2'                 # 一行評価
./koruby --dump -e '...'            # AST ダンプ
./koruby file.rb arg1 arg2 ...      # ARGV へ渡す

# AOT 特化 (二段ビルド)
make clean && make                  # 1回目: 普通の interpreter
./koruby -c your_script.rb          # node_specialized.c を生成
touch node.c && make optflags=-O3   # 2回目: 特化を埋め込んでビルド
./koruby your_script.rb             # 高速版実行
```

依存: `libgc-dev`、`libgmp-dev`、prism (build/static 同梱版を symlink で利用)。

## アーキテクチャ

```
koruby/
├── context.h          # VALUE, CTX, method_cache, cref など中核型
├── object.{h,c}       # クラス・オブジェクト・String/Array/Hash/Range/Bignum
│                      #   Boehm GC ラッパ (korb_xmalloc 等)、ID intern、メソッド
│                      #   ディスパッチ、require/load
├── builtins.c         # 組込メソッド本体 (cfunc 実装)
├── node.def           # ASTro AST evaluator (約 70 ノード)
├── node.{h,c}         # ASTro ランタイム (HASH/EVAL/OPTIMIZE/SPECIALIZE)
├── koruby_gen.rb      # ASTroGen サブクラス (ID/intptr_t/method_cache 拡張)
├── parse.c            # Prism AST → koruby AST (transduce + closure depth)
├── main.c             # entry point + ARGV/環境セットアップ
├── prism/             # symlink → ../naruby/prism (build/libprism.{a,so})
├── docs/              # 詳細ドキュメント
│   ├── done.md        # 実装済み機能 / 性能改善
│   ├── todo.md        # 未実装 / 今後の課題
│   ├── runtime.md     # 実装の解説 (特にメソッド dispatch)
│   └── perf.md        # 成功 / 失敗した最適化のまとめ
└── Makefile
```

## 関連ドキュメント

- [docs/runtime.md](./docs/runtime.md) — 実装の仕組み (VALUE 表現、メソッド呼出、クロージャ、例外、cref)
- [docs/perf.md](./docs/perf.md) — ベンチマーク結果と最適化の経緯 (成功 / 失敗どちらも)
- [docs/done.md](./docs/done.md) — 機能ごとの実装ステータス
- [docs/todo.md](./docs/todo.md) — 残課題 (言語仕様 / 性能の双方)
