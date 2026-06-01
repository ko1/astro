# spec port — pending failures (見つかった未解決の挙動)

rubyspec から port した test (test/test_spec_port.rb) で判明した、GC 以外の
挙動バグ / 未実装。test 側で `# PENDING:` コメント + assert をコメントアウトして
おり、ここに一覧する。

- **String#split(sep, limit)**: limit 引数が無視される。`"a,b,c".split(",", 2)` が
  `["a","b","c"]` を返す (CRuby は `["a","b,c"]`)。str_split に limit 処理が無い。
  GC 非関連の feature gap。
