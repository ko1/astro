# rubyharness — ASTro Ruby サンプル共有テスト+ベンチ基盤

CRuby を正解(オラクル)にした差分テストと、実行モード横断ベンチを、
ASTro の Ruby 系サンプル(naruby / baruby / koruby_precise …)で**共有**する。

## 構成

```
tools/   run_specs.rb         差分テストドライバ(1ファイル=1プロセス, crash recovery)
         run_bench.rb         多モードベンチドライバ
         gen_golden.rb        メソッド表面の golden を生成 → t/method
         gen_syntax.rb        構文組合せの golden を生成 → t/syntax
         gen_from_rubyspec.rb ruby/spec から自己完結な式を mining → t/spec
t/       hand/                手書き機能テスト
         syntax/hand_*.rb     手書き構文テスト
         method/ syntax/ spec/  生成物(.gitignore、make gen で再生成)
         README.md            テスト/ベンチの詳しい使い方
bench/   *.rb                 ~1s scale のマイクロベンチ
harness.mk                    make include(gen/test/bench を INTERP でパラメータ化)
```

## サンプルからの利用

各サンプルの Makefile に3行:

```makefile
INTERP ?= ./mysample
include ../rubyharness/harness.mk
test bench: mysample          # バイナリをビルドしてから走らせる
```

これで:

```sh
make gen                      # コーパス生成(共有、初回のみ)
make test                     # 全コーパスを mysample で実行・CRuby 差分
make test CAT=array           # 領域を絞る(開発ループ)
make test STRESS=1            # GC ストレス下(GC-safety バグ炙り出し)
make test INTERP=ruby         # ハーネス自己チェック(全 PASS のはず)
make bench                    # cruby/cruby+yjit/interp/aot+compile/aot+cached
make clean-corpus             # 生成物 + code_store を消す
```

同一コーパスを各サンプルに当てれば、**サンプル横断で「Ruby のどこまで」「速度」**
を比較できる(subset 差がそのまま PASS 率差に出る)。`code_store` は各サンプルの
cwd に作られるので AOT は per-sample。

詳細は [`t/README.md`](t/README.md)。

## DOOM アプリベンチ (clone-on-demand)

[khasinski/doom](https://github.com/khasinski/doom) (純 Ruby の DOOM エンジン) を
optcarrot 同様の**アプリ規模ベンチ**として使う。ソースと WAD は**リポジトリに入れず**
`apps/doom/` に clone する(`.gitignore` 済)。`tools/doom.sh` が全ソースを 1 ファイルに
bundle し(koruby は `require` 非対応)、固定視点で N フレーム headless render して
フレームバッファの checksum を出す。CRuby と各サンプルで checksum が一致すれば正しい
(タイミングはハーネス/利用側が外から計測)。

```sh
sh tools/doom_setup.sh         # 初回: khasinski/doom + shareware WAD を取得
make doom                      # サンプルのツリーウォーカで render → checksum
make doom-aot                  # --aot-compile して --compiled-only で render
make doom DOOM_MODE=cruby      # オラクル(CRuby)の checksum
make doom FRAMES=120           # フレーム数を増やす(sustained 計測用)
```

koruby_precise では plain / AOT / CRuby すべて checksum 一致 (E1M1, 320×240)。
実測 (120f): CRuby 4.95s / CRuby+YJIT 2.56s / koruby AOT 3.24s。

## ruby/ruby-bench micros (clone-on-demand)

[ruby/ruby-bench](https://github.com/ruby/ruby-bench) (= yjit-bench) の**単一ファイル
micro** を取り込む。ソースは**リポジトリに入れず** `apps/ruby-bench/` に clone
(`.gitignore` 済)。各 micro は `run_benchmark(n){ … }` を呼ぶだけなので、koruby は
`require` 非対応 → `tools/rubybench.sh` が `run_benchmark` shim (block を BENCH_ITRS
回まわして結果を `p`) を前置 + `require_relative` を除去して bundle し、結果を出す
(CRuby と一致すれば正しい、時間は外側で計測)。

```sh
sh tools/rubybench_setup.sh                 # 初回: ruby/ruby-bench を clone
make rubybench BENCH=fib                     # 1本を走らせる(tree-walker)
make rubybench BENCH=fib RB_MODE=aot         # AOT
make rubybench BENCH=nqueens RB_MODE=cruby-yjit  # 参照(YJIT)
make rubybench BENCH=matmul BENCH_ITRS=20    # 反復数を増やす(sustained 計測)
make rubybench-all                           # 全 micro を CRuby と差分(正当性 sweep)
```

koruby_precise: **26/31 micro が CRuby と結果一致**(残 3 = Ractor.make_shareable×2 /
toplevel define_method×1、+2 は Ractor で skip)。app 系ベンチ(rails/graphql/liquid…)
は gem/bundler 依存で対象外。

## rubyboy (Game Boy emulator) アプリベンチ (clone-on-demand)

[sacckey/rubyboy](https://github.com/sacckey/rubyboy)(純 Ruby の Game Boy エミュレータ)を
optcarrot / doom 同様の**アプリ規模ベンチ**として使う。ソースは**リポジトリに入れず**
`apps/rubyboy/` に clone する(`.gitignore` 済、テスト ROM `tobu.gb` は repo 同梱)。
`tools/rubyboy.sh` が headless エンジン(GUI 依存の sdl/raylib/lcd を除く)を 1 ファイルに
束ねて `EmulatorHeadless` を FRAMES フレーム走らせ、フレームバッファの checksum を出す
(CRuby とサンプルが一致すべき)。

```sh
sh tools/rubyboy_setup.sh      # 初回: sacckey/rubyboy を clone(ROM は同梱)
make rubyboy                   # サンプルのツリーウォーカで step → checksum
make rubyboy-aot               # --aot-compile して --compiled-only で step
make rubyboy RB_MODE=cruby     # オラクル(CRuby)の checksum
make rubyboy FRAMES=200        # フレーム数を増やす(sustained 計測用)
```

koruby_precise: **plain / AOT とも CRuby とピクセル完全一致**(60f checksum
`4747678158831331132`)。マルチファイル `require` を使う実アプリで、`require` 先の
クラスメソッド(`Cartridge::Factory.create`)が AOT でも解決される。

## rubykon (Go/囲碁 MCTS AI) アプリベンチ (clone-on-demand)

[PragTob/rubykon](https://github.com/PragTob/rubykon)(純 Ruby の Monte-Carlo 木探索
囲碁 AI、`ruby/ruby-bench` 同梱)を計算重めのアプリベンチとして使う。`tools/rubykon.sh`
が seed 固定の MCTS バッチを**マルチファイル `require` のまま**(bundle せず=require-AOT
経路を通す)走らせ、選ばれた best move の FNV checksum を出す(CRuby とサンプルが一致すべき)。

```sh
make rubykon                   # ツリーウォーカで MCTS → checksum
make rubykon-aot               # --aot-compile --run で discover+bake → --compiled-only
make rubykon RB_MODE=cruby     # オラクル(CRuby)
make rubykon GAMES=40 ITERS=200  # ワークロードを増やす(sustained 計測)
```

koruby_precise: **plain / AOT / CRuby で checksum 完全一致**(`687797821343675504` @ GAMES=3 ITERS=60)。
seed 固定 MCTS なので決定的。既定 run_benchmark は探索木全体(object id 込み)を p するため、
専用ドライバで best move だけ抽出している。

## ruby-json (StringScanner ベース JSON パーサ) アプリベンチ (clone-on-demand)

`ruby/ruby-bench` の "ruby-json"(C 拡張を使わない純 Ruby の JSON パーサ)。`tools/ruby_json.sh`
が `data.json`(public-domain のサッカーデータ)を ITRS 回パースし、結果を JSON.generate で
正規化した checksum を出す(CRuby とサンプルが一致すべき)。koruby の `lib/strscan.rb` +
`lib/json.rb` + ASCII-8BIT 被写体上の Regexp キャプチャを exercise する。

```sh
make ruby-json                 # ツリーウォーカでパース → checksum
make ruby-json-aot             # --aot-compile --run → --compiled-only
make ruby-json RB_MODE=cruby   # オラクル(CRuby, C json 拡張)
make ruby-json ITRS=1000       # 反復数を増やす(sustained 計測)
```

koruby_precise: **plain / AOT / CRuby で checksum 完全一致**(`14207606204983450109` @ ITRS=50)。
data.json のパース結果も CRuby と byte 一致。

## protoboeuf (Protocol Buffers) アプリベンチ (clone-on-demand)

`ruby/ruby-bench` の "protoboeuf"(C 拡張を使わない生成済み純 Ruby protobuf codec)。
`tools/protoboeuf.sh` が Marshal で固定メッセージ集合をロード→デコード→ITRS 回リエンコードし、
エンコード結果の checksum(`String#sum(64)`)を出す(CRuby とサンプルが decode/encode とも
byte 一致すべき)。koruby の `Marshal.load` + `Array#pack(buffer:)` + ASCII-8BIT 上の
`String#<<(int)` を exercise する。

```sh
make protoboeuf                # デコード→リエンコード → checksum
make protoboeuf-aot            # --aot-compile --run → --compiled-only
make protoboeuf RB_MODE=cruby  # オラクル(CRuby)
make protoboeuf ITRS=100       # 反復数を増やす(sustained 計測)
```

koruby_precise: **plain / AOT / CRuby で checksum 完全一致**(`4011150550` @ ITRS=10)。
decode / encode とも CRuby と byte 完全一致。

## etanni (テンプレートエンジン) アプリベンチ (clone-on-demand)

`ruby/ruby-bench` の "etanni"(`eval` で Proc にコンパイルする ERB 風テンプレートを、
JSON ロードした gem-server データに対して描画)。`tools/etanni.sh` が固定テンプレートを
ITRS 回描画し、出力の checksum(`String#sum(64)`)を出す(CRuby と byte 一致すべき)。
`eval`→Proc + `instance_eval` + heredoc テンプレート + koruby の `lib/json.rb`
(343KB の gem_specs.json を JSON.load)を exercise する。

```sh
make etanni                # テンプレート描画 → checksum
make etanni-aot            # --aot-compile --run → --compiled-only
make etanni RB_MODE=cruby  # オラクル(CRuby)
make etanni ITRS=500       # 反復数を増やす(sustained 計測)
```

koruby_precise: **plain / AOT / CRuby で checksum 完全一致**(`13803342` @ 175817 bytes)。

## json-parse (JSON パース throughput) ベンチ (clone-on-demand)

`ruby/ruby-bench` の json_parse_float 系(Ractor ハーネスは外す)。ELEMENTS 個の
float 配列 JSON を生成→パースし、パース後 float を **IEEE ビット**(`pack("E*").sum(64)`)で
checksum する(CRuby と一致すべき)。ビットで比べるのは、CRuby の C json 拡張が float を
`Float#to_s`(最短往復、koruby の `lib/json.rb` が使う)より桁多く出力するため — 文字列だと
食い違うが、両者とも同じ double にデコードするのでビットは一致する。`lib/json.rb` の Float
パースを大量に exercise する。

```sh
make json-parse                # 生成+パース → checksum
make json-parse-aot            # --aot-compile --run → --compiled-only
make json-parse RB_MODE=cruby  # オラクル(CRuby, C json 拡張)
make json-parse ELEMENTS=100000  # 件数を増やす(sustained 計測)
```

koruby_precise: **plain / AOT / CRuby で checksum 完全一致**(`62505125` @ ELEMENTS=3000)。
