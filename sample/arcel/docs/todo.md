# arcel — TODO

## DONE

### 環境整備
- [x] CEL conformance suite を取得 (30 .textproto / 837 evaluable cases)
- [x] textproto parser (cel-spec の repeated / [Any] / inf/nan / trailing comma / `\u`/`\U` 対応)
- [x] conformance runner (`test/run_conformance.rb`、`--use-ref` / `--bin` / `--filter` / `--include`)
- [x] cel-go reference 実装 (`test/celgo_ref/`、`eval` / `bench` / `repl` の 3 サブコマンド)
- [x] cel-cpp reference 実装 (`test/celcpp_ref/`、`eval` / `bench`)
- [x] benchmark scaffolding (`benchmark/run.rb`、11 ケース、4-way 比較: arcel-i / arcel-A / cel-go / cel-cpp)

### 言語実装
- [x] リテラル: int / int64 / uint / double / bool / null / string / bytes
       (octal/hex/unicode escape は string で UTF-8 codepoint、bytes で byte 直書き)
- [x] 算術: `+ - * / %` (int/uint/double, overflow 検査、cross-type 抑止)
- [x] 比較: `== != < <= > >=` (cross-type 数値、bool ordering、string/bytes lex)
- [x] 論理: `&& || !` (short-circuit + commutative-on-error)
- [x] 三項 `?:`
- [x] 単項マイナス・否定 (negative INT64_MIN literal も含む)
- [x] ident / field access (`.`) / index (`[]`)
- [x] list / map literal
- [x] 標準関数: `size`, `type`, `int`, `uint`, `double`, `string`, `bool`, `bytes`, `dyn`,
       `startsWith`, `endsWith`, `contains`, `matches`
- [x] マクロ: `has(...)`, `all`, `exists`, `exists_one`/`existsOne`, `filter`, `map`
- [x] マクロ2: `xs.<macro>(idx, val, body)` 二引数形式、`transformList`, `transformMap`
- [x] 入力 binding (JSON → arcel_value 自前 tree → 識別子ルックアップ)
- [x] arena: per-eval (transient) と per-program (bindings) の 2 段
- [x] CLI: `eval` / `bench` / `repl` (cel-go reference と同一プロトコル)
- [x] **CEL conformance suite 808/808 = 100% pass** (cel-go reference は同 harness で 89.7%)
- [x] **bench で cel-cpp の geomean 14× speedup** (realistic K8s policy で 22.4×)

### 埋め込み API + 拡張機能 (Phase 1–10)
- [x] **Phase 1**: C library API (`arcel.h` + `libarcel.{a,so}`)、CLI も同 API 経由
- [x] **Phase 2**: Bazel BUILD + extern-C from C++、`runtime/` を Bazel package 化
- [x] **Phase 3**: `AC_OBJECT` + `arcel_object_desc` (embedder の native struct を pass-through)
- [x] **Phase 3b**: header-only libprotobuf adapter (`arcel_protobuf.h`)
- [x] **Phase 4**: cel-cpp API shim (`compat/celcpp_compat.hpp`)
- [x] **Phase 5**: REPEATED proto fields (`arcel_value_list_new` + arena handle in field callback)
- [x] **Phase 6**: `map<K,V>` proto fields (`arcel_value_map_new`)
- [x] **Phase 7**: `AC_TIMESTAMP` / `AC_DURATION` + cel-spec ops (constructor, comparison, arithmetic, selectors with tz)
- [x] **Phase 8**: 型識別子 (`int`, `string`, `google.protobuf.Timestamp`, …) + wrapper-message リテラル (auto-unwrap) を parse-time fold
- [x] **Phase 9**: AOT bake で `arcel_env_new` が `CCACHE_DISABLE=1` を auto-set (sandbox / read-only HOME 環境でも動く)
- [x] **Phase 10**: in-process binding-path bench (`examples/embed_bench.cc` — JSON / native struct / libprotobuf / cel-cpp shim を 1 binary で計測)
- [x] **Phase 11**: default を interp に反転 (`arcel_env_new` が `no_compile=true` で生成)。AOT が欲しい時は `arcel_env_set_no_compile(env, false)` か CLI `--compile`。理由: realistic CEL では interp 単体で cel-cpp 比 5-9× が出ていて、AOT の追加 win 3% に対して bake の運用コスト (subprocess make / dlopen / `code_store/` artifact / ccache 周りの罠) を default で払う価値が薄い

## 次フェーズ — 性能勝負

`docs/perf.md` 詳細。優先順:

- [x] AOT 経路を機能させる
  - [x] `astro_cs_build` の make が ccache 経由で落ちる問題: env を
        透過させるか、Makefile 経路で `CCACHE_DISABLE=1` を強制
        → Phase 9 (2026-05-09): `arcel_env_new` で `CCACHE_DISABLE` 未設定時に自動で `=1` する
  - [x] AOT 動作確認後、interp との差を取り直す → `docs/perf.md` 表 (cel-cpp 比 22.4×、cel-go 比 11.6×)
- [ ] map lookup の hash table 化
  - [ ] `arcel_field` / `arcel_index`: 8 entry を超えたら hash table
        (open-addressing, FNV)
  - [ ] field name の hash を AST に焼く (parse 時に precompute、
        node に operand として hold)
- [ ] AOT 時の field name embed
  - [ ] SD specialize で `name` literal を直接焼き込み、`arcel_field`
        ヘルパ呼び出しを inline match に折りたたむ
- [ ] `xs.matches(re)` の事前 compile
  - [ ] 第 1 段: regcomp の結果を node の side cache に持つ
        (`@ref` operand)
  - [ ] 第 2 段: astrogre backend に切り替えて静的 DFA 化
- [ ] in-process bench (`embed_bench.cc`) で見えてきた flat-AOT 案件:
      string / map / list 操作が hot な predicate では AOT win が ~3% に
      留まる。helper 関数の inline 強化 (特に `arcel_field` の name
      literal embed) で改善できるはず。

## 機能拡張 (要望に応じて)

- [x] timestamp / duration の代表的 op (`> < ==`, `+ duration`、文字列 parse)
       → Phase 7 (2026-05-09): AC_TIMESTAMP / AC_DURATION + tz selectors. timestamps suite 73/73 (100%)
- [x] `google.protobuf.Timestamp` 等の型識別子参照
       → Phase 8 (2026-05-09): parse-time fold to string literal
- [x] `google.protobuf.Int32Value{value: X}` 等の wrapper-message リテラル
       → Phase 8 (2026-05-09): parse-time auto-unwrap to wrapped primitive
- [ ] cel-go ext: `strings.replace`, `lists.range` 等
- [ ] `optional` 型 (`x.?y`, `optional.of(...)`)
- [ ] PG mode (代表入力で実 input shape を観測 → 第 2 段の specialize)
- [ ] **proto2/3 user 定義 message literal** (`TestAllTypes{...}`): 範囲外
       の方針だが、需要次第で embedder 側 (cel-cpp shim 等) に型レジストリ
       hook を持たせる手はある。arcel 本体に protobuf 依存は入れない方針を
       維持。

## ハーネス改善

- [ ] benchmark を timestamp / matches / 大規模 input でも回せるようにケース追加
- [ ] `make test --quick` (速い subset) と `make test --full` (proto/ext 含む) の分離
- [ ] CI 用に小さい smoke (1 秒で終わる) を separate target に
- [ ] `test/run_conformance.rb` の `:ext` タグから timestamps / 一部 conversions
      を外して default に取り込む (Phase 7+8 で 100% pass しているため)

## ドキュメント

- [ ] `docs/runtime.md`: arena / VALUE / マクロ binding stack の実装詳解
       (arjsv の `runtime.md` 形式)
- [ ] cel-go との差分を README ベンチ表に貼り続ける
       (regression detection)
- [x] `docs/idea.md` / `docs/spec.md` / `docs/perf.md` / `docs/todo.md` を
      Phase 6–10 の現状に合わせて全面更新 (2026-05-09)

## Known issues / 既知の差分

- [x] `CCACHE_DISABLE=1` を毎回付ける必要がある (`astro_cs_build` の subprocess
       が ccache を見にいく)。Makefile 側で吸収するか、astro_cs_build を
       直す
       → Phase 9 (2026-05-09): `arcel_env_new` 内の setenv で吸収。CLI も embedder も prefix 不要。明示で `CCACHE_DISABLE=` を空に setenv すれば従来動作
- [x] AOT が interp と tied — 実は AOT build 自体が失敗していて
       interp に fallback している
       → Phase 9 で解消、現状の数値は AOT 実動状態
- [ ] map lookup が flat scan: 大きい入力での性能テストはまだ。
       `k8s_admission_ish` で OK だが 100-key map に対しては未測
- [ ] `embed_bench` は project root から実行すると `code_store/` の
      include path 解決 (cwd-依存) で AOT bake が落ちる。`sample/arcel`
      cwd で実行すること (`cd sample/arcel && bazel-bin/.../embed_bench`)。
      runtime 側の絶対パス解決を直すか、env で cwd を pin したい
- [ ] cell-cpp shim の `Activation::InsertObject` は per-iter で arena alloc
      していないが、shim 側 `CelValue` の StatusOr ラップが ~50-100ns
      乗る (embed_bench 結果より)。production 用途で気になる場合は
      arcel 直叩きを推奨
