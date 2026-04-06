# ASTro Project

ASTro (AST-based Reusable Optimization Framework) は、AST を辿るインタプリタの部分評価により高速化を実現する言語実装フレームワーク。

## プロジェクト概要

詳細は `docs/idea.md` を参照。

## リポジトリ構成

- `lib/astrogen.rb` — ASTroGen コア（node.def からインタプリタ・部分評価器等の C コードを自動生成）
- `sample/calc/` — 最小の計算機言語サンプル（3ノード）
- `sample/naruby/` — Ruby サブセット言語 naruby（21ノード、JIT対応）
- `docs/` — 論文 PDF および設計ドキュメント
