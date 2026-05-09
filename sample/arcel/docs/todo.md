# arcel — TODO

## DONE

### 環境整備
- [x] CEL conformance suite を取得 (30 .textproto / 837 evaluable cases)
- [x] textproto parser (cel-spec の repeated / [Any] / inf/nan / trailing comma / `\u`/`\U` 対応)
- [x] conformance runner (`test/run_conformance.rb`、`--use-ref` / `--bin` / `--filter` / `--include`)
- [x] cel-go reference 実装 (`test/celgo_ref/`、`eval` / `bench` / `repl` の 3 サブコマンド)
- [x] benchmark scaffolding (`benchmark/run.rb`、11 ケース、3-way 比較)

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
- [x] **bench で cel-go の 1.8〜9.5× の speedup** (realistic K8s policy で 4×)

## 次フェーズ — 性能勝負

`docs/perf.md` 詳細。優先順:

- [ ] AOT 経路を機能させる
  - [ ] `astro_cs_build` の make が ccache 経由で落ちる問題: env を
        透過させるか、Makefile 経路で `CCACHE_DISABLE=1` を強制
  - [ ] AOT 動作確認後、interp との差を取り直す
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

## 機能拡張 (要望に応じて)

- [ ] timestamp / duration の代表的 op (`> < ==`, `+ duration`、文字列 parse)
- [ ] cel-go ext: `strings.replace`, `lists.range` 等
- [ ] `optional` 型 (`x.?y`, `optional.of(...)`)
- [ ] PG mode (代表入力で実 input shape を観測 → 第 2 段の specialize)
- [ ] proto2/proto3 message literal: 範囲外の方針だが、需要次第で
       protobuf-c を embed する道はある

## ハーネス改善

- [ ] benchmark を timestamp / matches / 大規模 input でも回せるようにケース追加
- [ ] `make test --quick` (速い subset) と `make test --full` (proto/ext 含む) の分離
- [ ] CI 用に小さい smoke (1 秒で終わる) を separate target に

## ドキュメント

- [ ] `docs/runtime.md`: arena / VALUE / マクロ binding stack の実装詳解
       (arjsv の `runtime.md` 形式)
- [ ] cel-go との差分を README ベンチ表に貼り続ける
       (regression detection)

## Known issues / 既知の差分

- [ ] `CCACHE_DISABLE=1` を毎回付ける必要がある (`astro_cs_build` の subprocess
       が ccache を見にいく)。Makefile 側で吸収するか、astro_cs_build を
       直す
- [ ] AOT が interp と tied — 実は AOT build 自体が失敗していて
       interp に fallback している。詳細は perf.md
- [ ] map lookup が flat scan: 大きい入力での性能テストはまだ。
       `k8s_admission_ish` で OK だが 100-key map に対しては未測
