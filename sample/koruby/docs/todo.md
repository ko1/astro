# todo.md — koruby Ruby 互換性ギャップ

[done.md](./done.md) は実装済み機能の一覧。 ここは **未実装 / 不完全 /
既知バグ** の作業リスト。

## 現状 (2026-05-08, sixth pass)

- **自前 test/ruby/**: **24/24 全 OK** (737 件)。
- **CRuby `spec/ruby/language/` (rubyspec, 65)**: **3,745 pass / 190 fail / 51 err / 35 perfect**
  (前 round 比 +19 pass; 累計 +341 pass)。
- **optcarrot**: 30 frames で 85 fps、 動作・出力一致。
- **CRuby `spec/ruby/core/` 14 主要 cat**: **13,186+ pass、 192 ファイル perfect**
  (前 round 比 +47 perfect、 +780 pass)。 本 round で perfect 化した spec:
  - `core/array/{any,clear,assoc,plus,try_convert}`
  - `core/hash/{new,try_convert,to_proc}`
  - `core/string/{hex,oct,try_convert,plus,include,prepend}`
  - `core/integer/{multiply,plus,uminus,divide,digits}`
  - `core/proc/lambda`
  - `core/comparable/{lt,gt,gte,lte,clamp}` (5)
  - `core/kernel/{class,raise,instance_of,instance_variable_defined,dup,
    case_compare,throw,respond_to,respond_to_missing}` (9)
  - `core/binding/clone`
  - `core/module/{const_set,class_variable_set,deprecate_constant,lt,lte,gt,gte,
    comparison,private_class_method,public_class_method,attr_reader(部分)}` (10)
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
