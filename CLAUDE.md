# ASTro Project

ASTro (AST-based Reusable Optimization Framework) は、AST を辿るインタプリタの部分評価により高速化を実現する言語実装フレームワーク。

## プロジェクト概要

詳細は `docs/idea.md` を参照。`docs/usage.md` は ASTroGen + runtime ライブラリの利用ガイド。

## リポジトリ構成

- `lib/astrogen.rb` — ASTroGen コア（`node.def` からインタプリタ・部分評価器・ハッシュ等の C コードを自動生成）
- `runtime/` — 全サンプル共通の C ランタイム
  - `astro_node.c` — `#include` 形式の共通ヘルパ (`HASH`, `DUMP`, ハッシュ関数群、`alloc_dispatcher_name`)
  - `astro_code_store.{h,c}` — AOT/PG コードストア API (`astro_cs_init` / `astro_cs_compile` / `astro_cs_build` / `astro_cs_load` / `astro_cs_reload`)
- `sample/` — 各種言語実装サンプル
  - 教育用: `calc` (3 ノード電卓), `pascalast` (Pascal サブセット)
  - メインストリーム: `naruby` (Ruby サブセット, JIT 対応), `abruby` (Ruby サブセット, CRuby C 拡張), `koruby` (Ruby+, optcarrot 動かすことを目標), `luastro` (Lua サブセット), `ascheme` (Scheme), `wastro` (Wasm), `jstro` (JavaScript), `astocaml` (OCaml サブセット), `castro` (C サブセット), `asom` (SOM), `astrogre` (Ruby/Onigmo 互換 regex エンジン + `are` という grep CLI)
- `docs/`
  - `idea.md` — 設計思想 (ASTro の核心アイデア、JIT 設計、Code Store)
  - `usage.md` — ASTroGen + runtime の利用ガイド (新サンプルを書くとき読む)
  - `perf.md` — クロスサンプル性能向上知見集
  - `code_store_quirks.md` — Code Store 利用時の罠メモ (dlopen キャッシュ等)
  - 論文 PDF (VMIL2025, PPL2026)

## 各サンプルの doc

サンプルが固有の done/todo/perf/runtime ドキュメントを持つ場合は `sample/<lang>/docs/` に置く (例: `sample/astrogre/docs/perf.md`、`sample/castro/docs/perf.md`)。クロスサンプルの知見は root の `docs/perf.md` から参照する。
