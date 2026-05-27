# koruby_precise — koruby + precise GC framework

`sample/koruby` を ASTro precise GC framework (= `runtime/precise_gc/`) に
fork した版。 koruby が libgc (= Boehm conservative GC) で動いていたのを、
15 種類の precise GC backend を切替て試せる testbed に変える migration。

## migration status (2026-05)

- ✅ **Phase 1**: 字面 fork + AroObjectHeader 統合 + libgc API stub 化 + build pass
- ✅ **Phase 2**: AROH_VISIT_ROOTS 実装 (= value stack + korb_vm 全 class pointers
     + globals method_table + main_obj + current_frame + cref chain)
- ✅ **Phase 3**: AROH_SCAN_EDGES per heap obj type (= T_OBJECT/STRING/ARRAY/HASH/
     RANGE/CLASS/MODULE/PROC/FLOAT/BIGNUM の switch)
- ✅ **bootstrap CTX 整理**: aro_gc_init を class 作成前に繰り上げ、 全 heap obj が GC heap 上
- ⏳ **Phase 4** (= future): fiber stack precise tracking (= GC_add_roots 代替)、
     binding / call cache の visit
- ⏳ **Phase 5** (= future): GMP bignum finalizer (= aro_gc_finalize_register +
     AROH_FINALIZE)
- ⏳ **Phase 6** (= future): sp[] spill discipline を node_eval.c の hot path
     全箇所に適用 (= 移動 GC + STRESS audit 通過のため、 数 session 規模)

## 動作確認 (現状)

| 条件 | 結果 |
|---|---|
| GC=copy (default) で fib.ko.rb | ✅ 9227465 (= fib(35) を 30ms 程度で計算) |
| 15 backend × no-STRESS × fib.ko.rb | ✅ 全 15/15 pass |
| GC=copy × no-STRESS × test/test_*.rb (25 tests) | ✅ 25/25 pass |
| 非移動 backend × STRESS=1 × fib.ko.rb | ✅ mark/mark_gen/mark_gen_inc/immix/bump pass |
| 移動 backend × STRESS=1 | ❌ crash (= sp[] spill 未実装) |
| 全 backend × STRESS=1 × test suite | ❌ many fail (= 同上、 deep precise rooting 要対応) |

## 概要

- **VALUE 表現は CRuby x86_64 互換。** `Qfalse=0`, `Qnil=8`, `Qtrue=0x14`, FIXNUM=低位ビット 1, FLONUM=低位 2 ビット 0b10, SYMBOL=低位 8 ビット 0x0c, ヒープオブジェクトは `RBasic { AroObjectHeader head, struct korb_class *klass }` ヘッダで開始。
- **GC: ASTro precise GC framework.** `make GC=<backend>` で 15 種類から選択 (= copy / mark / mark_gen / copy_gen / mark_compact / mark_compact_gen / mark_bump_gen / immix / immix_gen / mark_bitmap_gen / mark_card_gen / mark_freelist / mark_gen_inc / bump / none)。 詳細は `docs/precise_gc_quickstart.md`、 `docs/gc_design.md` 参照。 デフォルト copy (= Cheney semispace)。 audit run は `make ARO_GC_WB_AUDIT=1` + `BARUBY_GC_STRESS=1 BARUBY_GC_PURGE=1`。
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

- 真の正規表現 (Regexp は文字列スタブ; `=~`/`match`/`scan` は no-op)
- splat 引数 (`*args`) のメソッド受け側 (一部対応; 全箇所未対応)、kwargs (`**opts`)、ブロック引数 (`&blk`) の受け側
- ブロックでの destructure (`each {|k, v| ...}` で 2 要素配列を分解)
- `Comparable`/`Enumerable` の真の mixin (現在は flatten copy)
- `Object#method`、`Method`/`UnboundMethod` クラス
- 真の Symbol#to_proc
- 多重代入の splat / nested patterns
- `Fiber`、Thread
- IO の本格実装、Encoding、Float の細かい挙動 (Infinity, NaN)

### optcarrot 対応状況

✅ **完走!** NES のエミュレーションが最後まで動作:

```sh
$ /path/to/koruby -e 'require_relative "lib/optcarrot";
    Optcarrot::NES.new(["-b", "--frames", "30", "examples/Lan_Master.nes"]).run'
fps: 71.47
checksum: 4096
```

- 30 フレーム実行: koruby 12.97s, CRuby 1.18s (~11× 遅い、AOT 特化なし)
- 注: video checksum は CRuby と一致しない (emulation 細部に微妙な差; 走るが完全互換ではない)
- 詳細: [docs/done.md](./docs/done.md#optcarrot-対応の現状)

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

## インストール

### 前提パッケージ (Ubuntu/Debian)

```sh
sudo apt install build-essential ruby ruby-bundler libgc-dev libgmp-dev git
```

- `build-essential` — gcc / make
- `ruby` (3.x) + `bundler` — ASTroGen 実行 + libprism のビルドに必要
- `libgc-dev` — Boehm GC (conservative GC、ルート登録不要)
- `libgmp-dev` — Bignum バックエンド

prism は `../naruby/prism` への symlink で共有しているので、
naruby 側で一度 libprism を build しておく必要がある (詳細は
[`../naruby/README.md`](../naruby/README.md) の libprism セクション)。

### ビルド & 実行

```sh
make                                # 1回目ビルド
./koruby fib.ko.rb                  # 実行
./koruby -e 'p 1+2'                 # 一行評価
./koruby --dump -e '...'            # AST ダンプ
./koruby file.rb arg1 arg2 ...      # ARGV へ渡す

# AOT 特化 (code_store/all.so を runtime dlopen)
make clean && make                  # 1回目: 普通の interpreter
./koruby --aot-compile your_script.rb  # run + SD_<hash>.c 群 + all.so 生成
./koruby your_script.rb             # 2 回目以降は all.so を auto-load
```

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
