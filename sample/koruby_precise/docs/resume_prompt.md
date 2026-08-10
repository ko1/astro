# 再開プロンプト (2026-08-10 セッション再起動用)

以下をそのまま次セッションの最初のプロンプトに:

---

koruby_precise の rubyspec 追従の続き。「自明に解決できる rubyspec を全て実装」の
マラソン中で、残りタスクを小→大の順に全部やる (memory の
project_koruby_precise_rubyspec_2026_08 と project_koruby_thread_io_design 参照)。

現状: spec/ruby 全体 17,465 pass (59.3%)、library 39.6%、whole-file-fail 15。
計測は ruby tools/mspec_real_run.rb <dir> <jobs> (本物 mspec、DUMP=file で per-file tsv)。

残りタスク:
1. CSV 複数行 parse バグ (CSV.parse("a,b\n1,2") が1行目しか返さない)。
   strscan の基本操作 (scan/scan_until/check/match?/skip/rest) は CRuby と
   一致確認済みなので、csv/parser.rb 側の実行を追う
2. `(...)` argument forwarding (PM_FORWARDING_* を parse.c で *args/**kw/&blk に lower)
3. digest を純 Ruby で (MD5/SHA1/SHA256、library/digest 68 files)
4. marshal の fail 91 + err 49 を mine して修正
5. date/datetime 純 Ruby 実装 (147 files、最大の library 塊)
6. fd ベース IO 層 (docs/io_design.md の続き、core io/file ~3600 err の本命)
7. socket 最小実装 (fd-IO 層の後)

作法: 各マイルストーンで make test / STRESS+PURGE / AOT (optcarrot checksum 60838、
--aot-compile 先行) / make codeql-check (5 rules) を回してからコミット。
vendored lib (lib/*.rb) の koruby 未対応構文はコメント付き局所 patch。
数字報告は pass 数と % を併記。full sweep の過去 dump (/tmp/mspec_all_*.txt) は
消えているので必要なら再計測。

---
