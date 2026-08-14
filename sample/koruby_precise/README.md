# koruby_precise

Ruby サブセットの言語実装。**ASTro フレームワーク**（AST を辿るインタプリタの部分評価による高速化）の上に、
**precise rooting + moving/copy GC** で構築されている。CRuby drop-in を目標に、core 言語・core クラス・
正規表現・stdlib の広範囲をカバーし、実物の **optcarrot（NES エミュレータ）を CRuby と checksum 完全一致**で動かす。

> v1（2026-06 以前）は slots ABI で全面再構築（v2）した。以降 M0（calc 級 subset）から出発し、
> 現在は下記の通り実用 core Ruby 処理系まで到達している。設計は [docs/v2_design.md](./docs/v2_design.md)、
> CLI/AOT/gate 仕様は [docs/v2_spec.md](./docs/v2_spec.md)。

## 実行モデル

同じ AST から2通りに実行できる（ASTro の核心）:

- **インタプリタ**（tree-walk, `--plain`）— コード生成なしの共通エンジン。tree-walk dispatcher は **~68 KB** と極小。
- **AOT**（`--aot-compile` → `--compiled-only`）— 各ノードを部分評価して特殊化ディスパッチャ（SD）を C に吐き、
  `code_store/all.so` として dlopen。プログラムごとに特殊化コードを生成する。

GC は **precise moving/copy GC**（`GC=copy` default）。全ての alloc-heavy path を
`BARUBY_GC_STRESS=1 BARUBY_GC_PURGE=1`（毎 alloc で GC + retired plane を mprotect）で検証している。

## 性能（optcarrot, 180 フレーム, checksum 59662 全一致）

| 実行系 | fps | 対 素の CRuby | 対 CRuby+YJIT |
|---|---:|---:|---:|
| **koruby AOT** | **73.4** | **2.40×（速い）** | 0.63× |
| koruby interp (tree-walk) | 34.7 | 1.14×（速い） | 0.30× |
| CRuby (no yjit) | 30.5 | 1.00× | — |
| CRuby + YJIT | 115.8 | 3.79× | 1.00× |

- **AOT は素の CRuby を 2.4× 上回る**。tree-walk インタプリタ単体でも素の CRuby を上回る。
- YJIT には optcarrot（method-call / object アクセス支配のワークロード）で負ける。マイクロベンチでは
  多くで AOT が YJIT を上回り、再帰（ackermann/fib）と object 系で負ける、という分布。
- バイナリ: 本体は strip 後 **1.77 MB**（`-ggdb3` 込みだと 9.15 MB、うち 7.2 MB がデバッグ情報）。
  optcarrot 全体を AOT 特殊化すると `code_store/all.so` が **~2.9 MB**（~943 SD / 523 TU）。

## rubyspec 充足（core, CRuby drop-in 目標）

計測は **本物の mspec を無改造の spec に噛ませる** `tools/mspec_real_run.rb` を使う。
2026-08-14 時点:

```
DUMP=core.tsv ruby tools/mspec_real_run.rb ~/ruby/src/master/spec/ruby/core 12
files=2144   fully-clean（0 fail 0 err）= 1,026
examples=22,326  pass=17,908  fail=3,111  err=1,307   → core example pass-rate 80.2%
```

- 単一 spec の失敗詳細は `ruby tools/runspec1.rb <spec>`（例 `array/uniq`）。
- `tools/rubyspec_run.rb`（mspec **shim** + spec 連結方式）は速いが `it_behaves_like` や
  mock が独自実装なので **pass を水増しする**（同時期に shim 86.7% / 実 mspec 78%）。
  数字を出すときは実 mspec のほうを使うこと。
- 残りの分布と意図的除外は [docs/rubyspec.md](./docs/rubyspec.md)。

corpus（`make test`）は CRuby オラクル差分の golden test **100,354 件を 0 fail / 0 crash** で維持している。

## 実装済み機能（抜粋・すべて CRuby 一致を確認）

- **言語**: closure/proc/lambda/`->`、block・`yield`・`&blk`・`Symbol#to_proc`、splat `f(*a,&p){}`・
  `**kwargs`・anonymous rest、多重代入、パターンマッチ（`case/in`）、`method_missing`・`respond_to_missing?`、
  `define_method`、`Binding`/`eval(str, binding)`/`TOPLEVEL_BINDING`、`super`（prepend MRO 線形化含む）、
  例外 + `backtrace`/`caller`、`defined?`、`freeze`/`FrozenError`。
- **正規表現**: `libastrogre` ベースの本物の Regexp/MatchData。`=~`/`$1..$9`/`$~`/`$&`/`` $` ``/`$'`、
  scan/match/split/sub/gsub（`\1` backref + block）、名前付きキャプチャ、lookahead/lookbehind、
  in-pattern backref、POSIX class、`/i`・`/m` フラグ、`Regexp.union`、補間 `/#{}/`。
- **数値**: fixnum tagged 算術、bignum（GMP）、Rational、Complex、CRuby 互換 MT19937（seeded 完全一致）。
- **core クラス**: Array/Hash/String/Symbol/Range/Struct/Data/Set/Comparable/Enumerable
  （lazy Enumerator 一部）/Time（sub-second）/pack・unpack/sprintf。
- **メタプロ**: Module#include/prepend/extend、`class << self` 一般 body、class instance variable、
  const_set/alias_method/undef_method、builtin subclass/extend（side-table 方式）。
- **stdlib**: ENV/ARGV/File/Dir/IO/StringIO/Marshal（一部）、IO::Buffer、IO.copy_stream、autoload。
- **Thread / Fiber**: green thread M:1（scheduler + blop 層。Thread/Mutex/Queue/ConditionVariable/
  IO.select）、Fiber の resume/yield/raise/transfer/kill/storage。

## 意図的な除外（現時点でスコープ外）

- **encoding / transcoding**: 3-bit tag で UTF-8 / US-ASCII / BINARY のみ。任意 encoding・変換・
  Unicode case mapping は非対応（[docs 参照](./docs/) と `project_koruby_precise_encoding`）。
  ※正規表現そのものは対応済み。encoding 依存の regex テストのみがこの境界。
- **真の並行性**: Thread は同期実行モデル。Ractor / Fiber scheduler なし。
- **gem エコシステム**: 外部 gem / native 拡張のロード、`require` のパス解決、autoload は非対応。

## ビルド・テスト・ベンチ

```sh
make                       # koruby_precise を直接ビルド（GC=copy moving default）
                           # prism は ../naruby/prism を symlink（vendored, untracked）
make test                  # rubyharness 差分テスト（CRuby オラクル, 93,399 件）
                           #   CAT=<category> / STRESS=1 で絞り込み・GC stress
make bench                 # 多モード bench（interp / aot+compile / aot+cached / cruby+yjit）
make optcarrot             # optcarrot をインタプリタで（--plain, FRAMES= 指定可）
make optcarrot-aot         # optcarrot を AOT で（bake → --compiled-only の fps）

ruby tools/mspec_real_run.rb <dir> [jobs]   # rubyspec 充足率（本物の mspec。DUMP=path は ENV で）
ruby tools/rubyspec_run.rb <dir>   # 同（shim 版・速いが pass 水増し。DUMP=path WORST=1 で詳細）
ruby tools/runspec1.rb <spec>      # 単一 spec の pass/fail 詳細
```

GC 健全性チェック: `BARUBY_GC_STRESS=1 BARUBY_GC_PURGE=1 ./koruby_precise prog.rb`
（alloc 跨ぎの stale pointer を毎 alloc GC + mprotect で炙り出す）。

## ドキュメント

- 設計: [docs/v2_design.md](./docs/v2_design.md) / block 設計 [docs/v2_blocks_design.md](./docs/v2_blocks_design.md)
- 仕様（CLI / AOT / スコープ / gate）: [docs/v2_spec.md](./docs/v2_spec.md)
- rubyspec 充足の記録: [docs/rubyspec.md](./docs/rubyspec.md)
- 性能改善の記録（成功・見送り両方）: [docs/perf.md](./docs/perf.md)
- GC / rooting: [docs/rooting_guide.md](./docs/rooting_guide.md) / [docs/gc_comparison.md](./docs/gc_comparison.md)
- closure/sp モデル: [docs/closure_sp_model.md](./docs/closure_sp_model.md)
- 実装済み機能の履歴: [docs/done.md](./docs/done.md) / 残作業: [docs/todo.md](./docs/todo.md)
- 上位フレームワーク: [../../docs/idea.md](../../docs/idea.md)（ASTro 設計思想）/
  [../../docs/usage.md](../../docs/usage.md)（新サンプルの書き方）
- テスト・ベンチ基盤: [../rubyharness/](../rubyharness/)
