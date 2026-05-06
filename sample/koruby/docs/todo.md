# todo.md — koruby Ruby 互換性ギャップ

[done.md](./done.md) は実装済み。 ここは **未実装 / 不完全 / 既知バグ** の作業リスト。

現状: koruby 自前 test/ 全 pass (24 ファイル, 190 件 OK), optcarrot は CRuby と一致。
CRuby の test/ruby/ を tu_shim 経由で **1,108,357 ケース pass / 1,430,888 中** (in-scope 67 ファイル, 77.5%)。test_integer_comb (+920k) と test_integer (+160k) の Float / Encoding / Regexp 系 fail が分母を支配しているため在の在 % は伸びにくいが、language semantics のテスト群はかなり緑。

## 範囲外 (project policy / user 指定)

これらは TODO ではなく **scope の外**。 触らない。

| 項目 | 理由 |
|---|---|
| Regexp (`=~` / `/.../` / etc.) | astrorge 経由で integrate する方針 |
| Thread / Mutex / Queue / ConditionVariable | single-threaded only |
| Encoding-aware String (`String#encoding`, `force_encoding`, multi-byte succ, m17n strings) | byte sequence のみ |
| Process / fork / spawn / `assert_separately` 等 | excluded |
| ObjectSpace 走査 / 詳細 | excluded |
| NaN-boxing | 値表現変更禁止 |
| Refinements (`refine` / `using`) | 言語拡張、 範囲外 |
| Ractor | 並列拡張、 範囲外 |
| Fiber Scheduler / Async I/O | 範囲外 |
| ruby2_keywords | 互換性 marker、 range の外 |
| DidYouMean (NoMethodError 提案) | 大物。 別途 |

## §A 残バグ

- [x] ~~**Boehm GC 再帰クラッシュ** in test_exception~~ — 実は GC の問題ではなく `Kernel#exit` が libc `exit()` を直接呼んでいて SystemExit を上げず、 後続テストが止まっていた件 + `raise nil` / `raise 1, 1` が state_value=nil/1 のまま伝播していて (anon) や nil が unhandled で出ていた件。 `kernel_exit` を SystemExit raise に変更し、 `kernel_raise` で argv の型を厳密にチェック (`raise(non-Exception)` → TypeError)、 `def self.foo` での singleton class wrapping 中の ivar lookup を super 経由に修正。 test_exception が 88/173 pass で完走。
- [x] ~~**block param `*x` splat が Fiber 越しに値消失**~~ — `korb_yield_slow` が `params_cnt > 1 && argc == 1 && Array` のとき auto-destructure するが、 block が `|*x|` (rest only) のとき rest_slot に値を入れていなかった。 rest_slot 用の gather + single-Array passthrough を追加。 to_enum も `|*x|` で動くようになった。
- [ ] **Float#floor(n) の Float 精度** — Float 表現の本質 (291.4.floor(2) は 291.39 になる)。
- [ ] **proc/lambda の post-rest の parameter 名** が `[:req]` のまま
- [ ] **m17n strings の各種**: encoding を真面目に処理してないので multi-byte 周りで slot wrap や split で誤動作 (slow loops が cumulative して hang)

## §B Proc / Lambda 強化

- [x] ~~**kwargs (proc / lambda)** が arity / parameters / 呼び出しに反映されない~~ — `build_call_with_block` に lambda と同じ kwargs prologue (required_keyword / optional_keyword / **kwrest) を追加。 proc も lambda 経路の block_literal_kw を使う。 test_keyword: 261→318 (+57)、 test_proc: 270→317 (+47)。
- [ ] **`define_method` arity** が CRuby より緩い (test_keyword)
- [ ] **`proc.curry` lambda 厳密モード**

## §C 標準ライブラリ — 残小物

- [ ] String#bytesplice (byte-level 編集)
- [ ] Struct: `keyword_init: true`、 IndexError on bad index、 to_h with block、 `#define` の TypeError 整合性
- [ ] Data: 真の Data 実装 (現状 Struct 経由)
- [ ] Complex / Rational: `coerce` 経由の polar canonicalization、 NaN/Infinity の to_s 表記、 expt special angles

## §D Module / Class 内部

- [ ] `Class#attached_object` (singleton class API)
- [ ] `Module#prepend_features`, `Module#append_features` 公開
- [ ] `Module#const_source_location` の真の実装 (line 番号を保存)

## §E パーサー (prism のバージョン制約)

- [ ] `def f(&nil)` / `def f(**nil)` (Ruby 3.4) — PM_MISSING_NODE で吸収済み
- [ ] kwsplat strict-arity の境界

## §G 2回目のセッションで追加された主な機能 / 修正

### Proc / Lambda / Block 関連 (大物)

- **`def self.foo(&b)` block bind bug 修正** — `node_singleton_def` / `node_obj_singleton_def` に block_slot 引数を追加。これが効かないと module/class method の `&b` parameter が常に nil になっていた (test_sprintf_comb の load fail もこれ起因)。
- **proc cref capture** — `struct korb_proc` に `cref` 追加。block 作成時の `c->cref` を `korb_proc_new_with_cref` で捕捉、proc_call で復元。`class C; X=1; proc { X }; end` の C::X 解決に必要。
- **proc post params 完全対応** — `struct korb_proc` に `post_cnt` 追加、`node_proc_set_post_cnt` で parse 時に設定。proc_call の bind 順序を req → post → opt/rest に書き換え。
- **proc `&blk` parameter** — `struct korb_proc` に `block_slot` 追加。proc_call で caller の current_block (or nil) を bind。
- **Proc#arity 修正** — `req + post` を required total に。opt/rest/lambda strict mode 場合分け。
- **Method#arity 修正** — kwh_save_slot / post_params_cnt も考慮。
- **prologue_proc_method で recv → c->self override** — define_method'd proc が instance method として呼ばれた時に self が class のままだったのを修正。
- **instance_eval / class_eval / Class.new の cref override** — block の cref を receiver class に temporary に書き換え、`def` と const lookup が receiver class で resolve するように。

### tu_shim 互換性

- **BasicObject methods** — `__id__` `==` `!=` `!` `equal?` `__send__` `instance_eval` `instance_exec` を cBasic に直接 install (CRuby と同じ; これが無いと Class.new(BasicObject) の subclass で何もできない)。
- **Object#__id__** alias。
- **Module#autoload / autoload?** stub (no-op)。
- **defined?(`A::B::C`)** const path 対応 — rescue で nil 化。`unless defined?(Test::Unit::Assertions)` などが効くようになり tu_shim 全載せ可能。

### Hash / Array / Module 補完

- **Hash#assoc / rassoc / `<` `<=` `>` `>=`** (set comparison)。
- **Array#map! / collect! / each_index** (Hash の同名メソッドと衝突しないように `class Array` block 内に配置)。
- **attr_reader/writer/accessor** が `[:a, :b]` / `[:c=]` / `[:a, :a=, :b, :b=]` を返すように修正 + invalid name で NameError。

### 成果 (in-scope 67 ファイル)

baseline 1,106,121 → **1,108,357 pass** (+2,236)。
- test_proc: 317 → 559 (+242, 大物)
- test_iterator: 0 → 83 (load 修正)
- test_defined: 0 → 59 (load 修正)
- test_name_error: 0 → 14 (load 修正)
- test_hash: 1153 → 1233 (+80)
- test_module: +27, test_array: +67, test_basicinstructions: +27, test_call: +18
- test_keyword: +5, test_method: +7, test_const: +11

### §G 残課題

- **Method#parameters の keyword 名追跡** — `def f(a:)` が `[[:keyreq, :a]]` を返さない (今は `[[:keyrest]]` のみ)。`korb_method` に kw param 名配列が必要。
- **test_keyword の kwsplat 系** — super_kwsplat / instance_exec_kwsplat / Fiber_resume_kwsplat 等で 200+ fail。kwargs forwarding semantics の細部。
- **test_enumerator / test_fiber / test_float が timeout/segfault** — Frame ライフタイム or 巨大ループの問題。`korb_build_backtrace` で `f->method` が解放済みポインタを参照することがある。
- **test_array の sample_random_srand0 / shuffle (~2200 fail)** — Random reproducibility (CRuby と同じ seed で同じ結果)。

## §F 過去セッションで追加された主な機能 / 修正

- prepend+include super loop (frame.super_skip_n + iclass-aware MRO walker)
- Class.allocate.new TypeError
- Method#>> / Method#<< / Proc#>> / Proc#<< (関数合成)
- Range#bsearch + Array#bsearch_index
- Exception#cause + #full_message + #detailed_message + #exception
- Exception#cause cycle 検出 (循環防止)
- NoMethodError#receiver / #name
- Fiber resume/yield で c->cref / current_class / current_frame 保存
- proc opt_cnt + node_default_init による proc default value 動作
- multi-rescue clause での `=> e` lvar 明示コピー
- obj_instance_eval / instance_exec / module_eval / module_exec で symbol-proc / Method-proc shim 直接 dispatch
- korb_hash_value 再帰ガード
- Array#flatten 再帰ガード
- String#concat (int / multi-arg / 自己エイリアス snapshot)
- Integer 系の bitwise op に coerce 対応
- Boolean (`true`/`false`/`nil`) の `&` `|` `^`、 NilClass の `to_a/to_h/to_f/to_i/nil?`
- Anonymous `*rest` (`def f(*)`) が引数を吸わなかったバグ修正
- `def self.foo` を top level で許可
- Range#begin / end / min / max エイリアス、 Range#reverse_each / each_with_object
- 大量の String / Module / Object 補完 (chomp! / chr / clear / codepoints / casecmp(?) / delete_prefix! / delete_suffix! / upto / succ! / +@ / -@ / Module#public_methods / private_methods / protected_methods / method_defined?(name, inherit) / public_method_defined? 等の inherit 引数)
- public/private/protected が引数あり時にシンボル/配列を返す (Ruby 3.0+)
- raise(cause: x) の kwarg 形に対応
- visibility の inherit 引数
- bsearch_index, each_entry, chunk, &/|/-, values_at, union/intersection/difference, rindex, select!/reject!/keep_if/delete_if, sort_by!, to_h(&blk) (Array)
- transform_keys!/transform_values!/select!/reject!/keep_if/delete_if/initialize_copy/compact[!]/to_proc/to_hash/to_h(&blk) (Hash)
- div / divmod / fdiv / floor / ceil / round / truncate (Rational)
- Numeric#fdiv / finite? / infinite? / nan? / real / imaginary / real? / integer?
- Complex#kind_of?(Numeric) / Rational#kind_of?(Numeric)
- Method#super_method, Module#const_source_location stub, Module#deprecate_constant stub
- Symbol#call, Symbol#!~, Object#to_enum, Object#remove_instance_variable, Object#Hash/Array/String/Integer/Float
- Range#bsearch + Lazy#each_cons / each_slice / cycle / filter_map / uniq / eager / chunk_while / slice_when / first(n)
- Random#urandom / hex / base64 / _dump_data / _load_data
- Float#floor(n) / ceil(n) で ndigits 引数
- String#byteslice / append_as_bytes / setbyte / getbyte / split with block
- class variable name validation (`@@` プレフィックス必須)
- module_const_set NameError validation
- tu_shim: assert_not_empty / assert_method_defined? / assert_method_not_defined? / build_message / all_assertions / all_assertions_foreach / assert_send / assert_not_send
- tu_shim: NoMemoryError / SystemStackError / UncaughtThrowError / ThreadError / SystemCallError / Errno::* 定数群
- EnvUtil.verbose_warning / capture_warning / labeled_class / labeled_module
- PM_MISSING_NODE を nil 代入で吸収 (Ruby 3.4 syntax)
- 約 ~80 件の補完メソッド + ~50 件の bug 修正で +15k pass
