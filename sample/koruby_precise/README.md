# koruby_precise

Ruby サブセットの言語実装。**ASTro フレームワーク**（AST を辿るインタプリタの部分評価による高速化）の上に、
**precise rooting + moving/copy GC** で構築されている。CRuby drop-in を目標に、core 言語・core クラス・
正規表現・stdlib の広範囲をカバーし、実物の **optcarrot（NES エミュレータ）を CRuby と checksum 完全一致**で動かす。

> v1（2026-06 以前）は slots ABI で全面再構築（v2）した。以降 M0（calc 級 subset）から出発し、
> 現在は下記の通り実用 core Ruby 処理系まで到達している。設計は [docs/v2_design.md](./docs/v2_design.md)、
> CLI/AOT/gate 仕様は [docs/v2_spec.md](./docs/v2_spec.md)。

## 実行モデル

同じ AST から2通りに実行できる（ASTro の核心）:

- **インタプリタ**（tree-walk, `--plain`）— コード生成なしの共通エンジン。全 125 個の
  `DISPATCH_node_*` を合計しても **~34 KB**（平均 275 B/ノード）と極小。
- **AOT**（`--aot-compile` → `--compiled-only`）— 各ノードを部分評価して特殊化ディスパッチャ（SD）を C に吐き、
  `code_store/all.so` として dlopen。プログラムごとに特殊化コードを生成する。

GC は **precise moving/copy GC**（`GC=copy` default）。全ての alloc-heavy path を
`BARUBY_GC_STRESS=1 BARUBY_GC_PURGE=1`（毎 alloc で GC + retired plane を mprotect）で検証している。

## 性能（optcarrot, 180 フレーム, checksum 59662 全モード一致）

2026-09-05、専用機 sp4（Ryzen 9 8945HS / 8 cores, 16 threads、Linux 7.0、
performance governor、gcc 15.2、他ジョブなし）で計測。比較対象は自前ビルドの
**CRuby 4.0.2 +PRISM**（`v4.0.2`, revision `d3da9fec82`）。master と修正版を
1 round 内で交互に、各 3 round の median:

| 実行系 | fps | 対 素の CRuby | 対 CRuby+YJIT |
|---|---:|---:|---:|
| **koruby AOT**（aot+cached） | **96.4** (96.3–96.9) | **1.54×** | 0.32× |
| CRuby (no yjit) | 62.5 (62.3–62.5) | 1.00× | 0.21× |
| CRuby + YJIT | 297.6 (294.2–298.0) | 4.76× | 1.00× |

- optcarrot では **warm AOT が素の CRuby の 1.54×**。YJIT は AOT の 3.09×。
- **`node_vcall` のラッパノードを外して 81.7 → 96.4 fps (+18.0%)**（2026-09-05）。
  裸の識別子 `foo` の miss を NameError にするために call ノードを `@noinline` の
  ラッパで包んでいたのを、パーサが `NodeHead` に印を付けて miss 側だけが読む形に変えた。
  成功時に dispatcher が 1 段増えるのが実測で効いていた。経緯と切り分けは
  `~/ruby/src/trials/2026-09-05-koruby-optcarrot-regression/`。
- 修正前の 2026-09-04 計測（同じ sp4、CRuby 4.0.6 比較、各 7 回 median）は
  AOT **81.3** (80.5–82.2) / interp **51.9** (51.6–52.9) / CRuby 63.1 / YJIT 300.1。
  interp と cold bake は修正後に測り直していない（上の表は AOT のみ 09-05 の値）。
- 標準の `make optcarrot-report FRAMES=180 BENCHRUNS=3` では AOT 80.9 fps、
  AOT cold（bake + 1 run）35.738 s、warm run 2.321 s（いずれも vcall 修正前の値）。
- optcarrot の AOT bake 単体は **33.350 s**。Ruby ソース42ファイル合計 **233,406 bytes**、
  bundle **134,350 bytes**、生成 `code_store/all.so` **3,252,600 bytes**。
- 純 Ruby DOOM（E1M1、60フレーム）は CRuby 1.095 s / YJIT 0.529 s / koruby interpreter
  0.991 s / AOT warm 0.503 s（checksum `17930386881013214317` 全モード一致）。AOT bake は **11.275 s**。
- 53 本の microbench（各5モード、best-of-3、`GC=copy`）の実時間 geomean は、素の CRuby = 1.00 に対し
  **YJIT 0.49 / interp 0.74 / AOT cold 1.04 / AOT warm 0.37**。warm AOT は素の
  CRuby の 2.70×、YJIT の 1.32×で、YJIT に 32/53 ベンチで勝つ。再帰・method send・
  ivar/object 系は YJIT が強い。
- 同じ条件で `GC=copy_gen` も測定した（geomean: AOT warm 0.38）。`ary` / `gen_gc` は改善したが、
  `gcchurn` / `strscan` は copy より約9%遅く、効果は workload 依存だった。
- `GC=mark_gen` も同条件で測定し、AOT warm geomean は CRuby 比 0.42（YJIT 比 約0.86）。
  さらに `bump` / `immix` / `immix_gen` / `mark_compact_gen` / `mark_bump_gen` も同条件で測定した。
  8 backend・全53本の CRuby/YJIT 比較と各 raw log は [docs/perf.md](./docs/perf.md) に記載した。
- 比は CPU、コンパイラ、CRuby/YJIT バージョンで大きく動く。数字を引用するときは
  比較対象も同じ環境で測り直すこと。詳細と過去値は [docs/perf.md](./docs/perf.md)。
- **計測前に optcarrot 配下の `code_store` を消すこと。** 古い store を新しい binary で
  読むと素の実行は SEGV、`--compiled-only` は compile-miss で即死する。

## rubyspec 充足（core, CRuby drop-in 目標）

2026-09-04 時点（koruby_precise `0a908f2a`、rubyspec `4eaa9145`、`spec/ruby/core`）。
計測は **本物の mspec を無改造の spec に噛ませる** `tools/mspec_real_run.rb`
（`DUMP=core.tsv ruby tools/mspec_real_run.rb ~/ruby/src/master/spec/ruby/core 12`）。

| 指標 | 値 |
|---|---:|
| **example pass-rate** | **97.0%** |
| examples | 22,910 (pass 22,215 / fail 523 / err 172) |
| fully-clean files | 1,633 / 2,144 (76.2%) |
| 完走しない files | 4（CRuby 上で計 45 examples） |
| 保守的下限（未完走を全て不合格として算入） | 96.8% |

- pass-rate は完走した spec の `pass / (pass + fail + err)`。
- 2 回連続実行での差は 1 example のみ（`file/open` が pass↔err で振れる）。
  ただし per-file 20s timeout があるので、**測る前にマシンの負荷を確認すること**
  （過去 sweep の残骸プロセスを落としてから測り直したら、未完走 files が 7 → 4 になった。
  残骸の件は [docs/todo.md](./docs/todo.md) の「sweep が fork した子プロセスを残す」）。
- 単一 spec の再現は
  `SPEC_TEMP_DIR=<writable> ./koruby_precise tools/mspec_launch.rb <absolute-spec-path>`。
- `tools/rubyspec_run.rb`（mspec **shim** + spec 連結方式）は速いが `it_behaves_like` や
  mock が独自実装なので **pass を水増しする**。数字を出すときは実 mspec のほうを使うこと。

カテゴリ別（summary が返った examples）:

| category | pass / total | category | pass / total | category | pass / total |
|---|---:|---|---:|---|---:|
| `argf` | 127/139 (91.4%) | `array` | 2,884/2,898 (99.5%) | `basicobject` | 160/172 (93.0%) |
| `binding` | 85/98 (86.7%) | `builtin_constants` | 27/27 (100.0%) | `class` | 50/54 (92.6%) |
| `comparable` | 54/54 (100.0%) | `complex` | 166/169 (98.2%) | `conditionvariable` | 9/11 (81.8%) |
| `data` | 85/88 (96.6%) | `dir` | 325/333 (97.6%) | `encoding` | 618/632 (97.8%) |
| `enumerable` | 532/536 (99.3%) | `enumerator` | 421/431 (97.7%) | `env` | 172/193 (89.1%) |
| `exception` | 215/250 (86.0%) | `false` | 12/13 (92.3%) | `fiber` | 161/171 (94.2%) |
| `file` | 899/913 (98.5%) | `filetest` | 85/89 (95.5%) | `float` | 255/260 (98.1%) |
| `gc` | 39/39 (100.0%) | `hash` | 550/562 (97.9%) | `integer` | 585/598 (97.8%) |
| `io` | 1,602/1,634 (98.0%) | `kernel` | 2,110/2,235 (94.4%) | `main` | 23/27 (85.2%) |
| `marshal` | 473/475 (99.6%) | `matchdata` | 180/185 (97.3%) | `math` | 243/243 (100.0%) |
| `method` | 194/199 (97.5%) | `module` | 1,021/1,049 (97.3%) | `mutex` | 28/29 (96.6%) |
| `nil` | 26/27 (96.3%) | `numeric` | 325/326 (99.7%) | `objectspace` | 107/107 (100.0%) |
| `proc` | 235/247 (95.1%) | `process` | 339/363 (93.4%) | `queue` | 46/46 (100.0%) |
| `random` | 85/87 (97.7%) | `range` | 606/606 (100.0%) | `rational` | 155/158 (98.1%) |
| `refinement` | 24/25 (96.0%) | `regexp` | 249/258 (96.5%) | `set` | 165/175 (94.3%) |
| `signal` | 52/53 (98.1%) | `sizedqueue` | 59/61 (96.7%) | `string` | 3,846/3,906 (98.5%) |
| `struct` | 170/170 (100.0%) | `symbol` | 262/270 (97.0%) | `systemexit` | 6/6 (100.0%) |
| `thread` | 261/327 (79.8%) | `threadgroup` | 8/8 (100.0%) | `time` | 629/652 (96.5%) |
| `tracepoint` | 29/76 (38.2%) | `true` | 12/13 (92.3%) | `unboundmethod` | 100/106 (94.3%) |
| `warning` | 29/31 (93.5%) | — | — | — | — |

主な残課題:

| 残課題 | 内容 |
|---|---|
| TracePoint のイベント配信 | API 層はあるが `:line` / `:call` / `:return` / `:b_call` などの実行時フックが未実装 |
| 実行中フレームの観測 | `caller` / `caller_locations` / `Thread#backtrace` と location 系、例外 backtrace / `full_message` / トップレベル表示の一部が不完全 |
| Thread/Fiber の割り込みと同期 | Fiber 内で lock 中の thread への `raise` / `kill` など、green-thread モデルと CRuby native thread の意味差 |
| eval/Binding の字句スコープ | `eval(str)` から呼び出し元・外側 block の local を読み書きするケース、Binding の外側 scope 列挙 |
| 完走しない 4 files | `mutex/lock`、`process/kill`、`process/status/wait`、`process/wait`。Fiber 内 lock への割り込みと signal/wait の wakeup がハング経路。さらに `process/wait` 系は fork した子が `timeout` の kill を生き延びて残留するので、sweep 後にプロセスを確認すること |
| 個別 edge case | ENV の変換プロトコル、Encoding::Converter / Unicode、String、Process/IO など |

失敗の履歴と設計上の制約は [docs/rubyspec.md](./docs/rubyspec.md) と
[docs/todo.md](./docs/todo.md) に記録している。

corpus（`make test`）は CRuby オラクル差分の golden test **100,354 件を 0 fail / 0 crash** で維持している。

## 実装済み機能（抜粋・すべて CRuby 一致を確認）

- **言語**: closure/proc/lambda/`->`、block・`yield`・`&blk`・`Symbol#to_proc`、splat `f(*a,&p){}`・
  `**kwargs`・anonymous rest、多重代入、パターンマッチ（`case/in`）、`method_missing`・`respond_to_missing?`、
  `define_method`、`Binding`/`eval(str, binding)`/`TOPLEVEL_BINDING`、`super`（prepend MRO 線形化含む）、
  例外 + `backtrace`/`caller`、`defined?`、`freeze`/`FrozenError`。
- **正規表現**: `libastrogre` ベースの本物の Regexp/MatchData。`=~`/`$1..$9`/`$~`/`$&`/`` $` ``/`$'`、
  scan/match/split/sub/gsub（`\1` backref + block）、名前付きキャプチャ（`MatchData#[:name]` /
  `#named_captures` / `Regexp#names`。ただし **`/(?<n>…)/ =~ str` がローカル変数 `n` を
  作る形は未対応** — prism の MATCH_WRITE ノードが未実装）、lookahead/lookbehind、
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

- **encoding / transcoding**: String header の 3-bit フィールドで encoding を持つ。
  **直接扱えるのは UTF-8 / US-ASCII / ASCII-8BIT の 3 つ**で、それ以外は「名前だけ覚える」
  （`force_encoding("ISO-8859-1")` は通り `#encoding` も返すが、文字単位の操作は
  NotImplementedError）。`String#encode` の実 transcoding と Unicode case mapping は非対応
  （`project_koruby_precise_encoding`）。※正規表現そのものは対応済みで、encoding 依存の
  regex テストだけがこの境界。
- **真の並行性**: Thread は **green thread M:1**（native 1 本の上で協調スケジューリング。
  blocking 操作でのみ切り替わり、preemption なし）。**真の並列実行はしない**。
  Ractor と Fiber scheduler は無し（`Fiber.scheduler` は nil を返すスタブ）。
- **gem エコシステム**: 外部 gem / native 拡張のロードは非対応。
  （`require` の `$LOAD_PATH` 解決と `autoload` は実装済み — [done.md](./docs/done.md)）

## ビルド・テスト・ベンチ

```sh
make                       # koruby_precise を直接ビルド（GC=copy moving default）
                           # prism は ../naruby/prism を symlink（vendored, untracked）
make test                  # rubyharness 差分テスト（CRuby オラクル, 100,354 件）
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
