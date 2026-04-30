# koruby — kind of Ruby

ASTro framework を使った Ruby サブセット処理系。
naruby / abruby を参考に **CRuby C extension ではない、スタンドアロン Ruby 処理系** として実装。

## 設計方針

- **VALUE 表現** — CRuby x86_64 互換 (FIXNUM/FLONUM/SYMBOL/Qnil/Qtrue/Qfalse の即値、ヒープオブジェクトは `RBasic`互換ヘッダで開始)。CRuby のコードを将来流用しやすいように。
- **GC** — Boehm GC (libgc, conservative)。ノードや VALUE の手動 mark/sweep が不要で、書きやすい。
- **Bignum** — GMP (mpz_t)。`Integer` クラスは Fixnum ↔ Bignum を透過に扱う。
- **パーサ** — Prism (CRuby と同じ AST 生成器)。`prism/` は naruby のものへの symlink。
- **AST ノード** — ASTro `node.def` で定義し、`koruby_gen.rb` が `ID`/`intptr_t`/`struct method_cache *` 等 koruby 固有型のハッシュ・特化サポートを追加。
- **クロージャ** — `__shared-fp__` 方式。`yield` で呼ばれるブロックは親フレームと **同じ fp** を共有する（escape しない前提）。`param_base` でブロック自身のローカル開始位置を記録。

## 実装済み機能

| カテゴリ | サポート |
|---|---|
| リテラル | 整数 (Fixnum/Bignum)、浮動小数、文字列、シンボル、true/false/nil、self、配列、ハッシュ、Range |
| 変数 | ローカル変数 (depth対応)、インスタンス変数 (shape ベース)、グローバル変数、定数、`Foo::Bar` パス |
| 制御 | if / unless / while / until / break / next / return |
| 演算 | `+ - * / % == != < <= > >= << >> & \| ^`、Fixnum 高速パス + オーバーフロー時 GMP 昇格、Array/Hash の [] []= |
| 論理 | `&& || !` (短絡評価) |
| メソッド | `def`、明示レシーバ呼び出し、暗黙 self、ブロック付き、`yield`、インラインキャッシュ |
| クラス | `class Foo < Bar`、`module M`、メソッド継承、`Class.new` |
| ブロック/Proc | `{|x| ...}`、`->{}`、Proc#call、yield 経由のブロック呼び出し |
| 例外 | `begin/rescue`、`raise`、`Kernel#raise` |
| 組込 | Kernel: p / puts / print / class / inspect / to_s / nil? / is_a? / respond_to? <br> Integer: 算術・比較・ビット演算・to_s/i/f / times / succ <br> Array: size/[] / push / each / map / select / reduce / join / inspect / == <br> Hash: [] / []= / size / each <br> Range: each / first / last / to_a <br> String: + / << / size / == / to_sym <br> Class: new / name <br> Proc: call |

未対応の主な機能: case/when, 多重代入, 多くの ivar attr_*, ensure, super, lambda の return 文セマンティクス, 定数の lexical scope, モジュール include の mixin, 文字列 interpolation の細かい挙動など。

## ビルド & 実行

```sh
make                     # ビルド
./koruby fib.ko.rb       # 実行
./koruby -e 'p 1+2'      # 一行評価
./koruby --dump -e '...' # AST ダンプ
./koruby -c fib.ko.rb    # 特化Cコード生成 (node_specialized.c)
```

依存: `libgc-dev`、`libgmp-dev`、prism (build/static にある同梱版を利用)。

### 二段ビルド (AOT 特化)

```sh
make clean
make                                      # 1回目: 普通の interpreter
./koruby -c fib40.ko.rb                   # node_specialized.c を生成
touch node.c && make optflags="-O3"      # 2回目: 特化を埋め込んでビルド
./koruby fib40.ko.rb                      # 高速版実行
```

## ベンチマーク (fib(40), x86_64)

| 構成 | 時間 |
|---|---|
| ruby (no JIT) | 4.5s |
| **ruby --yjit** | **1.2s** |
| koruby (interp) | 7.9s |
| koruby (AOT 特化, -O3) | 3.0s |

## アーキテクチャ

```
koruby/
├── context.h          # VALUE, CTX, method_cache などコア型
├── object.h object.c  # クラス・オブジェクト・String/Array/Hash/Range/Bignum
├── builtins.c         # 組込メソッドの cfunc 実装
├── node.def           # ASTro AST ノード evaluator (node_*  EVAL 本体)
├── node.h node.c      # ASTro ランタイム (HASH/EVAL/OPTIMIZE/SPECIALIZE) + 生成 .c の include
├── koruby_gen.rb      # ASTroGen サブクラス (ID/intptr_t/method_cache 対応)
├── parse.c            # Prism AST → koruby AST (transduce + closure depth)
├── main.c             # entry point
├── prism/             # symlink → ../naruby/prism
└── Makefile
```

## 既知の制限

- Boehm GC は conservative なので、long-running プログラムでは naruby より GC pause が大きい場合あり。
- Proc 連結ブロックは escape しない前提 (env が親 fp を直接指す)。
- Bignum 演算は GMP に直接委譲、Fixnum/Bignum 混合の比較は ko_int_cmp で。
