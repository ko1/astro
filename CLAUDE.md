# ASTro Project

ASTro (AST-based Reusable Optimization Framework) は、AST を辿るインタプリタの部分評価により高速化を実現する言語実装フレームワーク。

## プロジェクト概要

詳細は `docs/idea.md` を参照。`docs/usage.md` は ASTroGen + runtime ライブラリの利用ガイド。

## リポジトリ構成

- `lib/astrogen.rb` — ASTroGen コア（`node.def` からインタプリタ・部分評価器・ハッシュ等の C コードを自動生成）
- `runtime/` — 全サンプル共通の C ランタイム
  - `astro_node.c` — `#include` 形式の共通ヘルパ (`HASH`, `DUMP`, ハッシュ関数群、`alloc_dispatcher_name`)
  - `astro_code_store.{h,c}` — AOT/PG コードストア API (`astro_cs_init` / `astro_cs_compile` / `astro_cs_build` / `astro_cs_load` / `astro_cs_reload`)
- `sample/` — 各種言語実装サンプル (全 29 個)
  - 教育用: `calc` (6 ノード電卓), `abc` (POSIX/GNU `bc` 互換 任意精度電卓, GMP 仮数+scale / libgc, `bc` との差分テスト 5,500+)
  - Ruby 系 (6): `naruby` (Ruby サブセット, JIT 対応), `baruby` (naruby fork + Array/String + libgc, **統一 GC testbed**), `baruby_precise` (baruby fork、precise rooting + **14 種類の自前 GC を build-time switch** できる GC algorithm 比較 testbed), `abruby` (Ruby サブセット, CRuby C 拡張), `koruby` (Ruby+, optcarrot 動かすことを目標), `koruby_precise` (**主力**: koruby を slots ABI で全面再構築 (v2) + precise rooting/moving GC。CRuby drop-in が目標で、optcarrot を checksum 一致で実行、AOT が YJIT を上回る。rubyspec core は実 mspec で 80%。wasm32-wasip1 対応: --build で全埋め込み AOT .wasm + ブラウザ demo)
  - その他動的言語 (4): `luastro` (Lua サブセット), `pystro` (Python 3 サブセット, GMP bignum + class + try/except), `jstro` (JavaScript, hidden class IC), `anlox` ([Lox](https://craftinginterpreters.com/) = Crafting Interpreters の題材言語: 動的型 + クロージャ + 単一継承クラス/this/super, パース時 resolver で local を (depth,slot) 解決, `// expect:` 自己完結テスト。An\* = "ASTro Nutshell" 題材言語シリーズ)
  - 関数型 / OO (6): `ascheme` (Scheme), `ascheme_precise` (ascheme fork、libgc → precise GC framework 移行版・17 GC backend), `astocaml` (OCaml サブセット), `asml` (Standard ML サブセット, **HM 型推論完備** + 型駆動の dispatcher 特殊化), `ancaml` ([MinCaml](https://esumii.github.io/min-caml/) = 単相 ML サブセット: 単相 HM 型推論 + de Bruijn フレーム + TCO, `ocaml` との差分テスト。An\* = "ASTro Nutshell" 題材言語シリーズ), `asom` (SOM)
  - 静的型 (3): `pascalast` (Pascal サブセット), `castro` (C サブセット), `anpy` (ChocoPy = 静的型付き Python 3.6 サブセット: 型検査器 + 単一継承クラス + クロージャ, libgc, `python3` との差分テスト)
  - スタックマシン (1): `aforth` (Forth サブセット、 全 word が AST NODE)
  - データ処理 (2): `astr` (R サブセット, libgc + tagged VALUE + numeric/string/list), `arawk` (POSIX awk サブセット, regex 除く、 gawk 0.93× geomean、 astrogre との AST interpreter 統合実験 (Phase 2) を予定)
  - DSL / 非ソース (5): `wastro` (Wasm), `astrogre` (Ruby/Onigmo 互換 regex エンジン + `are` という grep CLI), `nuq` (jq クローン: JSON フィルタ言語 + 70+ builtin), `arjsv` (JSON Schema draft-07 validator, CRuby C 拡張, `json_schemer` 互換 API), `arcel` (CEL 互換 predicate DSL: standalone CLI, cel-go/cel-cpp の dropin 代替を狙う。conformance 100% / 808-808 で cel-go の 89.7% を上回る、bench で cel-go 比 9× / cel-cpp 比 14× geomean, realistic K8s admission policy で cel-cpp 比 22.4× = 52 ns/op)
- `docs/`
  - `idea.md` — 設計思想 (ASTro の核心アイデアのみ。各論は `idea_*.md` に分割: astrogen / code_store / jit / evaluation / variadic / future。idea.md 末尾のドキュメントマップ参照)
  - `usage.md` — ASTroGen + runtime の利用ガイド (新サンプルを書くとき読む)
  - `perf.md` — クロスサンプル性能向上知見集
  - `code_store_quirks.md` — Code Store 利用時の罠メモ (dlopen キャッシュ等)
  - 論文 PDF (VMIL2025, PPL2026)

## 各サンプルの doc

サンプルが固有の done/todo/perf/runtime ドキュメントを持つ場合は `sample/<lang>/docs/` に置く (例: `sample/astrogre/docs/perf.md`、`sample/castro/docs/perf.md`)。クロスサンプルの知見は root の `docs/perf.md` から参照する。
