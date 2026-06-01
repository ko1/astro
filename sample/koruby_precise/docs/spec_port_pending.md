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

- **[FIXED 2026-06] stored-Proc captured-array mutation crashes under STRESS**:
  `f = ->(x){ acc << x }; (1..30).each{|x| f.call(x) }` で acc (closure capture の
  moving array) が stale → SEGV だった。真因は dispatch ではなく EVAL_node_lshift の
  Array fast-path (`a << x`) が korb_ary_push で l を grow (GC で移動) させた後 stale な
  C-local l を return していたこと。`a.push(x)` が動いて `a<<x` が落ちたのは push が
  別 path だったため。park slot を返すよう node.def 修正で解決。test_spec_port2 の
  closure-capture を un-pend 済 (normal/STRESS 両 green)。
  当初疑った dispatch 経路の stale-recv も実在し別途修正済
  (korb_dispatch_to_method の dispatch_recv_root park)。

- **[FIXED 2026-06] method dispatch で frame->self が stale (rubyspec STRESS 最大 cluster)**:
  DISPATCH_node_ivar_get / korb_class_of_class(recv) の SEGV。korb_eval_string が
  top_frame を prev=NULL で push するため、nested require/load 中に外側 (suspended) の
  eval top_frame が visit_roots の head chain から切れ、その間に main_obj が動くと
  frame->self が stale 化していた。CTX に eval_frame_chain stack を追加し全 eval
  top_frame を walk するよう修正。あわせて ary_first_n/last_n/delete_at/class_brackets/
  initialize, ary_lshift の builtin stale-self も修正。
  → rubyspec STRESS sweep: PASS 179→555 (CRASH 138→81 以降さらに低下)。

- **[FIXED 2026-06] string range の STRESS+PURGE crash — range→arena 移行で解消**:
  range を moving (aro_gc_alloc) 化し、真因の node_range_new の begin 先評価 stale
  (begin が end eval の GC で死んで range->begin に stale 格納) を AROH_ROOT_STACK park
  で解決。range.c の moving-r cascade も一掃。each/to_a/begin/include/first/new が動作。
  残 edge: step(大配列 grow)/min/max(niche)/symbol succ/reverse_each。詳細は array_moving_gc.md。

- **[旧 PENDING の記録] string range の STRESS+PURGE crash (range→arena 未移行)**:
  `("a".."e").each {...}` や単に `r=("a".."e"); GC; r.begin` が STRESS+PURGE で SEGV /
  `r.begin` が `false` に化ける。begin と end が **両方 heap obj (string)** のときだけ
  begin が壊れる (`5.."e"` や `"a"..nil` は OK)。rng_each 非依存の pre-existing GC bug。
  原因: korb_range は **xmalloc (非 moving)** で、begin/end の moving string は libc-obj
  registry (koruby_runtime.c phase f) 経由で forward される。registry walk の timing と
  to/from swap の組合せで r->begin が「現サイクルで未使用な to-space 領域」を指す
  stale ref になり、forward_payload (gc_copy.c:509) が NULL を返して begin=0(Qfalse) 化。
  正しい修正は **range を array 同様 arena (aro_gc_alloc) に移行** すること
  (gc_copy.c:503-506 のコメントが Phase 3 として明記)。range.c 全体の `struct korb_range *r`
  C-local 保持を sp[-argc-1] 再読込に直す必要があり中規模。array 移行が一段落してから着手。
  rng_each の numeric path は parked self を返すよう修正済 (string path のみ未解決)。
