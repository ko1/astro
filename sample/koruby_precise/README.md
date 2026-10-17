# koruby_precise

**ASTro**（AST を辿るインタプリタの部分評価で速くするフレームワーク）の上に作った Ruby 処理系。
**precise rooting + moving/copy GC**。目標は CRuby の drop-in で、CRuby の rubygems・bundler・stdlib を
そのまま読み、**Rails 8.1 のアプリと ruby-bench の 79 本（railsbench・lobsters・optcarrot ほか）が動く**。

> v1（2026-06 以前）は slots ABI で全面的に作り直した（v2）。設計は [docs/v2_design.md](./docs/v2_design.md)、
> CLI / AOT / gate の仕様は [docs/v2_spec.md](./docs/v2_spec.md)、いまの状況の一覧は [docs/status.html](./docs/status.html)。

## 実行モデル

同じ AST を 2 通りに実行できる（ASTro の核心）:

- **インタプリタ**（tree-walk、`--plain`）— コード生成なしの共通エンジン。
- **AOT**（`--aot-compile` / `--pg-compile` で焼く → 以後の実行で使う）— 各ノードを部分評価して特殊化した
  ディスパッチャ（SD）を C で吐き、`code_store/all.so` として dlopen する。prelude（Ruby で書いた組み込み）は
  `preload_store/` に 1 度だけ焼いて全プログラムで共有する。
- **WASI**（wasm32-wasip1）: インタプリタと、AOT を全部埋め込んだ単体 .wasm（[wasi/README.md](./wasi/README.md)）。
  ※ 2026-10-09 以降の `<execinfo.h>` の include で WASI ビルドが止まっている（[docs/todo.md](./docs/todo.md)）。

GC は **precise moving/copy GC**（`GC=copy` が既定）。割り当ての多い経路は
`BARUBY_GC_STRESS=64 BARUBY_GC_PURGE=1`（64 回の割り当てごとに GC + 旧領域を mprotect）で検証している。

## 何が動くか（2026-10 時点）

| 対象 | 状況 |
|---|---|
| Rails 8.1 | `rails new` / `bundle install` / scaffold / `db:migrate` / ActiveRecord（sqlite3）/ 画面・JSON・開発時のエラー画面が CRuby と同じ（2026-09-29） |
| ruby-bench（Shopify/ruby-bench の既定 79 本） | 79 本すべて動く（railsbench・lobsters・activerecord・erubi-rails・liquid・hexapdf・psych・optcarrot ほか）。性能は下の表 |
| gem のテストスイート | 342 gem・7,068 ファイルを CRuby と結果比較: 6,888 ファイル一致、313 gem は全ファイル一致（2026-09-28〜29、表は [docs/status.html](./docs/status.html)） |
| C 拡張 | CRuby の C API の互換層（builtins/cext.c）。`gem install` が mkmf で拡張を `.kompat.so` にビルドする（拡張のソースは無改造）。nokogiri・sqlite3・puma・json・bigdecimal・psych・msgpack・bcrypt・nio4r・openssl・digest ほか。ビルドできない拡張は抜いて入れ、koruby の `lib/` が代わりに立つ |
| CRuby の test/rubygems | 162 ファイル・3,541 テストを 1 ファイルずつ比較し、koruby だけが落ちるのは 3 件（いずれも意図した違い、2026-10-17）。`tools/cruby_tests.sh rubygems` |
| gem / bundle コマンド | `root/bin/gem`・`bundle`・`rake` ほか（shebang は koruby）。rubygems.org からの `gem install` も動く |
| optcarrot / DOOM | NES エミュレータ optcarrot と純 Ruby の DOOM が CRuby と checksum（画素）一致 |

## 互換性の数字

| 指標 | 値 | 計測 |
|---|---|---|
| ruby/spec（core / language / library、3,747 ファイル） | **32,363 / 32,364 例 = 99.997%**（TracePoint の 76 例を除く。含めると 32,438 / 32,440） | 2026-10-17、20b36a9c、本物の mspec で 1 ファイルずつ（`tools/spec_sweep/sweep.rb`） |
| 手書きの回帰テスト（rubyharness/t、1,220 ファイル） | **17,725 行すべて CRuby master と一致**。GC 負荷試験（STRESS+PURGE）では 41 行が違う（2026-10-16 の作業前の build 1743b846 でも同じ 41 行） | 2026-10-17 / 10-16 |
| CRuby の test/rubygems | koruby だけが落ちるテスト 3 / 3,541 | 2026-10-17 |

- 残りの 1 例は `Proc#binding`（外側を読まないブロックの binding からメソッドの引数が見えない）。直すとブロックを作るたびに
  +約 270 命令かかるので見送った（[docs/todo.md](./docs/todo.md)）。
- 経過と方法は `~/ruby/src/trials/2026-10-15-koruby-rubyspec/README.md`（2026-10-15 の 98.55% から）。
- 比較の基準は CRuby master（4.1.0dev）。

## 性能

計測はすべて専用機 sp4（Ryzen 9 8945HS、8 コア 16 スレッド、Linux 7.0、performance governor、gcc 15.2、
`bench-lease` で専有）。比較対象は CRuby 4.1.0dev（master `69b49ac7ae`、2026-08-28）+PRISM。
数字は CPU・コンパイラ・CRuby の版で大きく動くので、引用するときは比較対象も同じ環境で測り直すこと。

### optcarrot（NES エミュレータ、180 フレーム、各 5 回の中央値、checksum 59662 全回一致）

2026-10-17、koruby `20b36a9c`（`~/ruby/src/trials/2026-10-17-koruby-status-perf/`）:

| 実行系 | fps | 範囲 | 対 CRuby | 対 YJIT |
|---|---:|---|---:|---:|
| CRuby + ZJIT | 314.1 | 313.2–317.8 | 4.68× | 1.05× |
| CRuby + YJIT | 299.6 | 298.9–301.3 | 4.47× | 1.00× |
| **koruby AOT** | **278.2** | 276.9–279.0 | **4.15×** | **0.93×** |
| koruby interp | 70.8 | 70.0–73.1 | 1.06× | 0.24× |
| CRuby（JIT なし） | 67.0 | 64.1–68.0 | 1.00× | 0.22× |

AOT は先に焼いて（`--aot-compile`）から `--compiled-only` で測る。焼きの時間は含まない。

### ruby-bench（Shopify/ruby-bench、ハーネスの「最後の 10 回の平均」）

| ベンチ | koruby AOT | CRuby | YJIT | 計測 |
|---|---:|---:|---:|---|
| **railsbench**（Rails 8.1 + sqlite3、2000 リクエスト / 反復） | **1032〜1051 ms**（3 巡） | 1239 ms | 682 ms | 2026-10-17、3e7952b3（run 90・91） |
| optcarrot | 725〜739 ms（91 の 2 巡） | 3181 ms | 724 ms | 同上 |
| chunky-png | 167 ms | — | 211 ms | 2026-10-03、e10646b9（run 68） |

- railsbench の AOT は CRuby の 0.85 倍の時間（CRuby より速い）、YJIT の 1.5 倍。10/3 朝の 1948 ms から約 −47%。
- YJIT に勝っているもの（AOT / YJIT の時間比、run 68）: chunky-png 0.79、fannkuchredux 0.47、sudoku 0.87、rubykon 0.91。
  負けているもの: psych-load 1.26、lee 1.25、liquid-render 1.66、liquid-compile 1.68。
- インタプリタ（AOT なし）は 79 本中 23 本で CRuby より速く、68 本が CRuby の 2 倍以内（2026-09-30、754eaa48）。
- ベンチごとの表・方法・経過は `~/ruby/src/trials/2026-09-29-rubybench/README.md`、要約は [docs/status.html](./docs/status.html)。
  CRuby / YJIT は run 51（2026-09-29）の値で、同じ binary なので取り直していない。

### その他

- 純 Ruby の DOOM（E1M1、60 フレーム）は画素の checksum が CRuby と一致（2026-09-28）。
- rubyharness の 53 本の microbench（2026-09-04、0a908f2a）: 実時間の geomean は CRuby = 1.00 に対し
  YJIT 0.49 / interp 0.74 / AOT 0.37（焼きを含めると 1.04）。値と過去の経過は [docs/perf.md](./docs/perf.md)。
- **計測前に対象ディレクトリの `code_store` を消すこと。** 古い store を新しい binary で読むと SEGV か compile-miss になる。

## 範囲外・制限

- **TracePoint**: ベストエフォート（`:line` / `:call` / `:return` / `:b_call` / `:class` / `:raise` / `:script_compiled` ほか）。
  無効時に実行される命令は増えない。AOT の実行中のフレームと `:c_call` / `:c_return` には効かない。rubyspec の数には入れない。
- **並列実行しない**: Thread は green thread（1 本のネイティブスレッドの上で切り替える）。Ractor は API だけで逐次実行。
- RubyVM::InstructionSequence / AbstractSyntaxTree、Fiber scheduler、Process / fork を介する spec は範囲外。
- 一覧は [docs/todo.md](./docs/todo.md) の「§0 範囲外」。

## ビルド・テスト・ベンチ

```sh
make                       # koruby_precise を直接ビルド（GC=copy moving default）
                           # prism は CRuby master と同じ版の submodule（初回に生成・build。docs/v2_spec.md §5.1）
make root                  # root/ を作る: CRuby（CRUBY=、既定 ~/ruby/install/master/bin/ruby）の stdlib・rubygems・
                           # default gem の spec と、bundled gem を koruby 自身の `gem install` で入れたもの。
                           # これがあると koruby は起動時に rubygems を読み（CRuby と同じ）、gem を名前で require できる。
                           # root/bin/gem・bundle・rake …（shebang は koruby）。
                           # C 拡張は mkmf で .kompat.so にビルド（ビルドできないものは拡張抜き）。`--user-install` は ~/.gem/koruby/<ver>。
                           # make test は root に依存する
make test                  # rubyharness 差分テスト（CRuby オラクル。比較先は PATH の ruby なので master の ruby を先に置く）
                           #   CAT=<ファイル名の先頭> / STRESS=1 で絞り込み・GC stress
make bench                 # 多モード bench（interp / aot+compile / aot+cached / cruby+yjit）
make optcarrot             # optcarrot をインタプリタで（--plain, FRAMES= 指定可）
make optcarrot-aot         # optcarrot を AOT で（bake → --compiled-only の fps）

SWEEP_TMP=/tmp/ksw ruby tools/spec_sweep/sweep.rb <jobs> <out.tsv> core language library
                                   # rubyspec（本物の mspec で 1 ファイルずつ。SWEEP_TMP は短いパス）
bash tools/cruby_tests.sh <dir> <outdir> [jobs]   # CRuby の test/<dir>（rubygems / ruby …）を koruby と CRuby で比べる
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
