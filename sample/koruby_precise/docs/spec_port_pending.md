# spec port — pending failures (見つかった未解決の挙動)

rubyspec から port した test (test/test_spec_port.rb) で判明した、GC 以外の
挙動バグ / 未実装。test 側で `# PENDING:` コメント + assert をコメントアウトして
おり、ここに一覧する。

- **String#split(sep, limit)**: limit 引数が無視される。`"a,b,c".split(",", 2)` が
  `["a","b","c"]` を返す (CRuby は `["a","b,c"]`)。str_split に limit 処理が無い。
  GC 非関連の feature gap。

- **`obj[i] = <Array/Hash literal>` writeback dropped** (IMPACTFUL — 要修正):
  index 代入の右辺が **Array/Hash リテラル** (変数でなく) のとき、代入が黙って捨てられる。
  `h[1] = [9]` → `{}` (CRuby `{1=>[9]}`)。`a[0] = [9]` → 変化なし。
  `(h[1] ||= []) << 5` も無効 → **Enumerable#group_by 等が壊れる** (内部で `h[k] ||= []` を使う)。
  変数右辺 (`v=[9]; h[1]=v`) と Hash 末端 scalar (`h[1][2]=3`) は OK。
  原因: node_aset は recv を sp[0] に置くが、リテラル右辺の node_ary_new/node_hash_new が
  自分の結果を **同じ sp の sp[0]** に書いて recv を clobber → store が間違った object
  (リテラル自身) を対象にする。fp slot に退避案も、リテラルの element が **同じ
  arg_index frame slot** を再利用するため衝突 (parse 時の arg_index 共有)。
  → 正しい修正は parse.c + node.def 両方で node_aset の recv/idx slot を val リテラルと
    非衝突に予約する必要があり、 regression リスク高。 dedicated に直す案件として pending。
  regression test: test/test_aset_literal.rb。

- **stored-Proc captured-array mutation crashes under STRESS** (control-flow rooting):
  `f = ->(x){ acc << x }; (1..30).each{|x| f.call(x) }` で acc (closure capture の
  moving array) が proc_call の epilogue で stale → SEGV。直接 `acc<<x` は OK。
  flat_map / each_with_object (bootstrap.rb の Enumerable、 内部で stored block を
  loop call) が依存。crash は proc_call (builtins/proc.c:377) の
  korb_proc_snapshot_env_maybe(_br.value) で _br.value を BUILTIN_TYPE deref。
  = method dispatch / proc_call / frame-transition で moving value を C-local 保持する
  共通 subsystem の rooting gap (test_alias_redef = korb_class_of_class(recv),
  test_fiber と同族)。 rubyspec STRESS の CRASH=143 の大半はこの共通経路由来 (個別
  builtin 修正では CRASH 数が動かないことで確認済)。 dedicated な dispatch/proc-call
  value-rooting の作り直しが要る = 次の本丸。
