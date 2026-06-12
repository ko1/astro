# koruby_precise — v2 (再構築中、M0 コア動作)

v1 実装は削除し (2026-06-12)、同日 v2 M0 コアを実装開始。
ベースは baruby_precise fork + v2 ABI (VALUE_REF / RAISE / CRuby 互換出力)。
ビルドは `cp -r ../baruby_precise/prism .` で prism を持ってきてから `make`。
block / Proc は対象外。

- 設計: [docs/v2_design.md](./docs/v2_design.md)
- 仕様 (CLI / AOT / スコープ / gate): [docs/v2_spec.md](./docs/v2_spec.md)
- v1 の実装・履歴: git 履歴を参照 (削除 commit の親まで遡る)
- v1 時代の分析・記録 (`docs/closure_sp_model.md`,
  `docs/sp_transition_analysis.md`, `docs/done.md`, `docs/todo.md` 等) は
  v2 設計の根拠として残してある
- テスト・ベンチは [../rubyharness/](../rubyharness/) を使う
  (CRuby オラクル差分テスト + 多モード bench)

v1 最終状態の参考値: rubyspec/mspec_shim ベースで広範に PASS、
optcarrot 35.8 fps (CRuby の 0.83×)。
