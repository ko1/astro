# todo.md — koruby 未実装機能 / 性能向上の今後の課題

[done.md](./done.md) で「あるもの」を、本書では **これから入れるべきもの** を整理する。

## 言語仕様の充足のために

### A. パラメータ受け側の拡張 (最重要)
- `*args` (rest) — 残りを Array で受ける
- `**kwargs` — keyword arguments
- `&blk` — Proc としてブロックを受ける
- デフォルト引数 `def foo(a = 10)`
- 後置必須 `def foo(a, *r, b)`
- ブロック引数の destructure `each { |(a, b)| ... }`
- forwarding `...`

これらは **optcarrot を実行するためにほぼ必須**。

### B. クラスシステム
- 真の `include` (mixin チェイン挿入。現在は flatten copy)
- `extend` (singleton class への include)
- 特異クラス (`class << obj`)、特異メソッド (`def obj.method`)
- `define_method`
- `method_missing`
- `Comparable` mixin (`<=>` を 1 つ書けば `< <= > >= == between?`)
- `Enumerable` mixin (`each` を書けば `map select reduce ...` が同期)
- `private` / `protected` の真の可視性チェック
- `Module#prepend`
- `Module.new { ... }`

### C. Proc / Lambda
- 真の lambda return (`return` でラムダ自身から脱出、enclosing メソッドからは脱出しない)
- 真の `&:method` (Symbol#to_proc) — 動的に AST を生成して body にする方式が必要
- `Method` / `UnboundMethod` クラス
- `&proc` で Proc をブロックとして渡す

### D. 多重代入
- splat 入り `a, *b, c = arr`
- ネスト `(a, b), c = ...`
- 多重代入の右辺 splat `a, b = *arr`

### E. 文字列周り
- 真の **正規表現** (Regexp / `Regexp.new` / `=~` / `match` / `scan` / `gsub(re)` / `sub(re)`)
- ヒアドキュメント (`<<~END` / `<<-END` / `<<END`)
- `String#%` の高度な書式 (`%-10s`、精度、`*` width など)
- `String#scan`、`String#match`、`String#=~`
- `String#each_line` (本物の改行分割)
- マルチバイト / Encoding (現状 ASCII バイト前提)
- `String#unpack`、`Array#pack` のフォーマット文字を全種

### F. I/O / OS
- `File.open` / 読み書きストリーム / `each_line` / `gets` / `puts` (stream に対するもの)
- `IO.popen` / `IO.pipe` / リダイレクション
- `Dir.glob` / `Dir.entries` / `Dir.pwd`
- `Process.fork` / `Process.wait` / `system`
- `Time` クラス
- `ENV` の本物の実装 (現在は空 Hash)
- `STDOUT.sync = true` の意味づけ

### G. その他のリテラル / 演算
- 範囲 `(1..)` / `(..10)` (endless / beginless)
- `Rational` / `Complex`
- 文字列 `%w[...]` / `%i[...]`
- 数値リテラルの `_` 区切り (パーサ任せ; 動作確認のみ)
- `defined?` の真の実装
- パターンマッチ (`case x; in [a, b]; ... end`)

### H. 制御構造
- `for x in coll; ...; end` (現状 `[koruby] PM_FOR_NODE not yet supported` を出す)
- `retry` / `redo`
- ラベル付き break/next (Ruby にはないがロジックの分岐補助)
- `BEGIN { }` / `END { }`
- `__method__` / `__callee__`

### I. リフレクション
- `Object#methods` / `private_methods` / `instance_methods`
- `ObjectSpace.each_object`
- `binding` / `eval`
- `Kernel#caller` (バックトレース)
- backtrace を例外オブジェクトに付与

### J. 並行性 (省略可能)
- `Fiber` (ucontext or makecontext で実装)
- `Thread` (現状は不要)
- `Mutex` / `Queue`

### K. 外部依存
- `require "stackprof"` などの gem は **現状すべて Qfalse** で受けている。最低限スタブは必要かも

## 性能向上のための課題

### 1. ✗ メソッド呼出のさらなるインライン化 (PG-baked call_static)
abruby / naruby と同様に、**プロファイル情報を使って call site に呼出先 SD を直接焼き込む** 必要がある。これにより:
- インラインキャッシュ check が消える
- C コンパイラがメソッド本体まで完全インライン展開

これが入れば fib(35) で YJIT 並 (0.15s) を狙える。今は AOT で 0.24s。

### 2. ✗ RESULT 構造体化
現在 `CTX::state` を毎 EVAL_ARG ごとにメモリから読む。abruby のように `RESULT { VALUE, state }` を 2 レジスタ返り値にすれば、メモリアクセスが減る。

### 3. ✗ ノード型に基づく具象 dispatcher
- `node_plus` を `node_fixnum_plus` / `node_float_plus` / `node_str_plus` などに **実行時に rewrite** することで、各 SD が型固定で書けて C コンパイラ最適化が効きやすくなる。
- abruby は既にこの方式 (`swap_dispatcher`)。

### 4. ✗ FLONUM 即値化
現在 Float は常にヒープ。CRuby と同じ FLONUM (低位 0b10) で即値化すれば、Float ヘビーなベンチで数倍のスピードアップが見込める。

### 5. ✗ Array/Hash/String の専用 backing
現在は素朴な `VALUE *ptr; long len; long capa;`。
- Array: small array (3 要素以下) を inline に、`ary->ptr` を構造体内に持つ (CRuby の embed array に倣う)
- Hash: open-addressing + Robin-Hood で線形探索を排除 (現状 hash bucket は使ってるが「片方向片リンクリスト経由で線形に走る」ロジックも残存)
- String: short string を inline に

### 6. ✗ メソッドキャッシュの polymorphic 対応
現在は monomorphic (1 entry) インラインキャッシュ。型が flapping するサイトでは毎回 miss する。`mc->klass[2]` 程度の polymorphic IC を入れたい。

### 7. ✗ Boehm から自前 GC へ
Boehm は便利だが mark コストが大きい。abruby ほどの本格 mark/sweep ではなく、せめて世代別 / heap-tagged GC に移行したい (中長期)。

### 8. ✗ Self-tail-call optimization
末尾再帰を while ループに書き換える。fib のような自己再帰でも効くが、本質的には method-call dispatch を消す方向 (= 1 と同じ) で詰めるほうが筋が良い。

### 9. ✗ Specialization の永続化 (Code Store)
ASTro の `astro_code_store` をまだ使っていない。一度コンパイルしたら `<hash>.so` を残し、次回起動時に `dlopen` するようにすれば、コンパイル時間が分散できる。

### 10. ✗ JIT モード
現在は AOT 限定。execution 中に hot な NODE を非同期コンパイルする L0/L1/L2 デーモン構成 (naruby は実装済み) を移植する。

## ドキュメント / テスト

- `test/` ディレクトリを切って xunit-style テストを書く (現状は test.ko.rb の単発スモーク)
- ベンチマークスイート (loop / call / prime_count / optcarrot / SciMark)
- 性能回帰検出 (CI 連動)

## 短期 ToDo (optcarrot 実走に向けて)

優先度順:

1. **`*args` / `&blk` の受け側** — config.rb の `each {|opt| opts[opt] = ...}` 系で必要
2. **`Comparable` / `Enumerable` の mixin** — Range / Array の演算で多用
3. **真の `include`** — モジュールのインスタンスメソッドを class instance method として継承
4. **真の `&:method`** — Symbol#to_proc
5. **`format` の `*` width / `%-10s` 系**
6. **真の正規表現** — opt.rb の解析で使用
7. **多重代入の splat** — `*args` 展開
8. **エラー時の line 情報** — デバッグ効率化
