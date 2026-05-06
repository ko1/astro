# todo.md — koruby Ruby 互換性ギャップ

[done.md](./done.md) は実装済み。 ここは **未実装 / 不完全 / 既知バグ** の作業
リスト。

現状 (2026-05-06):
- koruby 自前 test/ruby/ 全 pass (24 ファイル, 190 件)
- optcarrot は CRuby と一致 (動作・出力)
- CRuby `test/ruby/` (in-scope 67 ファイル): **1,108,357 pass / 77.5%**
- CRuby `spec/ruby/language/` (rubyspec, 65 ファイル): **2,406 pass / 68.0%、
  17 ファイルが 100%**

## §0 範囲外 (project policy / user 指定)

これらは TODO ではなく **scope の外**。 触らない。

| 項目 | 理由 |
|---|---|
| Regexp (`=~` / `/.../` / `match` / `scan`) | astrorge 経由で integrate する方針 |
| Encoding-aware String (`String#encoding`, `force_encoding`, multi-byte succ, m17n) | byte sequence のみ |
| Thread / Mutex / Queue / ConditionVariable / SizedQueue | single-threaded only |
| Fiber Scheduler / Async I/O | 範囲外 |
| Process / fork / spawn / `assert_separately` 等 | excluded |
| Signal / trap | excluded |
| ObjectSpace 走査 / 詳細 | excluded |
| GC.* (start / stress 以外の細部) | runtime 内部依存 |
| TracePoint / RubyVM | 範囲外 |
| NaN-boxing | 値表現変更禁止 (project memory) |
| Refinements (`refine` / `using`) | 言語拡張、 範囲外 |
| Ractor | 並列拡張、 範囲外 |
| ruby2_keywords | 互換性 marker、 範囲外 |
| DidYouMean (NoMethodError 提案) | 別途 |
| Random reproducibility (`srand` で seed 一致) | 範囲外 |

mspec_shim はこの一覧の constant を未定義時に skip 扱いにする。

## §A rubyspec 由来の高インパクト項目

rubyspec sweep (2,406 / 3,539) を上げるのに効くもの。

### A1. Lambda strict arity / Proc loose arity の区別 (~50 fail)

`yield_spec` / `proc_spec` / `send_spec` / `keyword_arguments_spec` /
`return_spec` の "raises ArgumentError" が大量に fail。

- lambda は `argc != params_cnt` で `ArgumentError`
- proc は緩く受ける (auto-destructure / nil pad)
- `define_method(name, &lambda)` も lambda として strict
- `Method#call` も strict

ある程度 proc.c に書いてあるが分岐が網羅できていない。

### A2. 非ローカル return (`return_spec` ~38 fail)

`def m; -> { return 1 }.call; 2; end` の `return` が m から抜ける semantics。
proc.c で KORB_RETURN を lambda は consume / proc は propagate に分けては
あるが、

- ブロックに渡された proc (`each(&proc)`) からの return → 元の method 抜け
- block scope 死後の return → `LocalJumpError`
- chained call 経由の return

の網羅が不十分。`korb_proc` に "return target frame" を持たせてマーキングが必要か。

### A3. defined? の細部 (~80 fail 残)

- `defined?($&)` `defined?($\``)` `defined?($+)` `defined?($_)` 等を
  `"global-variable"` で返す (Regexp 系は実体が無くても "global-variable" 文字列は返すべき)
- `defined?(super)` の真の存在チェック (現在は常に `"super"` 返す。 親クラスの
  メソッドが無ければ `nil`)
- private/protected 受信者経由 → 本来 `nil` だが `"method"` を返してしまう
  (respond_to? の visibility filter が無い)
- `defined?(A::B)` で例外を rescue した時の挙動

### A4. Module include の動的伝播 (`constants_spec` 50+ fail)

サブクラスを作ったあとに親 module へ const を追加 / `include` した時に
サブクラスから見えない (現在は flatten copy で固める)。

CRuby は include を「リンク」として持ち lookup 時に walk する。 koruby も
walk 自体はあるが、include 後の const 追加が見えない。

### A5. local_variables / Method / UnboundMethod (~25 fail)

- `Kernel#local_variables` — 現フレームの lvar 名一覧。 koruby は parse 時に
  名前を捨てているので、 method が touch する lvar 名 table を `korb_method`
  に保持する必要あり。
- `Method` class / `UnboundMethod` — `obj.method(:m)`、 `m.bind` / `unbind` /
  `arity` / `owner` / `parameters` / `super_method`
- `for_spec` / `match_spec` の半数は local_variables 起因

### A6. nested multi-target destructure

`((a, b), c), d = expr` の grouped LHS。 prism 上は PM_MULTI_TARGET_NODE が
PM_MULTI_WRITE_NODE の lefts/rest/rights に nested される。 現状 lefts/rights
の各要素が PM_MULTI_TARGET_NODE のときに recurse していない。

`variables_spec` の "grouped LHS without splat / with splats" 系 ~10 fail。

### A7. block in index assignment (`assignments_spec` 数件)

`obj[*args, &blk] = v` を prism は SyntaxError にしているが、 spec は
`-> { eval ... }.should raise_error(SyntaxError, /pattern/)` で「 pattern が
合うか」を見る。 SyntaxError は出るがメッセージが CRuby と違うため fail。
mspec_shim 側の substring matcher で吸収済みのケースもあるが、 トリッキーな
SyntaxError 群は個別調整が必要。

### A8. evaluation order (assignments_spec, variables_spec)

`((m1, m2) = receiver_expr, ...)` の左→右評価順。 現状 RHS 先評価のケースあり。

### A9. lexical scope const lookup (constants_spec ~10 fail)

block / lambda 内の const lookup が lexical scope を辿らない。

```ruby
class A
  C = :a_c
  -> { p C }.call  # :a_c
end
```

cref は proc に capture しているので動くはずだが、 一部のケース (block
literal at module level、 nested class) で resolution が失敗する。

### A10. ensure block の backtrace 整形

`ensure_spec` の "does not introduce extra backtrace entries" ↔ block frame の
名前 ('block' vs 'it')。 cosmetic だが backtrace format 次第で fail。

## §B 中インパクト項目

### B1. Lambda の `|*|` で N args を受ける

```ruby
l = lambda { |*| 1 }
l.call(1, 2, 3)   # ArgumentError: 0 expected ← bug
```

anonymous splat (`|*|`) のときに rest_slot が立っていない。 `proc { |*| }` は
動く。 lambda 経路の差。

### B2. `at_exit` / END (`END_spec` 23 fail = 0%)

`at_exit { ... }` ハンドラの登録 + main 終了時の LIFO 実行。

### B3. Hash literal `**hash` の empty 扱い

`m({}, **{})` で空 kwargs hash が positional に流れる。 CRuby は 3.x で
empty kwargs hash を消す。 `keyword_arguments_spec` で十数件 fail。

### B4. SyntaxError message 一致

`-> { eval "1 rescue RuntimeError 2" }.should raise_error(SyntaxError)` 等で
我々のメッセージが CRuby と違う。 「我々の方が valid と認識する」ケースもあり、
prism のエラーリカバリ次第。

### B5. Exception API

- `Exception#set_backtrace` (現状 stub)
- `Exception#backtrace_locations` (未実装)
- `Exception#full_message` の format 詳細
- `Exception#detailed_message` の color: highlight: kwargs

### B6. Class redefinition の TypeError

```ruby
class C < Object; end
class C < BasicObject; end  # → TypeError: superclass mismatch
```

現状 silent。 `class_spec` 数件。

## §C 残小バグ

- [x] ~~**Boehm GC 再帰クラッシュ** in test_exception~~
- [x] ~~**block param `*x` splat が Fiber 越しに値消失**~~
- [ ] **Float#floor(n) の Float 精度** — Float 表現の本質 (291.4.floor(2) は 291.39 になる)。
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

## §F 完全 pass を狙えそうな spec

100% に近い + 残ファイルが小さい spec 群。 上から順に手を入れると効率良し:

| spec | 残 fail | コスト感 | 備考 |
|---|---:|---|---|
| `line_spec` | 1 | 中 | `__LINE__` in loaded file の細部 |
| `throw_spec` | 0 (1 skip) | — | 既に 100% (Thread skip 含む) |
| `class_variable_spec` | 2 | 中 | toplevel での `@@cvar` RuntimeError、 overtaken ancestor RuntimeError |
| `ensure_spec` | 2 | 高 | backtrace 中の `'block'` ラベル |
| `symbol_spec` | 3 | 中 | null char in symbol name parser、 Encoding 1 件 |
| `array_spec` | 4 | 高 | BasicObject 経由 splat、 `arr[*splat] = []` |
| `precedence_spec` | 10 | 中 | `~/regex/` 系 (regex 統合待ち) を除けば近い |
| `for_spec` | 10 | 中 | `local_variables` (§A5) と attribute writer の細部 |
| `class_variable_spec` | 2 | 中 | RuntimeError 系 |

## §G 過去セッション履歴 (アーカイブ)

実装済み変更の履歴は本書 §G 以降と git log を参照。 手を動かす前にここを
ざっと見て、 既に試した方針を再走しないこと。

(以前の §G セッション内容は git log で参照可能 — todo.md を肥大化させない
ためここでは省略。)
