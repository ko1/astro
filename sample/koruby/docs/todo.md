# todo.md — koruby Ruby 互換性ギャップ

[done.md](./done.md) は実装済み。 ここは **未実装 / 不完全 / 既知バグ** の作業
リスト。

現状 (2026-05-07):
- koruby 自前 test/ruby/ 全 pass (24 ファイル, 190 件)
- optcarrot は CRuby と一致 (動作・出力)
- CRuby `test/ruby/` (in-scope 67 ファイル): **1,108,357 pass / 77.5%**
- CRuby `spec/ruby/language/` (rubyspec, 65 ファイル): **3,275 pass / 92.9%、
  35 ファイルが 100%** (Binding object 完全実装で +28 pass)
- CRuby `spec/ruby/core/binding/` + `core/kernel/{eval,binding}_spec`:
  **150 pass** (Binding 100% — local_variable_get/set/defined?/local_variables /
  receiver / eval / source_location / dup / clone)、 残るのは Refinements
  (out of scope) と IRB (out of scope)。

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

mspec_shim はこの一覧の constant を未定義時に skip 扱いにする。

## §A rubyspec 由来の高インパクト項目

rubyspec sweep の伸びしろ。 §A1〜B8 は前 todo 版から大半を実装済み (本セッ
ション)。 残るのは下記。

### A7. block in index assignment (`assignments_spec` 数件)

`obj[*args, &blk] = v` を prism は SyntaxError にしているが、 spec は
`-> { eval ... }.should raise_error(SyntaxError, /pattern/)` で「 pattern が
合うか」を見る。 SyntaxError は出るがメッセージが CRuby と違うため fail。
mspec_shim 側の substring matcher で吸収済みのケースもあるが、 トリッキーな
SyntaxError 群は個別調整が必要。

### A9. lexical scope const lookup (constants_spec ~10 fail)

`fixtures/constants.rb` 経由のテストで、 多階層 include / 動的 const 追加 /
"Object 名前空間 vs 明示再オープン" の細かな差で残 fail。 大半は cref 連鎖
の捕捉が parse 時の class/module nesting に追従していないことに起因。

### A10. ensure block の backtrace 整形

`ensure_spec` の "does not introduce extra backtrace entries" ↔ block frame の
名前 ('block in <main>' vs 'it')。 backtrace builder が block 実行を独立
frame として表示しないのが原因。 cosmetic 1 件。

## §B 中インパクト項目

### B2. `at_exit` / END (`END_spec` 23 fail = 0%)

`at_exit { ... }` ハンドラの登録 + main 終了時の LIFO 実行は実装済みだが、
`END_spec` は `ruby_exe` で子プロセスを起動して標準出力を比較する形なので
B6 (Process / spawn) との合わせ技が必要。 mspec_shim の `ruby_exe` を実装
すれば一気に通る見込み。

### B3. Hash literal `**hash` の empty 扱い

`m({}, **{})` で空 kwargs hash が positional に流れる。 CRuby は 3.x で
empty kwargs hash を消す。 `keyword_arguments_spec` で十数件 fail。 
call site の args 構築を kwargs と positional に明示分離する必要あり。

### B4. SyntaxError message 一致

`-> { eval "1 rescue RuntimeError 2" }.should raise_error(SyntaxError)` 等で
我々 (prism) のメッセージと CRuby の差で fail するケース。 mspec_shim 側
の substring matcher で半数は救えているが、 「prism は受け付けるが CRuby
は SyntaxError」 のケースは個別対応必要。

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

100% に近い + 残ファイルが小さい spec 群:

| spec | 残 fail | コスト感 | 備考 |
|---|---:|---|---|
| `line_spec` | 1 | 中 | `__LINE__` in loaded file の細部 |
| `class_variable_spec` | 1 | 中 | overtaken ancestor の RuntimeError 警告 |
| `symbol_spec` | 2 | 中 | null char in symbol name parser |
| `ensure_spec` | 2 | 高 | backtrace 中の `'block'` ラベル (§A10) |
| `precedence_spec` | 4 | 中 | 大半は `~/regex/` 系 (regex 統合待ち) |
| `array_spec` | 4 | 高 | BasicObject 経由 splat、 `arr[*splat] = []` |
| `proc_spec` | 6 | 中 | proc/block 引数 destructure の `(a, b)` パターン |

## §G 過去セッション履歴 (アーカイブ)

実装済み変更の履歴は本書 §G 以降と git log を参照。 手を動かす前にここを
ざっと見て、 既に試した方針を再走しないこと。

(以前の §G セッション内容は git log で参照可能 — todo.md を肥大化させない
ためここでは省略。)
