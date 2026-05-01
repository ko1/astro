# done.md — koruby 実装済み機能 / 性能改善

本書は **すでに動く** 言語機能と、**取り入れた性能改善** を一覧する。
未実装は [todo.md](./todo.md) に分離してある。

## 言語機能

### リテラル
- 整数 (Fixnum / Bignum 自動昇格、GMP `mpz_t`)
- 浮動小数 (Float、ヒープボックス) — FLONUM 即値化は未実装
- 文字列 (`""`, `''`, ヒアドキュメントは未確認)
- 文字列補間 `"#{...}"` → `node_str_concat` で実装
- シンボル `:foo`、補間 `:"@#{x}"`
- `nil` / `true` / `false` / `self`
- 配列 `[1, 2, 3]`、ハッシュ `{a: 1, b: 2}`、Range `1..10` / `1...10`
- (規定値の) Regexp は **文字列スタブ** で受ける (本物の正規表現は未実装)

### 変数 / 定数
- ローカル変数 (Prism の `depth` を使ったブロック越境アクセス)
- インスタンス変数 (`@x`、クラスごとの ivar shape で slot 管理)
- グローバル変数 (`$x`、線形テーブル)
- 定数 (lexical: `cref` チェインを CTX に持たせる)
- 定数パス (`Foo::Bar`)
- `||=` / `&&=` (ローカル / インスタンス / グローバル / 定数の各バージョン)
- 演算代入 `+= -= *= ...` (ローカル / インスタンス / 配列添字)
- 多重代入 `a, b, c = expr` (右辺が Array なら slot 配り、それ以外は単一 wrap)

### 制御フロー
- `if` / `elsif` / `else` / `unless`
- `if mod` / `unless mod` / `while mod` / `until mod`
- `while` / `until`
- `break` / `next` / `return` (state 伝搬)
- `&&` / `||` / `!` (短絡)
- `case x; when a; ... else ... end` (内部で `if (a === x)` チェーンに lower。`when` の主語が複数 (`when a, b, c`) も対応)
- `begin` / `rescue` (例外オブジェクトをローカル変数に bind 可能)
- `expr rescue fallback` (一行 rescue)
- `ensure` (raise / return / break いずれの非局所脱出でも実行)

### メソッド / クラス
- `def name(a, b, c) ... end` (位置パラメータのみ; `*args` `**kwargs` `&blk` 受けは未対応)
- メソッドディスパッチ `obj.foo(a, b)` / 暗黙 self `foo(a, b)`
- インラインキャッシュ (`struct method_cache` が node に @ref 相当で埋まる; `klass + method_serial` でヒット判定; 同一 callsite + 同一 receiver-class なら body + dispatcher を即実行)
- ブロック付き呼出 `foo { |x| ... }`、`foo(args) { ... }`
- `yield` / `yield args`
- ラムダ `-> { ... }`、`->(x) { ... }` (return セマンティクスは Proc と同一; 真の lambda return は未対応)
- `Proc#call`、`Proc#[]`
- `class Foo < Bar; ... end`、`module M; ... end` (cref チェインを push/pop)
- `include` (現状は flatten copy: モジュールのメソッド/定数を class へコピー。真の ancestor チェイン挿入ではない)
- `super` (引数あり) / `super` (引数なし forward — 現フレームの位置パラメータをそのまま親メソッドへ)
- `attr_reader` / `attr_writer` / `attr_accessor` (Module の cfunc として実装。getter/setter は本物の AST 経由のメソッドを動的に生成。`@x = arg`/`return @x` ノードを `ALLOC_*` で組み立てる)
- `private` / `public` / `protected` / `module_function` (現状は no-op; 可視性はチェックしない)
- `Module#const_get` / `const_set`、`Module#include`

### 例外
- 階層: `Exception` 以下に `StandardError` `RuntimeError` `ArgumentError` `TypeError` `NameError` `NoMethodError` `IndexError` `KeyError` `RangeError` `FloatDomainError` `ZeroDivisionError` `IOError` `LoadError` `FrozenError` `StopIteration` `LocalJumpError` `NotImplementedError` `ScriptError` `SyntaxError` を一通り定義
- `raise "msg"` → RuntimeError 風
- `raise Klass, "msg"` → 指定クラスの exception (msg 付き)
- ラッパ `begin ... rescue Klass => e ... end` (Klass 指定は受けるが現在は match を緩く判定 — 実際にクラス階層チェックは未対応)

### 組込クラス / メソッド (主要なもの)

#### Kernel
- `p` (multi-arg)、`puts`、`print`、`printf`、`format` / `sprintf`
- `raise` / `abort` / `exit`
- `require` / `require_relative` / `load` (循環防止 + `.rb` 補完)
- `inspect` / `to_s` / `class` / `==` / `!=` / `!` / `nil?` / `object_id`
- `freeze` / `frozen?` / `is_a?` / `kind_of?` / `instance_of?` / `respond_to?`
- `send` / `__send__` / `public_send`
- `instance_variable_get` / `instance_variable_set`
- `tap` (ほぼ no-op)、`block_given?` (常に false; 未配線)

#### Integer
- 算術 `+ - * / %`、比較 `< <= > >= == !=`、ビット `& | ^ << >> ~`、単項 `-`、`abs`
- `chr`、`to_s`、`to_i`、`to_f`、`zero?`、`succ` / `next` / `pred`、`floor` / `ceil` / `round` (整数なので no-op)
- `times { |i| ... }` (ブロック付き)
- `===` (== と同じ)

#### Float
- 算術 `+ - * /`、`to_s`、`floor`

#### String
- `+` / `<<` / `*` / `==` / `===`、`size` / `length` / `empty?`
- `to_s` / `to_sym` / `to_i(base)` / `to_f`
- `inspect`、`hash`
- `[]` / `[]=` (添字; 簡易)
- `chars`、`bytes`、`each_char`、`each_line` (split で代用)
- `start_with?` / `end_with?` / `include?`
- `upcase` / `downcase` / `reverse` / `replace`
- `chomp` / `strip`、`split` (空白 / 文字列セパレータ)
- `gsub` / `sub` (リテラル文字列マッチのみ; 正規表現は未対応)
- `tr` / `tr_s` (簡易)
- `%` (sprintf 連携)
- `dup` (簡易)

#### Array
- `[]` / `[]=`、`size` / `length`、`first` / `last`、`push` / `<<` / `pop`、`shift` / `unshift` / `prepend`
- `each` / `each_with_index` / `each_with_object`、`map` / `collect`、`select` / `filter`
- `reduce` / `inject` (引数 / 引数なし両対応)
- `sort` (`<=>` ベースの insertion sort)、`sort_by` (yield → ソート)
- `zip`、`flatten` (深さ 1)、`compact`、`uniq`
- `include?` / `index` / `find_index`
- `any?` / `all?` (ブロック受けは現状 truthy ベース; ブロック付き未対応)
- `min` / `max` / `sum`、`each_slice(n)`
- `reverse`、`clear`、`dup`、`concat`、`+`、`-`、`pack("C*")` (バイト並び限定)
- `inspect` / `to_s` / `==` / `===`

#### Hash
- `[]` / `[]=`、`size` / `length`
- `keys` / `values`、`each` / `each_pair` / `each_key` / `each_value`
- `key?` / `has_key?` / `include?`
- `merge` / `merge!`、`invert`、`to_a`
- `delete`

#### Range
- `each` / `map` / `collect` / `select` / `filter`、`reduce` / `inject`
- `first` / `last` / `to_a`、`step(n) { ... }`、`size` / `length`、`include?` / `===`

#### Symbol
- `to_s`、`==` / `===`、`inspect`、`to_proc` (現状は self を返すだけのスタブ)

#### Proc
- `call`、`[]`

#### Class / Module
- `new`、`name`、`===`、`attr_*`、`include`、`private` 系、`const_get` / `const_set`

#### File (クラスメソッドのみ)
- `File.read`、`File.join`、`File.exist?` / `exists?`、`File.dirname`、`File.basename`、`File.expand_path`

#### IO
- `STDOUT` / `STDERR` / `STDIN`、`$stdout` / `$stderr`
- `IO#puts` / `print` / `write` / `flush` / `sync=` (簡易; 標準出力にそのまま流す)

#### Struct
- `Struct.new(:a, :b, ...)` で新クラスを生成 (`attr_accessor` ＋ `initialize` ＋ `to_a` / `members`)

#### top-level 定数
- `ARGV` (コマンドラインから自動セット)、`ENV` (空 Hash スタブ)
- 例外クラス各種

## optcarrot 対応の現状

✅ **完走しました!** `Optcarrot::NES.new(argv).run` がエンドツーエンドで実行可能。

```sh
$ cd sample/abruby/benchmark/optcarrot
$ /path/to/koruby -e 'require_relative "lib/optcarrot";
    Optcarrot::NES.new(["-b", "--frames", "30", "examples/Lan_Master.nes"]).run'
fps: 71.47
checksum: 4096
```

CRuby (no JIT) との比較 (30 フレーム実行):
- CRuby: real 1.18s
- koruby: real 12.97s (約 11× 遅い、AOT 特化なし interp のみ)
- (注: video checksum は CRuby と一致しない。emulation の細部に微妙なバグが残存。完走は達成、emulation の正確性は今後の課題)

完走に至るまでに追加した主な機能 (本ドキュメント末尾「実装済みの性能改善」と並ぶ大きな塊):
- **Fiber** (ucontext ベース。256 KB スタック / fiber)
- **メソッド呼出 splat** (`unshift(*shortcut)` 等を runtime apply 経由で展開)
- **Range の splat** (`[*0..4096]` を実行時に Array 化)
- **ブロック destructure** (`each { |k, v| ... }` で Array を分解)
- **特異クラスメソッド** (`def self.foo` を per-class lazy singleton class へ; class.foo 経由で正しくディスパッチ)
- **lexical class/module reopen** (`class Optcarrot::ROM` の constant-path)
- **Optional / rest 引数** (Qundef sentinel + node_default_init prologue)
- **Hash 多数のメソッド** (fetch / merge / dup / map / select / reduce / compare_by_identity)
- **Array 多数のメソッド** (transpose / count / slice! / sort_by / each_slice / fill / [start, len]= 含む)
- **Float 演算** (`** == < <= > >= <=> -@ abs floor` 等)
- **Integer step / upto / downto** (block 無しでは Array を返すフォールバック)
- **Process.clock_gettime / Time.now**
- **Kernel#Integer / Float / String / Array** (型変換関数)
- **Range の Enumerable 系** (map / all? / any? / count)
- **Hash の比較 (deep eq)**、Array deep eq
- **String#sum / String#scan / =~ stub**
- **File.read / binread / extname / dirname / basename**

### 既知の差分 (要対処)
- **video checksum が CRuby と一致しない** — PPU 系の bit-twiddling のどこかで integer/array 挙動の差が出ている可能性 (調査未着手)
- 性能差 ~10×: メソッド呼出ディスパッチの最適化で詰める余地大 (PG-baked call_static)
- 真の正規表現は未実装 (=~/match/scan は no-op stub)

## 実装済みの性能改善

詳細は [perf.md](./perf.md) を参照。サマリのみ。

### ✅ 取り入れたもの

1. **インラインメソッドキャッシュ (`struct method_cache`)**
   - `mc->serial == method_serial && mc->klass == klass` でヒット判定
   - ヒット時: 直接 `mc->dispatcher(c, mc->body)` を呼ぶ — メソッドポインタを 2 段階で剥がす必要なし

2. **ASTro AOT 特化 (`./koruby -c`)**
   - 各 AST ノードの dispatcher を `SD_<hash>` に焼き直し → C コンパイラに大量にインライン展開させる
   - fib(35) で interp 0.55s → AOT 0.24s (2.3× 高速化)

3. **Fixnum 高速パス**
   - `node_plus`/`minus`/`mul` などで `FIXNUM_P(l) && FIXNUM_P(r)` を `LIKELY` 分岐
   - オーバフローは `__builtin_*_overflow` で検出 → GMP Bignum 経路へ

4. **CRuby 互換 VALUE 表現**
   - 即値 (Fixnum/Symbol/nil/true/false) はビット操作のみで判定
   - `RTEST` は 1 命令の AND 比較

5. **Boehm GC**
   - スタック・ヒープ全部を conservative scan。書き出すコードに mark/free 関数が一切要らない (実装速度の最大化)

6. **Closure を共有 fp で実装**
   - `yield` で呼ばれるブロックは親 fp をそのまま使う (env コピーなし)
   - `block_literal` 時に `param_base` (slot offset) を計算して保存

7. **state 伝搬による例外**
   - setjmp/longjmp を使わず、`CTX::state` 1 つで raise/return/break/next を表現
   - `EVAL_ARG` 後の分岐は `UNLIKELY` 付きで predictor friendly + コンパイラが部分木で消去可能

### ❌ 試したが取らなかったもの

- **NaN-boxing**: 既存の VALUE 表現 (CRuby 互換) を変更すると CRuby コードの将来流用が壊れるため見送り (ユーザ指示)
- **mark/sweep の自前 GC**: Boehm に比べて実装コストが大きく、初期段階の生産性を優先

詳細は [perf.md](./perf.md) を参照。
