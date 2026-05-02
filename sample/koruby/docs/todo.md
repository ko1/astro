# todo.md — koruby 未実装機能 / 性能向上の今後の課題

[done.md](./done.md) で「あるもの」を、本書では **これから入れるべきもの** を整理する。

現在のテスト pass 率: **42 / 67**（残り 25）。
本書冒頭のセクションで、残ってる **25 個の失敗テストごとの具体的要件** を厚めに書く。
次の作業者はここから着手すれば良い。

---

## 失敗テスト一覧（要件付き）

### A. parser / syntax 拡張系（重い、Prism のノードを増やす必要）

#### 1. `test_kwargs.rb` / `test_kwargs_splat.rb` — キーワード引数

要件:
- `def foo(a:, b: 10)` の **必須/オプション kw 引数**
- `**kwargs` rest
- 呼び出し側 `foo(a: 1, b: 2)` の hash として受け
- `foo(**h)` の splat
- 現状: 通常引数として hash を 1 つ受け取って `wrong number of arguments` で fail

実装ポイント:
- `parse.c` の `def` パラメータ処理 (`PM_PARAMETERS_NODE` の `keywords` / `keyword_rest` フィールド)
- `node_kwarg_required(slot, name)` / `node_kwarg_optional(slot, name, default)` / `node_kwarg_init(slot, name)` ノードを追加
- prologue で「argv の末尾が Hash なら kwargs として剥ぎ、各 kw param の slot に書き込む」処理
- `**rest` slot に残りの hash を入れる
- 呼び出し側: `foo(a: 1)` を `foo({a: 1})` に下げ、prologue 側で「hash 引数」として認識

優先度: 高（実用 Ruby コードで多用）

---

#### 2. `test_forwarding.rb` — `...` forwarding

要件:
- `def f(...); g(...); end`
- caller の引数 (位置 + kw + block) をそのまま callee に転送

実装ポイント:
- `parse.c`: `PM_FORWARDING_PARAMETER_NODE` / `PM_FORWARDING_ARGUMENTS_NODE` 対応
- caller frame から positional args / kwargs / block を「そのまま渡す」 — block_slot や fwd_args slot を hidden lvar として確保
- 呼び出し側で `g(...)` を `g(*fwd_args, **fwd_kw, &fwd_blk)` に展開

優先度: 中（modern Ruby 標準）

---

#### 3. `test_post_rest.rb` — 後置必須 `def f(a, *r, b)`

要件:
- 引数の後ろに固定パラメータ `b`
- 例: `f(1,2,3,4)` で `a=1, r=[2,3], b=4`

実装ポイント:
- `korb_method.u.ast.post_params_cnt` を追加（既に `is_simple_frame` 等同様の cfunc あり）
- prologue で rest_slot 計算後、後ろから post 個取って固定スロットへ
- `parse.c`: `PM_PARAMETERS_NODE` の `posts` フィールドを処理

優先度: 中

---

#### 4. `test_masgn_splat.rb` — splat 入り多重代入

要件:
- `a, *b, c = [1,2,3,4,5]` で `a=1, b=[2,3,4], c=5`
- `a, b = *arr` (右辺 splat)
- ネスト `(a, b), c = ...`

実装ポイント:
- `parse.c` の multi-write 処理で `PM_SPLAT_NODE` ターゲットを認識
- 右辺評価結果の Array を「pre / rest / post」に分割するヘルパ
- ネストは LHS が `PM_MULTI_TARGET_NODE` の時に再帰的 destructure

優先度: 中

---

#### 5. `test_block_destructure.rb` — `((a, b))` ネスト destructure

要件:
- `[[1,2],[3,4]].each { |(a, b)| ... }` で `a=1, b=2`
- 二重括弧 `((a, b), (c, d))` のネスト
- 現状: 一段の auto-destructure だけ動作

実装ポイント:
- `parse.c` の `PM_BLOCK_PARAMETERS_NODE` / `PM_MULTI_TARGET_NODE` をブロック param に対応
- `korb_yield_slow` の destructure ループで recursive 展開

優先度: 中

---

#### 6. `test_pattern_match.rb` — `case x; in pat; ... end`

要件:
- `in [a, b]` (array deconstruct)
- `in {a: x}` (hash deconstruct)
- `in Klass(...)` 
- `in pat if cond` (guard)
- `else` 句

実装ポイント:
- `parse.c`: `PM_CASE_MATCH_NODE` / `PM_*PATTERN*_NODE` 対応
- pattern 評価ノード `node_pat_*` を追加（is_ary_size / ary_bind / ary_get / hash_bind / class match）
- パターンを if/elsif chain に lower
- `else` 句: パターンマッチ全 fail で実行（現状 nil 返してる）

現状: `else` だけ落ちる (5/5)。else 対応だけでもすぐ pass にできるかも。

優先度: 中

---

#### 7. `test_for_loop.rb` — `for x in coll; ...; end`

要件:
- `for x in [1,2,3]; sum += x; end` でループ後 `x` も `sum` も外スコープに残る (block と違いスコープゲートしない)

実装ポイント:
- `parse.c`: `PM_FOR_NODE` 対応
- 内部的に `coll.each { |x| ... }` に下げる（ただしスコープゲートなし、つまり block の env を外側 frame と共有させる）

優先度: 低（ほぼ each で代用されている）

---

#### 8. `test_retry.rb` — `retry` の class filter

要件:
- `begin ... rescue Klass; retry; end` で `Klass` にマッチしたときだけ retry

実装ポイント:
- 現状 retry は実装されてるが、rescue Klass 自体の class match check が緩い (test_exception_class と同根)
- `node_rescue` の rescue_body 評価前に「現在の例外が Klass のサブクラスか」をチェックし、不一致なら propagate

優先度: 中（exception strict と一緒に直すと効率良い）

---

#### 9. `test_defined.rb` — `defined?` 真の実装

要件:
- `defined?(x)` → "local-variable", "method", "expression", "constant", etc. を返す
- 現状: 大半の ケースで "expression" 返してる

実装ポイント:
- `parse.c`: `PM_DEFINED_NODE` を `node_defined(expr_kind, ...)` 経由で
- expr の kind に応じて文字列を返すロジック

優先度: 低（reflection 用途）

---

#### 10. `test_singleton.rb` — `class << obj` / `def obj.foo`

要件:
- `class << x; def special; ...; end; end`
- `def x.special; ...; end`

実装ポイント:
- `parse.c`: `PM_SINGLETON_CLASS_NODE` 対応 (`class << expr; body; end`)
- 内部的に `korb_singleton_class_of_value(x)` で per-object class を作って body をその context で eval
- `def x.foo` は `parse.c` の `PM_DEF_NODE` 処理で `n->receiver` が `PM_LOCAL_VARIABLE_READ_NODE` 等のときに対応

優先度: 中

---

#### 11. `test_source_loc.rb` — Method#source_location 等

要件:
- `def foo; end; method(:foo).source_location` で `[file, line]` を返す
- `caller` の各エントリも `file:line` 形式

実装ポイント:
- `parse.c`: `T()` で各ノードに `head.line` / `head.source_file` を設定（現状は HEAD には未設定）
- Prism の `pm_newline_list` から binary search で line 取得
- `Method` クラスに `source_location` cfunc

優先度: 低（debug 用途）

---

### B. semantics 拡張（中程度の作業）

#### 12. `test_lambda_return.rb` — 真の lambda return

要件:
- `lambda { return 42 }.call` で 42（lambda 内 return は lambda から抜ける、外メソッドへ伝搬しない）
- 現状: Proc と同じく外メソッドまで return が伝搬する

実装ポイント:
- `proc_call` の中で `p->is_lambda == true` のときに KORB_RETURN を catch して値だけ返す
- すでに `proc_call` の末尾で RETURN 消費してるが、is_lambda 判定なしで一律消費している → Proc の non-local return が壊れている可能性も

優先度: 中

---

#### 13. `test_method_to_proc.rb` — `obj.method(:m).to_proc`

要件:
- `[1,2,3].map(&obj.method(:foo))` 経由で動く

実装ポイント:
- `Method#to_proc` cfunc を追加 (現状 `Method#call` のみ)
- 返す Proc は body == NULL かつ `self = method_obj` の form で、yield 時に `method_obj.receiver.send(method_obj.name, *args)`
- すでに Symbol#to_proc は同形式の shim を実装してあるので、それを method_obj 用に拡張

優先度: 低

---

#### 14. `test_visibility.rb` — `private` / `protected` の strict check

要件:
- `private` メソッドを explicit receiver で呼んだら NoMethodError
- `protected` メソッドを external (非継承) クラスから呼んだら NoMethodError

実装ポイント:
- `korb_method` に `visibility` フィールドを追加 (`PUBLIC` / `PRIVATE` / `PROTECTED` enum)
- `module_private` / `module_protected` で当該 method の visibility を更新
- `korb_dispatch_call` で visibility check: PRIVATE は explicit recv なら deny、PROTECTED は caller class が target class の hierarchy にいるかチェック
- mc->visibility byte cache で hot path 1 byte read で判定

優先度: 中

---

#### 15. `test_exception_class.rb` — `rescue Klass` の strict match

要件:
- `rescue ArgumentError` で `TypeError` をキャッチしないこと
- 例外オブジェクトが Klass の is_a? を満たすときだけマッチ

実装ポイント:
- 現在の `node_rescue` は rescue_body を unconditional に実行
- parser で rescue 句を `if exc.is_a?(K1) || exc.is_a?(K2); rescue_body; else; reraise; end` に lower
- または `node_rescue` に `rescue_classes` を持たせて runtime チェック

優先度: 中（よく使われる）

---

#### 16. `test_define_method.rb` — proc body の closure capture

要件:
- `make_class(prefix) { k = Class.new; k.define_method(:greet) { |n| "#{prefix}, #{n}" }; k }` で `prefix` を block の env から拾えるように

実装ポイント:
- 現実装は proc->body を AST メソッドとして登録するだけで env を drop している
- proc の env を method invocation 時に prologue の fp に snapshot copy する path が必要
- または `KORB_METHOD_PROC` 型を追加し、dispatch 時に `proc_call` ベースで起動 (env を持ち出せる)
- param_base 問題（block の param_base != 0 だと fp[0] arg と block の expected slot がズレる）も解決必要

優先度: 中

---

#### 17. `test_method_missing.rb` — method_missing 機構

要件:
- 既存：method_missing は実装したが、test の `Tracer` 系で fail
- 詳細確認必要 (`undefined method 'foo' for Tracer` — method_missing 呼ばれてない？)

調査: `./koruby test/ruby/class/test_method_missing.rb 2>&1`

実装ポイント:
- 現実装は class find_method 失敗時に method_missing を呼ぶ (object.c の korb_dispatch_call)
- mc が cached されると find_method が走らない → mc キャッシュの判定で method_missing path に行かない可能性
- mc->method == NULL のとき method_missing 呼ぶよう修正

優先度: 中

---

#### 18. `test_prepend_extend.rb` — `prepend` 真の MRO

要件:
- `prepend M` で M のメソッドが class 自身より **先** に lookup される

実装ポイント:
- 現状 prepend = include 同等にしている (no real method-resolution-order rewiring)
- `korb_class.prepends[]` を追加し、`korb_class_find_method` の lookup 順を `prepends → self → includes → super` にする

優先度: 低（include と挙動似ていれば test 部分通る）

---

#### 19. `test_misc_features.rb` — `redo` / `BEGIN` / `END` / 細かい

要件:
- `redo` で while/each ループの現 iteration をやり直し
- `BEGIN { ... }` / `END { ... }` ブロック (`END` は既に at_exit_procs として実装ありかも)
- その他 misc

実装ポイント:
- `node_redo` (state KORB_REDO 用): `node_while` / `korb_yield` で iteration の先頭に jump
- `BEGIN`: parse 時に program 先頭に prepend
- `END { ... }`: at_exit hook (`koruby_vm.at_exit_procs[]` 経由)、main.c の終了時に逆順実行

現状: `redo` で 4 期待が 3 (1 回 redo されてない)。

優先度: 低

---

### C. 細かいバグ系（軽い）

#### 20. `test_object_basic.rb` — equal? / hash content base

要件:
- `s.equal?(s.dup)` → false (object identity)
- `[1,2].hash == [1,2].hash` → true (content-based hash)

実装ポイント:
- 現状の `Object#equal?` は object_id 比較。`Object#object_id` は `(long)self / 8` で stale なメモリ再利用や string literal pooling で衝突
- 真の object_id テーブル (object → unique id) が理想だが、簡易には `(uintptr_t)self` をそのまま返せばよい
- `Array#hash` は kernel_object_id stub。FNV-1a 等の content-based hash に
- `String#hash` は str_hash あり、content-based のはず（要確認）

優先度: 低

---

#### 21. `test_string_extra.rb` — String#slice

要件:
- `"hello".slice(1, 3)` → `"ell"`
- `"hello".slice(0)` → `"h"`
- 現状: 一部 fail (2/17)

調査: 直近修正で bootstrap.rb の slice を `def slice(a, b = nil); b ? self[a, b] : self[a]; end` に書き換え済み。`self[0]` が `"h"` を返さない可能性。

実装ポイント:
- `str_aref(self, [int])` で start=int, len=1 として 1 文字返す挙動
- 現状 `"hello"[0]` は何返す？ `nil`?

優先度: 低

---

#### 22. `test_ivar_introspect.rb` — instance_variables 詳細

要件:
- `obj.instance_variables` で `@x` (Symbol) のリスト
- 現状: 1 個 fail (`test_instance_variables_list`)

調査必要。私の `obj_instance_variables` は class.ivar_names を全部返す（実際 set されたか問わず）。set 済みのみ返すべき。

実装ポイント:
- `korb_object` の ivars が "set 済み" を区別できるよう、Qundef を unset、それ以外を set として扱う

優先度: 低

---

#### 23. `test_backtrace.rb` — backtrace 内容

要件:
- 例外オブジェクトの `backtrace` で `["file:line:in 'method'", ...]` 形式の配列
- 現状: backtrace は空配列を返す stub

実装ポイント:
- `korb_raise` で例外オブジェクトの `@__backtrace__` ivar に `caller`-相当の配列を set
- `caller` 自体も `(eval):in '<name>'` 形式で行番号無し → `node->head.line` を parse 時に設定する必要 (上記 #11 と関連)

優先度: 低

---

#### 24. `test_begin_end_eval.rb` — `eval`

要件:
- `eval("1 + 2")` で 3 を返す
- `BEGIN { ... }` (test_misc と被る)

実装ポイント:
- `eval` を真に実装するには：
  - 文字列を `koruby_parse` でパース
  - 現フレームの fp を env にした proc 風実行
- これは大きい (current_class / cref 等の context 復元も必要)

優先度: 低

---

#### 25. `test_pack_unpack.rb` — String#unpack 完全

要件:
- `[1,2,3,4].pack("n*")` で 4 byte 列
- `str.unpack("n*")` で逆変換
- 現状: pack はある程度実装、unpack 未実装

実装ポイント:
- `str_unpack(format)` cfunc を追加
- フォーマット文字 (C/c/s/S/i/I/n/v/N/V/l/L/q/Q/a/A/Z/H/h/x/d/E/G/f/e/g) のパース + Array に詰める

優先度: 低

---

#### 26. `test_rational_complex.rb` — Imaginary literal

要件:
- `2i`、`3.5i` リテラル
- 現状: `[koruby] unsupported node: PM_IMAGINARY_NODE`

実装ポイント:
- `parse.c`: `PM_IMAGINARY_NODE` 対応 → `Complex(0, num)` に lower

優先度: 低

---

## 言語仕様の充足のために（汎用課題、上記と一部重複）

### A. パラメータ受け側の拡張 (最重要)
- `**kwargs` — keyword arguments [上 #1]
- 後置必須 `def foo(a, *r, b)` [上 #3]
- ブロック引数の destructure `each { |(a, b)| ... }` [上 #5]
- forwarding `...` [上 #2]

### B. クラスシステム
- 真の `include` (mixin チェイン挿入。現在は flatten copy method lookup; ancestors / is_a? は対応済み)
- `Module#prepend` の真の MRO [上 #18]
- 特異クラス (`class << obj`)、特異メソッド (`def obj.method`) [上 #10]
- `private` / `protected` の真の可視性チェック [上 #14]

### C. Proc / Lambda
- 真の lambda return [上 #12]
- `Method` / `UnboundMethod` クラス
- `Method#to_proc` [上 #13]
- proc body の closure capture [上 #16]

### D. 多重代入
- splat 入り `a, *b, c = arr` [上 #4]
- ネスト `(a, b), c = ...` [上 #4]
- 多重代入の右辺 splat `a, b = *arr` [上 #4]

### E. 文字列周り
- 真の **正規表現** (Regexp / `Regexp.new` / `=~` / `match` / `scan` / `gsub(re)` / `sub(re)`) — astrorge 待ち
- ヒアドキュメント (`<<~END` / `<<-END` / `<<END`)
- `String#%` の高度な書式 (`%-10s`、精度、`*` width など)
- マルチバイト / Encoding (現状 ASCII バイト前提)
- `String#unpack`、`Array#pack` のフォーマット文字を全種 [上 #25]

### F. I/O / OS
- `File.open` / 読み書きストリーム / `each_line` / `gets` / `puts` (stream に対するもの)
- `IO.popen` / `IO.pipe` / リダイレクション
- `Dir.glob` / `Dir.entries` / `Dir.pwd`
- `Process.fork` / `Process.wait` / `system`
- `Time` クラス (`Time.now` のみ stub)
- `STDOUT.sync = true` の意味づけ

### G. その他のリテラル / 演算
- `Rational` / `Complex` の **リテラル** `2i` `3r` (Rational は `3r` まで通る、Complex 未対応) [上 #26]
- 文字列 `%w[...]` / `%i[...]` (要確認)
- 数値リテラルの `_` 区切り (パーサ任せ; 動作確認のみ)
- `defined?` の真の実装 [上 #9]
- パターンマッチ (`case x; in [a, b]; ... end`) [上 #6]

### H. 制御構造
- `for x in coll; ...; end` [上 #7]
- `redo` [上 #19]
- `BEGIN { }` / `END { }` [上 #19, #24]
- `__dir__` / `__FILE__` / `__LINE__` 厳密化

### I. リフレクション
- `Object#methods` / `private_methods` / `instance_methods`
- `ObjectSpace.each_object`
- `binding` / `eval` [上 #24]
- `Kernel#caller` (stub あり、行番号未対応) [上 #23]
- backtrace を例外オブジェクトに付与 [上 #23]

### J. 並行性
- `Fiber` (実装済み、ucontext)
- `Thread` (現状は不要)
- `Mutex` / `Queue`

### K. 外部依存
- `require "stackprof"` 等の gem は **Qfalse** で無視している。最低限スタブは必要かも

---

## 性能向上のための課題

詳細は [perf.md](./perf.md) 参照。

### 1. ✗ メソッド呼出のさらなるインライン化 (PG-baked call_static)
abruby / naruby と同様、**プロファイル情報を使って call site に呼出先 SD を直接焼き込む** 必要がある。

実装の土台は **既にある**: `prologues.h` に `prologue_ast_simple_static_inl(...)` を追加済み (引数で dispatcher を取る変種)。残りは:

- `node_specialize.c` の `SPECIALIZE_node_func_call` / `SPECIALIZE_node_method_call` を改修
- `cs_compile_all` の context で flag を立てる
- mc が AST method を指していれば、その body の hash を取得し、特化 SD で `prologue_ast_simple_static_inl(c, n, recv, argc, ai, blk, mc, PARAMS, SD_<body_hash>)` を emit
- これにより `mc->dispatcher` の indirect call が direct call になり、gcc が body 全体を inline できる

期待効果: optcarrot で 110 fps → 150 fps 級 (YJIT 175 fps に近づく)

### 2. ✗ RESULT 構造体化
現在 `CTX::state` を毎 EVAL_ARG ごとにメモリから読む。abruby のように `RESULT { VALUE, state }` を 2 レジスタ返り値にすれば、メモリアクセスが減る。

dispatcher signature の変更を伴う大規模リファクタ。
- `typedef struct { VALUE v; int state; } RESULT;`
- すべての `node_*` を RESULT 返すように
- `EA(c, n)` macro: `RESULT _r = EVAL_ARG(...); if (UNLIKELY(_r.state)) return _r;`
- x86_64 SysV ABI で 16 バイト struct は rax/rdx 2 レジスタ返り

期待効果: 各 EVAL_ARG 後の `c->state` load を register check に変える。fib で 5-10% 速くなる見込み。

### 3. ✗ ノード型に基づく具象 dispatcher (swap_dispatcher)
- `node_plus` を `node_fixnum_plus` / `node_float_plus` / `node_str_plus` などに **実行時に rewrite**
- abruby 既に実装済み (`swap_dispatcher`)

期待効果: 型固定で C コンパイラ最適化。整数 heavy ベンチで 10-15% 改善。

### 4. ✗ FLONUM 即値化
**HEAD には既に実装あり** (`context.h` の `korb_double_to_flonum` / `korb_flonum_to_double`)。
ただし全 Float リテラル/結果が FLONUM 化されているか要確認。

### 5. ✗ Array/Hash/String の専用 backing
- Array: small array (3 要素以下) を inline (CRuby embed array)
- Hash: open-addressing + Robin-Hood (現状 linked-list bucket walk が hot)
- String: short string を inline

### 6. ✗ メソッドキャッシュの polymorphic 対応
現在は monomorphic (1 entry)。`mc->klass[2]` 程度の polymorphic IC。

### 7. ✗ Boehm から自前 GC へ
Boehm は便利だが mark コストが大きい。世代別 / heap-tagged GC へ。

### 8. ✗ Self-tail-call optimization
末尾再帰を while ループに書き換え。fib のような自己再帰でも効くが、本質的には method-call dispatch を消す方向 (= 1) が筋。

### 9. ✗ Specialization の永続化 (Code Store)
**HEAD で部分実装済み** (`code_store.c`): `--aot-compile` で `code_store/all.so` 生成、起動時 dlopen。
残: 多言語対応・hash 安定性。

### 10. ✗ JIT モード
現在は AOT 限定。execution 中に hot な NODE を非同期コンパイルする L0/L1/L2 デーモン構成 (naruby は実装済み) を移植。

---

## 短期 ToDo (次セッションで取り組む順)

1. **kwargs** (test_kwargs / test_kwargs_splat) — 利用範囲広い
2. **post_rest** + **masgn_splat** — Ruby らしさ
3. **rescue Klass の strict match** (test_exception_class / test_retry) — 既存テストの 2 つを一気に通す
4. **PGSD (PG-baked call_static)** — perf.md「YJIT 越え」の本丸。土台はある (`prologue_ast_simple_static_inl`)
5. **真の Regexp** (astrorge integrate) — String#scan / =~ / match / gsub(re)

これらを順に潰せば pass率 50+ → 60+ → optcarrot perf も YJIT 圏内。

---

## ドキュメント / テスト

- `test/ruby/<category>/test_*.rb` で 67 テスト整備済み (CRuby 風)
- ベンチマークスイート (loop / call / prime_count / optcarrot / SciMark) → `bm/` にある
- 性能回帰検出 (CI 連動) — 未着手

---

## 既知のバグ / 注意点

- **block 内の `return` は外メソッドに伝搬しない** (non-local return 未実装)。bootstrap の Enumerable 系 helper はすべて `&blk` + `blk.call(...)` + `break` の form で書き直してある
- `is_simple_frame` 判定で `__method__` / `__callee__` / `caller` を含むメソッドは強制的に non-simple (frame push) にしている
- `Exception` を T_OBJECT 化したので、HEAD の `struct korb_exception` layout は廃止。message は `@message` ivar
- `Class.new(super) { ... }` の block 内 def は new class の context で eval される (cref 切替)
- `bootstrap_src.c` は `bootstrap.rb` を bytes 配列化したもの。Makefile の rule で自動再生成

