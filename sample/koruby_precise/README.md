# koruby_precise — v2 (slots ABI で再構築中)

v1 の実装は削除した (2026-06-12)。**v2 として全面再構築する**ため。
M0 (calc 級 subset + AOT + STRESS/PURGE gate) は実装済み —
現状は [docs/v2_m0_status.md](./docs/v2_m0_status.md)。

```sh
make            # GC=copy (moving) default。prism は ../baruby_precise から
                # cp -r prism . (untracked vendored)
make run        # fib スモーク
make test       # rubyharness 差分テスト (CAT= / STRESS=1)
make bench      # 多モード bench (interp / aot+compile / aot+cached / cruby)
```

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
