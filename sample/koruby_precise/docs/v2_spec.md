# koruby v2 仕様 (v2_spec.md)

Status: 初稿 (2026-06-12)。設計は [v2_design.md](./v2_design.md)。
場所は `sample/koruby_precise/` (v1 を削除した跡地に in-place で再構築)。

## 1. 何であるか

- Ruby サブセットの AST インタプリタ。ASTroGen (`lib/astrogen.rb`) +
  `runtime/` の上に、[v2_design.md](./v2_design.md) の slots ABI
  (precise moving GC 前提) で実装する
- パーサは prism (v1 と同じ vendored 構成を再導入)
- バイナリ名は `koruby_precise` (rubyharness の `INTERP ?= ./$(notdir CURDIR))`
  にそのまま乗せるため)
- テスト・ベンチは `sample/rubyharness/` を使う。サンプル内に独自の
  テストコーパスは持たない

## 2. 言語スコープ

最終目標は v1 同等以上 (rubyspec 広範 PASS + optcarrot 動作)。初期スコープは
rubyharness のコーパス区分 (CAT) を単位に下から積む:

| 段階 | 入るもの |
|---|---|
| M0 | リテラル (Integer/Float/String/Symbol/nil/true/false)、算術・比較、
ローカル変数、if/unless/while/until、メソッド定義と呼び出し (位置引数のみ)、puts/p |
| M1 | Array/Hash/Range、block/yield/Proc、class 定義・ivar・継承、
可変長引数 (splat/opt/kw)、例外 (raise/rescue/ensure/retry)、String 演算一式、
Comparable/Enumerable の主要メソッド |
| M2+ | Module/include、特異メソッド、define_method、Fiber 等 — v1 done.md を
参考に需要順 |

実装しない (v1 と同じ方針): Regexp 自前実装 (astrogre 統合待ち)、
C 拡張 API、マルチスレッド。

## 3. コマンドライン仕様

### 3.1 起動形

```
koruby_precise [framework-flags] [sample-flags] [--] [script.rb] [args...]
koruby_precise -e 'code' [args...]
koruby_precise            # 引数なし: stdin からスクリプトを読む (CRuby 互換)
```

`script.rb` 以降の引数は Ruby レベルの `ARGV` に入る。

### 3.2 framework 標準フラグ (astro_build_extract_flags が所有)

`docs/sample_cli.md` の canonical CLI をそのまま使う。**エイリアスは作らない**。

| flag | 意味 |
|---|---|
| `--plain` | 純インタプリタ実行。code_store を無視 |
| `--aot-compile` | AOT bake。**koruby pattern** (§3.4): 必ず実行を伴う |
| `--pg-compile` | PG (profile-guided) bake。実行を伴う |
| `--build OUT` | 単体実行ファイルを OUT に出力 (`--generate-executable` 系) |
| `-q` / `--quiet` | 静粛 (バナー・統計を抑止) |
| `-v` / `--verbose` | 冗長 (GC/AOT の統計、bake ログ) |
| `-h` / `--help` | usage を表示して exit 0 |
| `--version` | `koruby_precise <ASTRO_VERSION>` を表示して exit 0 |

C ツールチェイン調整 (`--cc`, `-O*`, `--lto` 等) は argv ではなく
`ASTRO_BUILD_OPTS` 環境変数 (framework 規約)。

### 3.3 sample 固有フラグ

最小限から始める:

| flag | 意味 |
|---|---|
| `-e 'code'` | code を実行 (複数指定は当面非対応) |
| `--dump-ast` | パース結果の AST を表示して終了 |
| `--ccs` / `--clear-code-store` | `code_store/` を消してから続行 |

v1 にあった `-s` / `-b` / `-j` 等は、必要が生じた時点で個別に再導入する。

### 3.4 AOT の意味論 — koruby pattern

method の AST は**実行して初めて code_repo に集まる**ため、bake-only は
成立しない。`--aot-compile` は常に「実行しながら収集 → 終了時に bake」:

```sh
./koruby_precise --aot-compile app.rb   # 実行 + code_store/all.so を bake
./koruby_precise app.rb                 # 次回: cached SD を hash 一致で swap して実行
./koruby_precise --plain app.rb         # cache を無視して純インタプリタ
```

- code store は **cwd の `code_store/`** (framework 規約)。実体は
  `astro_cs_init` / `astro_cs_compile` / `astro_cs_build` / `astro_cs_load`
- cache ヒットは structural hash (HORG) 一致で判定。runtime promotion との
  整合は `@canonical=` (v2_design.md §8.3)
- `--pg-compile` は PG bake (PGSD、HOPT hash)
- rubyharness の bench モード `aot+compile` / `aot+cached` / `pg+cached` が
  この 3 フラグを直接叩く — **このフラグ仕様が harness との契約**
- 実装注意: `EVAL(c, fd->body)` のような runtime ポインタ越しの dispatch は
  独立した `astro_cs_compile` entry が必要 (v1 の教訓)。bake 環境で ccache が
  落ちる場合は `CCACHE_DISABLE=1` (docs/code_store_quirks.md)

### 3.5 終了コードと例外時の出力

| code | 条件 |
|---|---|
| 0 | 正常終了 |
| 1 | 未捕捉例外 / SyntaxError (CRuby 互換)。stderr に message + backtrace |
| 2 | usage エラー (不明なフラグ、ファイルなし) |

未捕捉例外の表示は CRuby 形式に揃える (rubyharness の差分テストが
stderr も比較対象にできるよう):

```
app.rb:3:in 'foo': msg (RuntimeError)
        from app.rb:7:in '<main>'
```

## 4. 環境変数

| 変数 | 意味 |
|---|---|
| `ASTRO_GC_STRESS[=N]` | 毎 alloc (または N alloc ごと) collect |
| `ASTRO_GC_PURGE` | PURGE モード (退役 plane を mprotect で殺す) |
| `KORUBY_SLOTS_BYTES` | slots 仮想予約サイズ (default 8 MiB。深い再帰テスト用) |
| `ASTRO_BUILD_OPTS` | AOT bake 時の C ツールチェインオプション |

GC backend は実行時ではなく **build-time switch** (`make GC=<backend>`、
baruby_precise と同じ流儀)。**default は初日から copy (moving)** —
non-moving は rooting 漏れを発火させないため gate に使えない
(v2_design.md §9.1)。mark 等は比較・デバッグ用。

## 5. ビルド

```sh
make                  # default backend でビルド
make GC=copy          # backend 指定 (marker file で -D 切替の再 link を保証)
make test [CAT=...] [STRESS=1]   # rubyharness (include ../rubyharness/harness.mk)
make bench            # rubyharness 多モード bench
make codeql           # 静的検査 (v2_design.md §10)
```

Makefile は `*.c` glob ではなく明示列挙 + GC marker file (v1 の
glob 依存罠を踏まない)。

## 6. 受け入れ基準 (マイルストーンの出口)

全段階で **copy (moving) backend + STRESS+PURGE を gate に含める**
(GC 強度の段階導入はしない。v2_design.md §9.1):

| 段階 | gate |
|---|---|
| M0 | `make test CAT=basic` green (STRESS+PURGE 含む) + `bench` の interp/aot 両モードが動く (AOT は M0 から契約に入れる) |
| M1 | 主要 CAT green (STRESS+PURGE 含む)、bench で v1 (git 履歴の基準値) / CRuby と比較 |
| M2+ | スコープ拡大ごとに同 gate 維持。最終目標 rubyspec 広範 PASS + optcarrot |
