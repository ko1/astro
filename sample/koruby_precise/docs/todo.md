# todo.md — koruby Ruby 互換性ギャップ

[done.md](./done.md) は実装済み機能の一覧。 ここは **未実装 / 不完全 /
既知バグ** の作業リスト。

## 現状 (2026-05-10, eleventh pass)

- **自前 test/ruby/**: **24/24 全 OK** (737 件)。
- **CRuby `test/ruby/` (全 135 ファイル)**: **89 / 135 has_pass (66%)、
  1,050,622 pass / 1,405,004 (74.8%)**。 起点 (2026-05-09 sweep #1) は
  62/135、 834,385 pass。 +27 ファイル復活、 +216k pass。
  - dumped core: 7 → 0 (super dispatch / refinement binding cref / mm
    recursion / fiber NULL body / Float SIGFPE / Integer overflow recursion
    / Comparable cmp_eq 全解消)
  - LOAD ERROR: 23 → 1 (残り test_time_tz は Regexp 必要)
  - timeout: 3 → 0 (IO.pipe を unbuffered に)
  - 残 0-pass の 46 ファイルは大半 intentional pending (TracePoint /
    ObjectSpace::WeakMap / Ractor / 真の Refinements / 非 UTF-8 Encoding /
    MJIT / 完全 Marshal / 真の ARGF / callcc)。
- **CRuby `spec/ruby/language/`**: **3,745 pass / 190 fail / 51 err**。
- **optcarrot**: AOT-cached で **~76 fps** (CRuby 43 fps の 1.8x、 yjit
  175 fps の 0.43x)。 100 frames best-of-1。
- **CRuby `spec/ruby/core/` 23 cat 集計** (array hash string integer numeric
  range comparable module proc kernel symbol float exception basicobject set
  rational random gc signal binding class enumerator regexp):
  - **pass=14,434 / fail=3,071 / err=1,014 (= 77.9% pass)**
  - **313 / 930 ファイル perfect (33.7%)**

## eleventh pass で test/ruby 互換性を全面強化

主要な fix と効果:
- **super dispatch bug** (object.c:korb_dispatch_binop): caller block の
  defining_method 漏れ。 `Class#new → user initialize → super` が
  "run_all" 等で lookup されてた致命バグ。 test_string 0 → 1965 pass。
- **UnboundMethod late-binding**: instance_method が name しか保存して
  なかったので、 後で define_method 上書きされると new body へ
  redirect され無限再帰。 captured_method を凍結。 test_super 蘇生。
- **Binding が stack-alloc cref を保存**: class body 中に binding する
  と class body 終了で dangle → SEGV。 cref chain を heap deep-copy。
  test_refinement 蘇生。
- **Struct.new {} block の cref leak**: `Struct.new(:x) do def
  method_missing; ... end end` が outer class の method_missing を
  上書き。 block->cref を一時 swap。 test_marshal 蘇生。
- **Class#clone が singleton method を copy しない**: 旧コードは
  `def AClass.cm1; end` 後 `MyClass = AClass.clone` で cm1 が消失。
  test_module 0 → 725 pass。
- **bootstrap method_missing recursion**: `self.inspect` が再 raise
  する class で `"undefined method '...' for #{self.inspect}"` 構築中
  に SEGV。 thread-local depth で 4 段以上で `(recursion)`。
- **Float SIGFPE**: `(long)(big_double)` の UB を `korb_dbl2int`
  (Bignum fallback) に。 flt_floor/ceil の n<0 div-by-0 修正。
- **Integer 無限再帰**: `2 ** -2^62` で fixnum overflow → 無限再帰。
- **A::B::C = 0 ** 0 の slot collision** (test_primitive)。
- **Array#cycle / repeated_combination / repeated_permutation を C 実装**
  (bootstrap.rb 版は break が nested block を貫通しなかった)。
- **巨大 index 防御**: korb_ary_aset / EVAL_node_aset / ary_aset で
  `LONG_MAX/sizeof(VALUE)` 超え index に IndexError raise (旧 OOM)。
- **IO.pipe unbuffered**: line-buffered だった → readpartial が無限
  block。 test_io / test_optimization の timeout 解消。
- **kwh_save_slot defaulting + FL_KWARGS peeling** in dispatch_to_method:
  `Method#call(**{})` 等で kwsplat が `*args` に紛れ込まない。
  test_keyword 613 → 791 pass。
- **dispatch arg-count error を ArgumentError に**: 旧 RuntimeError
  だったので assert_raise(ArgumentError) が拾えなかった。
- **String#[]= 実装**: 旧 stub。 IndexError raise 含む。
- **object_id を immediate VALUE に**: 旧 `(long)self / 8` は Fixnum
  小値を全部 0 に潰し Hash#hash 完全破綻。
- **Kernel#Float() に strict 検証**: "xyz" → ArgumentError、
  exception:false opt、 nil → TypeError、 Bignum 受理。
- **sprintf %b の width / precision / 0-pad / negative-prefix**。
- **tu_shim 大幅拡張**: Errno (130 const) / Encoding (.find / 70 alias /
  CompatibilityError 等) / Process (UID/GID/CLOCK_*) / Thread::Queue /
  IO 定数 / File::Constants / RubyVM::AbstractSyntaxTree / Bug /
  Socket / Dir.mktmpdir / Tempfile / FileUtils / MemoryViewTestUtils /
  ARGF / trace_var / caller_locations / refine 部分実装。

詳細は [done.md](./done.md) の「CRuby test/ruby/ 全 135 ファイル sweep」
セクション参照。
- **tenth pass で perfect 化したもの**:
  - chilled string 完全実装 (FL_CHILLED + parse.c の prism flag 読み分け +
    Symbol#to_s も chilled): `string/chilled_string_spec` `string/uplus_spec`
  - sized Enumerator + each redispatch: `hash/transform_values_spec`
  - NameError @name/@receiver は method_missing 経由でも記録:
    `exception/name_error_spec`
  - Object#<=>/initialize_copy/clone/dup 既定 hook: `kernel/comparison_spec`
  - GC モジュール (garbage_collect/disable/enable/start): `gc/{garbage_collect,
    disable,enable,start}_spec`
  - Random.new_seed の uniqueness 改善 + urandom 負サイズ: `random/{new_seed,
    urandom,seed}_spec`
  - Integer#allbits?/anybits?/nobits?/sqrt/try_convert/to_r/rationalize/
    numerator/denominator/ord: `integer/{allbits,anybits,nobits,sqrt,
    try_convert,numerator,denominator,ord}_spec`
  - Float#numerator/denominator/to_r: `float/{numerator,denominator}_spec`
  - Range#to_s/eql?/count(infinity for endless): `range/{to_s,eql,count}_spec`
  - Array#combination/permutation Enumerator + binomial size:
    `array/combination_spec` `array/permutation_spec` (+5)
  - Array#fetch/fetch_values/to_a/to_ary/deconstruct: 各 spec full
  - Hash#flatten/transform_keys{,!}/to_h/sort/replace/deconstruct_keys:
    `hash/{flatten,sort,replace,to_h,deconstruct_keys}_spec`
  - String#each_byte Enumerator/strip!/lstrip!/rstrip!: 各 spec full
  - Symbol#intern/name + inspect bare/quoted 判定 fix: `symbol/{intern,name,
    inspect}_spec`
  - Rational#integer?=false / Rational.new 禁止: `rational/{integer,
    rational}_spec`
  - Set#each Enumerator: `set/each_spec`
  - Signal::EXIT / Kernel module 関数 30 件 private: `signal/list_spec`,
    `kernel/{abort,exit}_spec`
  - Numeric#dup/clone/ceil/floor/round/truncate/fdiv: `numeric/{dup,clone,
    ceil,floor,round,truncate,fdiv}_spec`
  - Module lifecycle hook 既定 / BasicObject 既定 hook
  - Comparable#== identity 短絡 + Float 0.0 + NoMethodError swallow

## 旧現状 (2026-05-08, ninth pass)

- **CRuby `spec/ruby/core/` 14 主要 cat**: ninth pass で perfect 化:
  - `core/numeric/{ceil,floor,round,truncate,fdiv,dup,clone}` (7)
  - `core/integer/{allbits,anybits,nobits,sqrt,try_convert,to_r,rationalize}` (7)
  - `core/string/{lstrip,rstrip,strip,chomp,sum,getbyte,append,to_f}` (8)
  - `core/hash/{flatten,deconstruct_keys,replace,sort,to_h}` (5)
  - `core/array/{fetch,fetch_values,deconstruct,to_ary}` (4)
  - `core/exception/{message,inspect,name}` (3)
  - `core/symbol/{symbol,float,inspect}` (3)
  - `core/range/inspect` (1)
  - `core/comparable/equal_value` (mostly)
  - `core/module/{const_get,extended,included,prepended}` (4)
  - `core/kernel/{Array,Float,Hash,Integer,String}` (5 — private 化)
- **CRuby `spec/ruby/core/` 14 主要 cat (eighth pass)**: **13,320+ pass、 216+ ファイル perfect**
  本セッションで perfect 化したもの一覧 (代表):
  - `core/array/{any,clear,assoc,plus,try_convert,compact,rassoc,at,insert,
    pop,shift,drop,delete,rotate}` (14)
  - `core/hash/{new,try_convert,to_proc,default_proc}` (4)
  - `core/string/{hex,oct,try_convert,plus,include,prepend}` (6)
  - `core/integer/{multiply,plus,uminus,divide,digits,bit_and,bit_or,bit_xor}` (8)
  - `core/proc/{lambda,allocate}` (2)
  - `core/comparable/{lt,gt,gte,lte,clamp}` (5)
  - `core/kernel/{class,raise,instance_of,instance_variable_defined,dup,
    case_compare,throw,respond_to,respond_to_missing}` (9)
  - `core/binding/clone`
  - `core/module/{const_set,class_variable_set,deprecate_constant,lt,lte,gt,gte,
    comparison,private_class_method,public_class_method,attr_reader,
    attr_writer,attr_accessor,attr,private,protected}` (16)
  - `core/numeric/comparison`
  - `core/float/divide`
- **Binding**: **150 pass** (完全互換)。

## 旧現状 (2026-05-08, fifth pass)

- **CRuby `spec/ruby/language/`**: 3,726 pass / 35 perfect (mock-shim slot fix +301)。
- mock-shim slot bug 解消、 lambda opt args、 anon-rest+post 修正、
  hash literal string-key freeze、 missing keyword 全キー列挙、
  Proc#parameters slot indexing、 eval `__FILE__`、 clone(freeze:)、
  proc post-rest extra-drop、 case/in slot 衝突。

## 旧現状 (2026-05-08, fourth pass)

- **自前 test/ruby/**: **24/24 全 OK** (737 件)。
- **CRuby `spec/ruby/language/` (rubyspec, 65)**: **3,404 pass / 40 perfect**。
- **CRuby `spec/ruby/core/`** (13 主要 cat): **12,407 pass、 133 ファイル perfect**。
  本ラウンドで perfect 化: array (assoc/clear/dig/include/plus/take),
  hash (compact/delete/dig/empty/any/fetch/fetch_values/new/reject),
  module (case_compare/protected_instance_methods/public_instance_methods),
  class (new), range (new/hash), float (lt/le/gt/ge/uminus),
  integer (bit_length), string (bytes)。
- **Binding**: **150 pass** (完全互換)。

---

## 旧現状 (2026-05-08, second pass)

- **自前 test/ruby/**: **24/24 全 OK** (ArrayLshiftRedef 解決)。 合計 737 件 全 pass。
- **optcarrot**: CRuby と動作・出力一致。
- **CRuby `test/ruby/` (in-scope 67 ファイル)**: 1,108,357 pass / 77.5%。
- **CRuby `spec/ruby/language/` (rubyspec, 65 ファイル)**:
  **3,402 pass / 3,634 (93.6%)、 40 ファイルが 100% perfect**。
  本ラウンドで rescue / class_variable / yield / for / safe_navigator が
  perfect 化、 shim の evaluate を it block で wrap した波及で +119 pass。
- **CRuby `spec/ruby/core/`** にも改善が波及: Array.try_convert を実装、
  shim の MSpecNegatedExpectation で predicate? 系の should_not 反転を
  正しく扱う (Hash#empty? / Hash#any? が perfect)。
- **CRuby `spec/ruby/core/binding/` + `core/kernel/{eval,binding}_spec`**:
  **150 pass** (Binding 自体は 100%、 残るのは Refinements / IRB の out-of-scope のみ)。
- **CRuby `spec/ruby/core/` 主要カテゴリ**:
  - `kernel`: 6,489 pass / 293 fail / 145 err
  - `string`: 1,800 pass / 1,127 fail / 206 err
  - `array`: 1,171 pass / 436 fail / 67 err
  - `integer`: 869 pass / 172 fail / 183 err
  - `hash`: 400 pass / 97 fail / 33 err
  - `proc`: 195 pass / 60 fail / 25 err
  - `float`: 120 pass / 35 fail / 74 err
  - `symbol`: 117 pass / 69 fail / 31 err
  - `range`: 98 pass / 79 fail / 11 err
  - `binding`: 58 pass / 0 fail / 2 err (irb_spec の IO.popen など out-of-scope のみ)

## §0 範囲外 (project policy / user 指定)

これらは TODO ではなく **scope の外**。 触らない。

| 項目 | 理由 |
|---|---|
| Regexp (`=~` / `/.../` / `match` / `scan`) | astrorge 経由で integrate する方針 |
| Encoding-aware String (`String#encoding`, `force_encoding`, multi-byte succ, m17n) | byte sequence のみ |
| Thread / Mutex / Queue / ConditionVariable / SizedQueue | single-threaded only |
| Fiber Scheduler / Async I/O | 範囲外 |
| ObjectSpace 走査 / 詳細 | excluded |
| GC.* (start / stress 以外の細部) | runtime 内部依存 |
| TracePoint / RubyVM | 範囲外 |
| NaN-boxing | 値表現変更禁止 (project memory) |
| Refinements (`refine` / `using`) | 言語拡張、 範囲外 |
| Ractor | 並列拡張、 範囲外 |
| ruby2_keywords | 互換性 marker、 範囲外 |
| DidYouMean (NoMethodError 提案) | 別途 |
| Random reproducibility (`srand` で seed 一致) | 範囲外 |
| Process / spawn / fork / `ruby_exe` 子プロセス起動 | 範囲外 (子プロセスを介する spec は skip / 0 pass で許容) |
| IRB (`Binding#irb`) | 対話的 IRB は対象外 |

mspec_shim はこの一覧の constant を未定義時に skip 扱いにする。

## §A 完全 perfect 候補 (残 fail/err が 1〜4 件)

「あと数件で 100% pass」 になる language spec。 直近の作業優先度高め。
本ラウンドで rescue / class_variable / yield / for / safe_navigator は
perfect 化済み。

| spec | pass / fail / err | 原因の見当 |
|---|---|---|
| `variables_spec` | 168 / 2 / 0 | lambda 内 eval から外側 lvar 書き込み (lexical scope chain walk + multi-scope prism 連携) |
| `method_spec` | 268 / 5 / 0 | Ruby 3.x の特殊 param 系 (`def m(*, a)`、 `def m(a, **nil)` 等) |
| `class_spec` | 66 / 2 / 2 | Class.new block 内の `class X` の lexical scope (§B3) |
| `super_spec` | 117 / 1 / 2 | BasicObject 経由 super, 可視性変更後 super, define_method 経由の RuntimeError |
| `block_spec` | 180 / 2 / 2 | block の SyntaxError 系 (循環引数参照) と to_proc 周り |
| `metaclass_spec` | 22 / 1 / 1 | metaclass of metaclass (深い singleton chain) |
| `constants_spec` | 142 / 3 / 1 | private constant access、 unicode const name |
| `return_spec` | 51 / 3 / 0 | return inside class block の LocalJumpError、 toplevel return warning |
| `hash_spec` | 66 / 4 / 0 | string key freezing、 Ruby 3.x の `m(**h)` 非コピー特殊規則 |
| `keyword_arguments_spec` | 45 / 8 / 3 | `**hash` empty 扱い (§B2) |
| `regexp_spec` | 43 / 26 / 2 | astrorge 待ち |

## §B 中インパクト項目

### B1. block frame の backtrace ラベル

CRuby は block 内 raise の backtrace を `:in 'block in foo'` (or `:in 'block in <main>'`) と表示する。 koruby は呼び出し元の AST node line を貼るだけで block を独立 frame として表示しない。 `rescue_spec` / `ensure_spec` の "deepest rescue block" 系と、 backtrace API ベースの spec が複数 fail。 backtrace builder で running_block の生成位置を frame として挟む必要あり。

### B2. 空 kwargs hash の自動消失

`m({}, **{})` で空 kwargs hash が positional に流れる現象。 CRuby 3.x は empty kwargs hash を call-site で消去する。 `keyword_arguments_spec` で十数件 fail。 call site の args 構築を kwargs と positional に明示分離する必要あり。

### B3. Class.new block 内の `class X` lexical scope

`Class.new do; class X; end; end` で、 X は block 作成時の lexical scope (= 外側) に作られる。 koruby は `current_block->cref` を nk に push するため X が anon class 配下に入る。 builtins/module.c の `class_new` の cref 操作を見直し。

### B4. SyntaxError message 一致

`-> { eval "..." }.should raise_error(SyntaxError, /pattern/)` で prism と CRuby のエラーメッセージが違うため fail。 mspec_shim 側の substring matcher で半数は救えているが、 「prism は受け付けるが CRuby は SyntaxError」 のケース (例: yield in singleton class) は個別対応必要。

### B5. eval body から外側 block の lvar 更新

`eval("a = 2")` を block 内から呼ぶと、 eval body は新規 lvar `a` を作るだけで block の `a` を更新しない。 prism の depth 1+ scope への書き戻しを実装する必要あり。 `kernel/eval_spec` の "updates a local in a scope above a surrounding block scope" など。

## §C 残小バグ

- [ ] **ArrayLshiftRedef** (`test/test_basic_op_redef.rb`) — Array#<< の redef guard が発火しない (4/4 fail)
- [ ] **Float#floor(n) の Float 精度** — Float 表現の本質 (291.4.floor(2) は 291.39 になる)
- [ ] **proc/lambda の post-rest の parameter 名** が `[:req]` のまま
- [ ] **m17n strings**: encoding を真面目に処理してないので multi-byte 周りで slot wrap や split で誤動作
- [ ] **`def f(&nil)` / `def f(**nil)`** (Ruby 3.4) — PM_MISSING_NODE で吸収済みだが parameters に反映されない

## §D Module / Class 内部

- [ ] `Class#attached_object` (singleton class API)
- [ ] `Module#prepend_features` / `Module#append_features` 公開
- [ ] `Module#const_source_location` の真の実装 (line 番号を保存)
- [ ] `Module#constants(false)` の `inherit` 引数の細部
- [ ] `private constant :X` (定数の visibility)

## §E 標準ライブラリ — 残小物

- [ ] `String#bytesplice` (byte-level 編集)
- [ ] `Struct`: `keyword_init: true`、 `IndexError` on bad index、 `to_h` with block
- [ ] `Data`: 真の Data 実装 (現状 Struct 経由)
- [ ] `Complex` / `Rational`: `coerce` 経由の polar canonicalization、 NaN/Infinity の to_s 表記、 expt special angles
- [ ] `StringIO` クラス自体 (test の依存により $_ 系 spec が skip 多い)

## §F core spec の伸びしろ

`spec/ruby/core/` で **現状 60% 未満かつ実装可能** なクラス。 実装済み
基盤 (Binding / eval / proc / hash) を生かせば数百件単位で増える。

| カテゴリ | pass | fail | err | 備考 |
|---|---:|---:|---:|---|
| `string` | 1800 | 1127 | 206 | encoding 系除外でもまだ伸びしろ大 |
| `array` | 1171 | 436 | 67 | BasicObject splat、 reject_bang の余地 |
| `integer` | 869 | 172 | 183 | Float 精度系 + bignum 細部 |
| `range` | 98 | 79 | 11 | endless range step / first(n) の細部 |
| `symbol` | 117 | 69 | 31 | inspect / to_proc / encoding 関連の細部 |
| `proc` | 195 | 60 | 25 | curry / parameters / arity の細部 |
| `float` | 120 | 35 | 74 | Float 表現本質、 step / divmod 精度 |
| `hash` | 400 | 97 | 33 | merge with block / compare_by_identity |

## §G 過去セッション履歴

実装済み変更の履歴は git log を参照。 手を動かす前に直近 30 commits 程度を
ざっと眺めて既に試した方針を再走しないこと。 直近の大改修:

- 2026-05-07: Binding 完全実装 (binding TOTAL 110 → 150)
- 2026-05-07: Kernel#eval coerce + String#b + block fp shift + nested eval
- 2026-05-06: eval-with-binding (caller の lvars 参照、+99 pass)
- 2026-05-05: mock support (should_receive)、 LocalJumpError 検出
- 2026-05-04: describe→context→describe のローカル破壊修正

## §H precise GC (2026-05-28)

`BARUBY_GC_STRESS=1 BARUBY_GC_PURGE=1` (= per-alloc GC + mprotect ベース
stale-ptr 検出) 下で test 走行:

- 8-test 基本 battery (min1/def1/class1/fib10/method_chain/justmul/
  string_op/test_seq4): **normal / STRESS / STRESS+PURGE 全 8/8 pass**。
- 完全 test/ ディレクトリ (24 ファイル):
  - **normal**: **24/24 (全 OK)**
  - **STRESS**: 23/24 (test_basic_op_redef だけ — libc array Phase 3 未完)
  - **STRESS+PURGE**: 23/24 (同上)。 baseline (548e616a) では STRESS だけで
    3 件 fail だったので大幅改善。 test_eq_redef + test_alias_redef +
    test_basic_op_redef の inspect SEGV、 全て今回の session で解消。

### 残 test_basic_op_redef STRESS の root cause = Phase 3 未完

`korb_ary_new_capa` (= 配列) と `korb_hash_new` (= ハッシュ) は libc
xmalloc で確保 (= GC arena 外)。 visit_roots phase (a) が value stack の
slot を走査して forward_edge を呼ぶ際、 slot 値が libc 配列を指している
と forward_payload は arena obj として扱い:
- aligned = ALIGN8(gc_size=0) = 0 (= memset で 0 のまま)
- memcpy 0 bytes (= no-op だが to_top は事実上 進まない)
- HDR_SET_FORWARDED → libc 配列の gc_flags に bit 立て
- fwd_overlay_set で payload+8 (= klass field!) に to_top を書き込み
- *slot = to_top (= libc 配列の今後の参照先が壊れた arena 領域に向く)

結果、 配列 lvar の値が "to_top に置かれた直近の arena obj" (= 多くの
場合 main_obj) に化ける。 `a.class = Main` の理由。

Phase 3 (= 全 koruby obj を aro_gc_alloc に migrate) もしくは framework
側の forward_payload に "outside from-space and not registered as large
→ immortal as-is" path を入れることで根治。 後者は他 sample (baruby_
precise 等) は arena only で問題ないため framework 改変は影響なし。

### 解消した test_alias_redef NORMAL の logical fail

調査の結果、 `assert_equal` (= optional msg=nil 持ち method) の
prologue_ast_general に `c->current_frame->fp += arg_index;` の
shift が抜けていた pre-existing バグだった。 body が caller の
lvar slot から params を読んでいた。 commit d907a658 で fix。
prologue_ast_simple_inl / prologue_ast_full_inl_K は最初から shift
していて、 general だけ漏れていた。

このセッションの主要 fix (commit 686f01f0 〜 13edbcb7):

- **visit_roots phase (c+d) 統合**: frame 毎に cref chain を walk。
  method dispatch で frame.cref = mc->def_cref (= cref_dup の SEPARATE
  chain) になるため、 head 一本だけ走査では outer frame の cref->klass
  が stale 化。 PURGE で class body 内 const_set が SEGV した直接原因。
- **c->sentinel_frame + c->top_cref を visit_roots で unconditional に walk**:
  korb_eval_string が top_frame.prev=NULL で push するため、 sentinel は
  head chain から外れる。 sentinel.self (= main_obj) が bootstrap 中の
  GC で stale 化し、 user script 側の最初の puts 1 が dispatch SEGV。
- **node_class_def / node_class_reopen / node_module_def を synthetic
  frame push に統一**: 旧 impl は outer frame.current_class / cref を
  その場で書き換え、 C-local prev_class を across-body-GC で保持。
  GC 後に書き戻すと stale arena ptr が outer に書かれ、 次の GC で
  BAD SLOT。 synthetic 構造体 (.cref/current_class/self を inherit)
  を c->current_frame として push する形に統一。
- **korb_dispatch_to_method AST path で frame2 が cref/current_class/
  current_file を inherit**: frame2 の struct literal がそれらを
  初期化せず NULL のまま → body 側 const_lookup が NULL 経由で
  garbage を返して SEGV。
- **node_str_concat を ARO_ROOT_SCOPE で保護**: `korb_to_s_dispatch` が
  GC を発火するため C-local の累積 r と part が across-GC stale。
- **globals 撤去**: `koruby_top_cref` / `koruby_top_sentinel_frame` を
  file-scope static から CTX フィールドに移行。 multi-interpreter
  共存可能に。 struct korb_frame の定義を struct CTX_struct の前に
  移動して by-value embed を可能化。

### 残 pre-existing flakes (baseline からの持ち越し)

- **test_alias_redef** (normal も STRESS+PURGE も fail): const_lookup
  内で c->current_frame が stale stack 領域を指す。 cref->prev =
  korb_dispatch_to_method 内の saved RIP。 どこかの dispatch path が
  c->current_frame を local frame に push して unwind 漏れしている疑い。
- **test_basic_op_redef** / **test_eq_redef** (normal pass、STRESS / STRESS+PURGE で fail):
  GC root scan が visit_ptr_slot に libc code address (= `_int_malloc+N`)
  を渡して SEGV。 cref chain の prev が garbage stack 領域を指し、
  そこから wild ptr へ chain される。 method aliasing が同じ
  `struct korb_method *` を複数 entry に登録するため、 def_cref chain
  に corrupted node が混入する可能性。
