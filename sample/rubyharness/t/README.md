# テスト

`sample/rubyharness/` は ASTro の **Ruby 系サンプル共有**のテスト+ベンチ基盤。
各サンプルの Makefile から取り込み、`INTERP` だけ差し替えて使う:

```makefile
INTERP ?= ./mysample
include ../rubyharness/harness.mk
test bench: mysample            # バイナリをビルドしてから実行
```

同じコーパスを naruby / baruby / koruby_precise … に当てられ、サンプル横断で
「Ruby のどこまで」「速度」を比較できる(subset 差が PASS 率差に出る)。コーパス
生成・実行は共有、`code_store` は各サンプルの cwd に作られるので AOT は per-sample。

CRuby を正解(オラクル)にした差分テスト。ドライバ `tools/run_specs.rb` は
**1ファイル=1プロセス**で隔離実行し PASS/FAIL/ERR/CRASH/TIMEOUT を集計する
(若いインタプリタは SEGV/ハングするので隔離が必須)。ツールは全部 CRuby で動く `.rb`。

## コーパスの構成

| dir | 内容 | 由来 |
|---|---|---|
| `t/hand` | 機能テスト(literals/array/class/exception …) | 手書き |
| `t/method`  | 各 core クラスのメソッド表面(~87k) | `gen` で生成 |
| `t/syntax`  | 構文の組み合わせ(call/演算子/代入/リテラル/引数結合/pattern …) | `gen` で生成(+手書き数本) |
| `t/spec`    | ruby/spec の `expr.should ==` から抽出した自己完結な式 | `gen` が rubyspec から mining |

`t/spec` は `tools/gen_from_rubyspec.rb` が ruby/spec を走査し、setup/mock 非依存の
式だけを CRuby で検証して golden 化したもの(rubyspec が「どの挙動を対象にすべきか」
を駆動)。spec のある場所は `make gen RUBYSPEC=path/to/core` で指定。

## make ターゲット

ターゲットは **2つだけ**:

```sh
make gen     # コーパスを生成(初回 / 生成器を直したとき。t/method, t/syntax を書く)
make test    # コーパスを実行し CRuby と差分
```

`test` の挙動は**修飾変数**で変える(覚えるのは test 一つ):

| やりたいこと | コマンド |
|---|---|
| 全件・網羅(コミット前 / CI) | `make test` |
| 機能 X を集中(=速い、開発ループ) | `make test CAT=X`(例 `CAT=string` `CAT=operator` `CAT=exception`) |
| GC バグ炙り出し | `make test STRESS=1`(毎 alloc で GC + PURGE) |
| ハーネス自己チェック | `make test INTERP=ruby`(CRuby vs CRuby → 全 PASS のはず) |
| 別 GC backend でビルドして実行 | `make test GC=mark` |

修飾は組み合わせ可: `make test CAT=array STRESS=1 GC=copy JOBS=4`。

`CAT=<area>` は `t/**/<area>*.rb` を拾う(hand+method+syntax 横断)。area 名は
category(`array` `string` `integer` `hash` `range` `float` `symbol` / 構文の
`call` `operator` `assign` `literal` `pattern` `args` `block` / 手書きの
`class` `module` `method` `exception` …)。

(rubyspec モードはドライバの `--runner` で対応予定。mspec シムができたら
`make test` の一モードとして戻します。)

## ベンチマーク

`bench/*.rb`(~1s scale)を**実行モード横断**で計測。`make bench`:

```sh
make bench                        # cruby / cruby+yjit / interp / aot+compile / aot+cached
make bench BENCHRUNS=3            # best-of-3(既定 5)
make bench BENCHMODES=interp,aot+cached
make bench STRESS=1               # koruby 系を GC ストレス下で
```

モード:

| mode | 意味 |
|---|---|
| `cruby` / `cruby+yjit` | 基準(`ruby --yjit-disable` / `--yjit`) |
| `interp` | code_store 消去、純インタプリタ |
| `aot+compile` | **コールド**: `--aot-compile` のビルド時間を**含めた**1回実行 |
| `aot+cached` | **ウォーム**: ビルドは untimed、キャッシュ実行のみ計測 |
| `pg+cached` | PG(`--pg-compile`)。実装サンプル用(koruby は N/A) |

- best-of-N の min(ノイズ最小)、各セルは CRuby と stdout 比較(不一致は **MISMATCH**)、geomean は CRuby 比。
- `aot+cached` は `ASTRO_AOT_STRICT=1` で実行 — **interp fallback したら非ゼロ終了でセルが `INTERP!`** になる規約(純 AOT 検査)。**koruby は本フラグ未対応**なので、現状この検査は効かず、数値に silent な interp fallback(~31%)が混じりうる点に注意(新インタプリタで対応予定)。

`gen` だけが生成、`test*` は実行のみ。被テストの差し替えは `INTERP=` で:

```sh
make test INTERP=./koruby_precise        # 既定
make test-hand INTERP="ruby --yjit"      # 任意のコマンド可
make test JOBS=4                          # 並列数
```

## GC の指定(2軸)

```sh
# (1) 実行時 GC ストレス: 毎 alloc で GC + PURGE。held-across-GC バグを炙り出す本命。
make test-hand STRESS=1          # 被テストを env ASTRO_GC_STRESS=1 ASTRO_GC_PURGE=1 で実行
make test STRESS=1               # 全コーパスを GC ストレスで(遅い、長時間 soak)
#   STRESS=1 のとき timeout は自動で 120s に。env 名は GC_STRESS_ENV var で変更可。

# (2) ビルド時 GC backend(precise_gc の 14 種を build-time switch、既存の GC= var)
make GC=copy        test-hand    # 既定(コピー GC)
make GC=mark        test-hand
make GC=immix       test-hand STRESS=1   # backend 選択 × ストレスの組合せも可
#   backend 一覧は Makefile の GC_NUM_* 参照(none/mark/copy/immix/bump/…)。
```

STRESS=1 は被テストにだけ付き、CRuby オラクルは素のまま。87k 全件を STRESS で回すと
非常に遅いので、通常は `test-hand STRESS=1`(小)で日常チェック、全件は soak 用。

## 出力の読み方

- カテゴリ別表 + 末尾の **CRASH / TIMEOUT / ERR / FAIL リスト = 直す TODO**。
- 分類: PASS=CRuby と stdout 一致 / FAIL=不一致 / CRASH=signal(SEGV 等, 終了 ≥128)/
  TIMEOUT=時間切れ / ERR=(rubyspec モードのみ)trailer 無し。
- 非PASS があれば exit 1(CI 連携可)。
- カテゴリ別 **CRASH→FAIL→PASS の推移**が「Ruby のどこまで実装したか」。
- 単体再現(デバッグ): `./koruby_precise t/method/array_007.rb` を直接 / gdb で。

## 直接ドライバを叩く場合(make を介さない)

```sh
ruby tools/run_specs.rb --interp ./koruby_precise --diff ruby --dir t --pattern '*.rb' --jobs 8
```

| オプション | 既定 | 意味 |
|---|---|---|
| `--interp CMD` | (必須) | 被テスト(複数語可) |
| `--dir DIR` | (必須) | 再帰スキャン対象 |
| `--diff REF` | なし | stdout を REF と比較(差分モード) |
| `--runner FILE` | なし | rubyspec モード: spec の前に load(mspec シム) |
| `--timeout SEC` | 15 | 1ファイルの壁時計上限 |
| `--jobs N` | nproc | 並列プロセス数 |
| `--pattern GLOB` | diff=`*.rb` / runner=`*_spec.rb` | 対象 glob |
