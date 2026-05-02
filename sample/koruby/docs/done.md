# done.md — koruby 実装済み機能 / 性能改善

本書は **すでに動く** 言語機能と、**取り入れた性能改善** を一覧する。
未実装は [todo.md](./todo.md) に分離してある。

## テストスイートの現状 (2026-05-02)

`test/ruby/<category>/test_*.rb` に CRuby-スタイルのテストを配置。

```
$ for f in $(find test/ruby -name 'test_*.rb' | sort); do
    out=$(./koruby "$f" 2>&1 | tail -1)
    echo "$out" | grep -q "OK\|0 failures" && pass=$((pass+1)) || fail=$((fail+1))
  done
  echo "pass=$pass fail=$fail"
pass=124 fail=0
```

**124 / 124 全 pass** (assertion 数 ~2200)。残ってる互換性ギャップは [todo.md](./todo.md) を参照。

## 言語機能

### リテラル
- 整数 (Fixnum / Bignum 自動昇格、GMP `mpz_t`、`0b` `0o` `0x` プレフィックス、underscore separator)
- 浮動小数 (Float、FLONUM 即値化済み、ヒープボックスはフォールバック)
- 文字列 (`""`, `''`, ヒアドキュメント `<<-`/`<<~` の interpolation 含む)
- 文字列補間 `"#{...}"` → `node_str_concat` で実装
- シンボル `:foo`、補間 `:"@#{x}"`
- `nil` / `true` / `false` / `self`
- 配列 `[1, 2, 3]`、ハッシュ `{a: 1, b: 2}`、Range `1..10` / `1...10` (endless / beginless 含む)
- Regexp は **意図的に未実装** (project memory: `sample/astrorge` 経由で integrate 予定)

### 変数 / 定数
- ローカル変数 (Prism の `depth` でブロック越境アクセス)
- インスタンス変数 (`@x`、クラスごとの ivar shape で slot 管理 + inline cache)
- グローバル変数 (`$x`、線形テーブル)
- `$!` (rescue 中に current exception を保持; bare `raise` で再 raise)
- 定数 (lexical: `cref` チェイン経由 + super 階層 walk)
- 定数パス (`Foo::Bar` — 継承された定数も拾う)
- `||=` / `&&=` (ローカル / インスタンス / グローバル / 定数の各バージョン)
- 演算代入 `+= -= *= ...` (ローカル / インスタンス / 配列添字 / 属性 setter)
- 多重代入 `a, b, c = expr` (右辺が Array なら slot 配り、`*rest` 中央 splat、属性 setter / index 代入も LHS で動く)

### 制御フロー
- `if` / `elsif` / `else` / `unless`、modifier 形式
- `while` / `until` / modifier、`begin...end while` (do-while)
- `break` / `next` / `return` / `redo` / `retry` (state 伝搬)
- `&&` / `||` / `!` (短絡)
- `case x; when a; ... else ... end` (内部で `if (a === x)` チェーンに lower。`when a, b, c` も対応)
- `case x; in pattern; end` (基本パターン: literal / array / hash / class)
- `begin` / `rescue` / `else` / `ensure` (例外オブジェクトをローカル変数に bind 可能)
- 一行 rescue `expr rescue fallback`
- 多重 rescue clause `rescue A; ...; rescue B, C; ...; rescue => e; ...`
- `catch(tag) { throw tag, val }` (KORB_THROW state 経由でアンワインド)
- safe-navigation `recv&.method(args)` (recv が nil なら nil 返し、評価は一回だけ)

### メソッド / クラス
- `def name(a, b = 1, *r, c, **kw, &blk)` (位置 / 必須 / オプション / rest / post / kwargs / kwrest / block 引数)
- 真の **ancestor チェイン** (`include` した modules を `korb_class.includes[]` に保持。`Class#ancestors` / `is_a?` / `kind_of?` がこれを walk; method lookup は依然 flatten copy で fast path)
- `Module.new { ... }` / `Class.new(superclass) { ... }` (block を新クラスの context で eval)
- `Module#define_method(:name) { ... }` (proc body を AST メソッドとして登録; closure キャプチャ動作)
- `Module#prepend` (現在は include と同じ MRO 挿入で stub)
- `Module#undef_method` / `remove_method` (method_table から unlink)
- `Module#alias_method` / `alias` syntax
- `Object#extend(M)` (object の singleton class に M を include)
- `method_missing` フォールバック (find 失敗時に `method_missing(:name, *args)` で再 dispatch)
- `respond_to?` は user の `respond_to_missing?` も consult する
- 不明メソッドは `NoMethodError` (< NameError) を raise — `rescue NameError` で catch 可能
- メソッドディスパッチ `obj.foo(a, b)` / 暗黙 self `foo(a, b)`
- インラインキャッシュ (`struct method_cache` が node に @ref 相当で埋まる; `klass + method_serial` でヒット判定)
- ブロック付き呼出 `foo { |x| ... }`、`foo(args) { ... }`、do-end 形式
- `yield` / `yield args`、`block_given?`
- ラムダ `-> { ... }`、`->(x) { ... }`、`lambda { ... }` (strict arity 検査済み — wrong arity で ArgumentError)
- `Proc.new { }`、`proc { }` (lenient arity, 余分な引数は drop / 不足は nil)
- `Proc#call` / `Proc#[]` / `Proc#()` / `Proc#curry` (bootstrap.rb)
- **per-iteration closure capture**: `(1..3).each { |i| procs << proc { i } }` で各 proc が iter の i を保持 (`creates_proc` flag + fresh-env-with-writeback)
- `proc.call` は **outer 変数への書き戻し** が動く (env を共有)
- `class Foo < Bar; ... end`、`module M; ... end` (cref チェイン push/pop)
- `super` (引数あり / 暗黙転送 / 0 引数)
- `attr_reader` / `attr_writer` / `attr_accessor` (`@x` 経由の getter/setter を AST で動的生成)
- `private` / `public` / `protected` の各 form (no-arg modifier / `private :method` / 適用時は visibility check)
- `private_method_defined?` 系 (基本動作のみ)
- `Module#const_get` / `const_set` / `const_defined?`、`Module#include` / `prepend`
- `instance_eval { ... }` / `instance_eval(string)` / `class_eval` / `module_eval`
- `Object#send` / `__send__` / `public_send`、`obj.method(:name)` (Method object)
- `Method#call` / `to_proc`、`UnboundMethod#bind#call`

### 例外
- 階層: `Exception` 以下に `StandardError` `RuntimeError` `ArgumentError` `TypeError` `NameError` `NoMethodError` `IndexError` `KeyError` `RangeError` `FloatDomainError` `ZeroDivisionError` `IOError` `FrozenError` `StopIteration` `LocalJumpError` `NotImplementedError` `ScriptError` `SyntaxError` `LoadError` を一通り定義
- `Exception` を **T_OBJECT 化** (`@message` ivar に message を持つ)
- `Exception#initialize / message / to_s / inspect / backtrace` (Ruby 互換)
- `raise "msg"` → RuntimeError、`raise Klass, "msg"` → 指定クラス
- `rescue Klass => e` (クラス階層 walk で match — subclass も catch)
- `rescue A, B => e` (multi-class single clause)
- 多段 rescue clause で specific → fallback の順
- bare `raise` は `$!` を再 raise
- `1 / 0` で `ZeroDivisionError`
- `1 + nil` で `RuntimeError` (`expected Integer, got NilClass`)
- 比較演算 `Integer#< > <= >=` は非数値 RHS で `ArgumentError`

### Frozen
- `freeze` / `frozen?` (FL_FROZEN flag)
- 主要 mutator (`<<`, `push`, `pop`, `[]=`) は frozen check で `FrozenError` raise
- 即値 (Integer / Float / Symbol / nil / true / false) は inherently frozen
- `dup` は frozen を継承しない、`clone` は (今後対応予定)

### 組込クラス / メソッド (主要なもの)

#### Kernel
- `p` (multi-arg)、`puts`、`print`、`printf`、`format` / `sprintf`
- `raise` / `abort` / `exit`
- `require` / `require_relative` / `load` (循環防止 + `.rb` 補完)
- `inspect` / `to_s` / `class` / `==` / `!=` / `eql?` (type-strict on Numeric) / `equal?` (identity) / `===`
- `nil?` / `frozen?` / `freeze` / `is_a?` / `kind_of?` / `instance_of?` / `respond_to?`
- `send` / `__send__` / `public_send` (block forward 対応)
- `instance_variable_get` / `instance_variable_set` / `instance_variables`
- `dup` / `clone` (T_OBJECT/T_ARRAY/T_STRING/T_HASH をコピー)
- `tap` / `then` / `yield_self` / `itself`
- `block_given?`
- `caller` (簡易版)
- `__method__` / `__callee__`
- `loop` (StopIteration を swallow)
- `lambda` / `proc`
- `eval` (string)、`instance_eval(string)`
- `catch` / `throw` (任意の tag で unwind)
- `Kernel#Integer / Float / String / Array` (型変換、Array は Range/Hash を to_a で展開)

#### Math
- `Math::PI` / `Math::E`
- `sqrt sin cos tan asin acos atan sinh cosh tanh exp cbrt`
- `log` (1 引数 = ln, 2 引数 = base 指定)、`log2 log10`
- `atan2 hypot pow`

#### Integer
- 算術 `+ - * / %`、比較 `< <= > >= == !=`、ビット `& | ^ << >> ~`、単項 `-`、`abs`
- `chr`、`to_s(base)`、`to_i`、`to_f`、`zero?` / `positive?` / `negative?` / `even?` / `odd?`、`succ` / `next` / `pred`、`floor` / `ceil` / `round` / `truncate`
- `times { |i| ... }`、`upto` / `downto` / `step`
- `===` (== と同じ)
- `gcd` / `lcm` / `gcdlcm` / `digits(base)` / `pow(exp[, mod])` (Bignum overflow 対応)
- `[]` (bit access — inline fast path)、`bit_length`、`size` (machine word bytes)
- `div` (floored division)、`fdiv` (Float coerce divide)
- `eql?` is type-strict: `1.eql?(1.0) == false`

#### Float
- 算術 `+ - * / **`、`to_s` / `to_i` / `to_f`、`floor` / `ceil` / `round(n)` / `truncate`
- `<` / `<=` / `>` / `>=` / `==` / `<=>` (Integer 混在対応、NaN <=> nil)
- `-@` / `abs`
- `zero?` / `positive?` / `negative?` / `finite?` / `infinite?` / `nan?` / `divmod`
- `eql?` is type-strict
- 定数: `Float::INFINITY` / `NAN` / `MAX` / `MIN` / `EPSILON`

#### String
- `+` / `<<` / `*` / `==` / `===`、`size` / `length` / `bytesize` / `empty?`
- `to_s` / `to_sym` / `intern` / `to_i(base)` / `to_f` / `hex` / `oct`
- `inspect` / `hash`
- `[]` (Fixnum index inline fast path)、`[]=`、`slice(a[, b])`
- `chars`、`bytes`、`each_char` (block / no-block)、`each_byte`、`each_line` (実装済 — 以前の splitter alias を修正)、`lines`
- `start_with?` / `end_with?` / `include?`
- `upcase` / `downcase` / `swapcase` / `capitalize` / `reverse` / `replace`
- `chomp` / `strip` / `lstrip` / `rstrip` / `chop`、`split` (空白 / 文字列セパレータ / limit 対応)
- `ljust` / `rjust` / `center`
- `squeeze` / `count` / `delete` / `tr` (range 展開) / `tr_s` (squeeze 後)
- `gsub` / `sub` (リテラル文字列マッチ + block 形式)、Regexp は未対応
- `prepend` / `insert(pos, s)` / `delete_prefix` / `delete_suffix`
- `%` (sprintf — %d %s %x %o %X %b %f %g、padding、precision、`%{key}` の Hash 形式)
- `dup` / `clone`、`String.new(s = "")`
- `freeze` / `frozen?` (リテラルは frozen string 対応で `<<` で FrozenError)

#### Array
- `[]` / `[]=`、`size` / `length`、`first(n)` / `last(n)`、`push` / `<<` / `pop`、`shift` / `unshift` / `prepend`
- `each` / `each_with_index` / `each_with_object`、`map` / `collect`、`select` / `filter`、`reject`
- `reduce` / `inject` (block / Symbol arg / init 各 form)
- `sort` (block 渡せる)、`sort_by`、`sort!` (mutation)
- `zip`、`flatten` (深さ指定可)、`compact`、`uniq`
- `include?` / `index` / `find_index`
- `any?` / `all?` / `none?` / `one?` (block 受け対応)
- `min` / `max` / `minmax` / `minmax_by` / `sum`、`each_slice(n)`、`each_cons(n)` (no-block で Array<Array>)
- `reverse`、`clear`、`dup`、`concat`、`+`、`-`、`pack("C*")` (バイト並び限定)
- `inspect` / `to_s` / `==` / `===` / `eql?` (type-strict 要素ごと)
- `dig(*keys)` (Array/Hash chain 越え)
- `take_while` / `drop_while` / `take(n)` / `drop(n)`
- `flat_map` (一段 flatten)、`group_by` / `partition` / `tally`
- `min_by` / `max_by`、`chunk_while` / `slice_when` (bootstrap.rb)
- `shuffle` (Fisher-Yates copy)、`bsearch` (find-min mode)
- `Array[]` (class method literal form)
- `Array.new(n, default)` / `Array.new(n) { |i| ... }`
- Range slice: `a[1..3]` / `a[3..]` / `a[..2]` / `a[1...3]`
- `a[range] = value` (range 区間置換)
- 内容で hash + eql? (二つの `[1, 2]` literal は同じキーとして hit)

#### Hash
- `[]` / `[]=`、`size` / `length`
- 挿入順保持 (CRuby 1.9+ 互換 — overwrite で順序維持、delete + 再挿入で末尾)
- `keys` / `values`、`each` / `each_pair` / `each_key` / `each_value`
- `key?` / `has_key?` / `include?` / `member?`、`has_value?` / `value?`
- `merge` (block 渡しで衝突解決)、`merge!` / `update`
- `invert`、`to_a`、`to_h`、`to_s` / `inspect`
- `delete` / `fetch` (block / default value、KeyError raise)
- `compare_by_identity` / `compare_by_identity?`
- `transform_values` / `transform_keys` / `reject` / `select` / `filter`
- `any?` / `all?` / `count` / `find` / `detect` / `min_by` / `max_by` / `values_at` / `sort` / `sort_by`
- `group_by` / `filter_map` / `each_with_object` / `take(n)` / `flat_map`
- `dig(*keys)`、`sum [init] [block]`
- `dup` / `clone` / `empty?` / `===` / `map` / `collect` / `reduce` / `inject`
- `Hash.new(default)` / `Hash.new { |h, k| ... }` (default block)

#### Range
- `each` / `map` / `collect` / `select` / `filter`、`reduce` / `inject` (block / Symbol arg)
- `first(n)` / `last(n)` / `to_a`、`step(n)` (Float step 対応、no-block で Array)
- `size` / `length`、`include?` / `===`
- `cover?(v)` (endless / beginless 対応)、`min` / `max` / `sum`、`exclude_end?`
- `(5..)` / `(..10)` (endless / beginless リテラル)
- `zip(arr...)` / `each_with_index`

#### Enumerable mixin (bootstrap.rb)
- `each` を持つクラスに `include Enumerable` で全 helper を提供
- `to_a / count / map / select / reject / find / reduce / min / max / include? / first / each_with_index / any? / all? / none? / sort`
- `group_by / partition / each_cons / tally / min_by / max_by / sum / zip / flat_map / take_while / drop_while / each_with_object / chunk_while`
- alias: `collect / filter / inject / detect / entries / member?`

#### Comparable mixin
- include した class が `<=>` を定義すれば `< <= > >= == between? clamp` が自動的に来る
- `clamp(min, max)` / `clamp(range)` 両 form

#### Symbol
- `to_s` / `to_sym` / `===`、`inspect`
- `to_proc` (専用 proc shim — `argv[0].send(sym)` に dispatch)
- `<=>` / `==`、`length` / `size` / `empty?`
- `upcase` / `downcase` / `capitalize` / `swapcase`、`succ` / `next`

#### Proc / Lambda
- `call` / `[]` / `()` / `to_proc` (self 返し)、`arity`、`lambda?`
- `curry` (bootstrap.rb)
- proc.call の outer 変数への書き戻し動作

#### Rational / Complex (bootstrap.rb)
- `Rational(n, d)` / `Rational.new(n, d)` (gcd 簡約; Comparable include)
- `Complex(r, i)` / `Complex.new(r, i)`
- 算術・比較・`to_f` / `to_i` / `inspect` / `to_s`

#### Class / Module
- `new(...)`、`new(super){...}` / `Module.new{...}` 対応
- `name`、`===`、`attr_*`、`include`、`prepend` / `extend`、`private` 系
- `const_get` / `const_set` / `const_defined?`
- `ancestors` / `define_method(:name) { ... }`、`method_defined?`
- `instance_method(name)` → UnboundMethod、`bind(receiver).call`
- `undef_method` / `remove_method`

#### File (クラスメソッドのみ)
- `File.read`、`File.join`、`File.exist?` / `exists?`、`File.dirname`、`File.basename`、`File.expand_path`

#### IO
- `STDOUT` / `STDERR` / `STDIN`、`$stdout` / `$stderr`
- `IO#puts` / `print` / `write` / `flush` / `sync=` (簡易)

#### Struct
- `Struct.new(:a, :b, ...)` で新クラスを生成 (`attr_accessor` ＋ `initialize` ＋ `to_a` / `members`)

#### Fiber (ucontext ベース)
- `Fiber.new { |arg| ... }`、`fiber.resume(arg)`、`Fiber.yield(value)`
- 256 KB スタック / fiber、Boehm GC が swap した stack も scan

#### top-level 定数
- `ARGV` (コマンドラインから自動セット)、`ENV` (空 Hash スタブ)
- 例外クラス各種、`Float::INFINITY` 等の数値定数

## optcarrot 対応の現状

✅ **完走 + CRuby と checksum 一致** — `Optcarrot::NES.new(argv).run` がエンドツーエンドで実行可能、frame buffer も正しい。

```sh
$ cd sample/abruby/benchmark/optcarrot
$ /path/to/koruby -e 'require_relative "lib/optcarrot";
    Optcarrot::NES.new(["-b", "--frames", "180", "examples/Lan_Master.nes"]).run'
fps: 86.0
checksum: 60838  # ← ruby と一致
```

CRuby (no JIT) との比較 (600 フレーム実行):
| | FPS | vs CRuby |
|---|---:|---:|
| ruby | 38 | 1.00× |
| ruby --yjit | 163 | 4.27× |
| koruby (interp) | 50 | 1.30× |
| koruby (AOT-cached) | 87 | 2.29× |

(checksum 60838 で全実行 fixed-content 一致。これが一致しないと「empty frame だけレンダして速い」という擬似値になる — 過去にハマった。bench-optcarrot.sh は checksum 検証付き)

完走に至るまでに追加した主な機能:
- **Fiber** (ucontext ベース。256 KB スタック / fiber)
- **メソッド呼出 splat** (`unshift(*shortcut)` 等を runtime apply 経由で展開)
- **Range の splat** (`[*0..4096]` を実行時に Array 化)
- **ブロック destructure** (`each { |k, v| ... }` で Array を分解、`|(k,v), acc|` の混合 form も対応)
- **特異クラスメソッド** (`def self.foo` を per-class lazy singleton class へ)
- **lexical class/module reopen** (`class Optcarrot::ROM` の constant-path)
- **Optional / rest / kwargs 引数** (Qundef sentinel + node_default_init prologue + kwargs の hash peel)
- **Hash 多数のメソッド** (fetch / merge / dup / map / select / reduce / compare_by_identity 等)
- **Array 多数のメソッド** (transpose / count / slice! / sort_by / each_slice / fill / [start, len]= 含む)
- **Float 演算** (`** == < <= > >= <=> -@ abs floor` 等、FLONUM 即値)
- **Integer step / upto / downto** (block 無しでは Array を返すフォールバック)
- **Process.clock_gettime / Time.now**
- **Range の Enumerable 系** (map / all? / any? / count / step(Float))
- **Hash / Array の deep eq + content hashing**
- **多重代入の attribute setter 対応**: `@vclk, @hclk, @cpu.next_frame_clock = ...` で全 LHS が assign される (これが効かなかったので長い間 PPU が空フレームを返していた)
- **basic-op redef guard の正しい実装**: `class Integer; def gcd; end` で fast path が無効化されない (gcd は basic op じゃないので)

## 実装済みの性能改善

詳細は [perf.md](./perf.md) を参照。サマリのみ。

### ✅ 取り入れたもの

1. **インラインメソッドキャッシュ (`struct method_cache`)**
   - `mc->serial == method_serial && mc->klass == klass` でヒット判定
   - ヒット時: 直接 `mc->dispatcher(c, mc->body)` を呼ぶ — メソッドポインタを 2 段階で剥がす必要なし

2. **ASTro AOT 特化 (`./koruby --aot-compile`)**
   - 各 AST ノードの dispatcher を `SD_<hash>` に焼き直し → C コンパイラに大量にインライン展開させる
   - whileloop 100M で interp 2.0s → AOT 0.28s (7.2× 高速化、yjit 1.58s に対しても 5.7×)
   - optcarrot で interp 50 → AOT 87 fps (~1.7×)

3. **Fixnum 高速パス**
   - `node_plus`/`minus`/`mul`/`<` などで `FIXNUM_P(l) && FIXNUM_P(r) && !redef` を `LIKELY` 分岐
   - オーバフローは `__builtin_*_overflow` で検出 → GMP Bignum 経路
   - Integer#[] (bit access) inline、String#[] (single-char) inline

4. **CRuby 互換 VALUE 表現** (FLONUM 即値含む)
   - 即値 (Fixnum/Symbol/Flonum/nil/true/false) はビット操作のみで判定
   - `RTEST` は 1 命令の AND 比較

5. **Boehm GC**
   - スタック・ヒープ全部を conservative scan。書き出すコードに mark/free 関数が一切要らない

6. **Closure を共有 fp で実装**
   - `yield` で呼ばれるブロックは親 fp をそのまま使う (env コピーなし) — fast path
   - block_literal 時に `param_base` (slot offset) を計算して保存
   - `creates_proc` flag が立った block (body に `proc { }` 等が含まれる) のみ fresh-env-with-writeback の slow path に切替

7. **state 伝搬による例外**
   - setjmp/longjmp を使わず、`CTX::state` 1 つで raise/return/break/next/throw を表現
   - `EVAL_ARG` 後の分岐は `UNLIKELY` で predictor friendly + 部分木最適化可能

8. **PGO (-fprofile-use 二段ビルド)**
   - 1pass: `make koruby-pgo` で計装→optcarrot 実行→計測
   - 2pass: 計測ファイルを使って再ビルド
   - Float-heavy bench でとくに効く

### ❌ 試したが取らなかったもの

- **NaN-boxing**: 既存の VALUE 表現 (CRuby 互換) を変更すると CRuby コードの将来流用が壊れるため見送り (project memory: NaN-boxing 禁止)
- **mark/sweep の自前 GC**: Boehm に比べて実装コストが大きく、初期段階の生産性を優先

詳細は [perf.md](./perf.md) を参照。
