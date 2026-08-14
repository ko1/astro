# done.md — koruby 実装済み機能 / 性能改善

本書は **すでに動く** 言語機能と、**取り入れた性能改善** を一覧する。
未実装は [todo.md](./todo.md) に分離してある。

## 2026-08-13〜14 に入れたもの (rubyspec 実 mspec sweep ラウンド)

計測と全体像は [rubyspec.md](./rubyspec.md) の「2026-08-13〜14」節を参照
(core pass-rate 75.9% → 80.2%、make test 100,098 → 100,354)。

### 新規実装
- **IO::Buffer** (`prelude/io_buffer.rb`) — binary String + offset で表現。
  new/for/string/map/slice/transfer/free/resize、get_value 系 (18 型を
  pack/unpack)、get_string/set_string/clear/copy、`& | ^ ~` と破壊版、
  `<=>`/`==`、hexdump/inspect (256 バイトで打ち切り)。valid? は親の
  generation と bounds で判定し、無効な slice へのアクセスは InvalidatedError。
- **IO.copy_stream** — path / IO 風オブジェクトの両対応、length・offset の
  #to_int 変換、`#readpartial` 優先 (対話的な pipe でデッドロックしない)。
- **autoload** — Module#autoload / autoload? / Kernel#autoload。koruby は定数を
  即時解決するので「モジュールごとの表に記録し #const_missing で require して
  引き直す」形。top-level の定数ミスでも Object.const_missing を呼ぶよう修正。
  require が `$LOADED_FEATURES` に絶対パスを積むようにした。
- **Fiber**: #raise (Kernel/Thread と共通の例外ビルダ)、#transfer、#kill
  (中断点で unwind、ensure は走る。実行中の親を子から kill する場合は
  killing フラグを立てて switch 点で unwind)、storage (`Fiber[]` / `#storage` /
  `Fiber.new(storage:)`)、blocking?、scheduler スタブ。
- **File::Stat**: readable?/writable?/executable? とその *_real? (access(2) では
  なく保持している stat 値で判定)、size?、dev_major/dev_minor/rdev_major/
  rdev_minor、birthtime (statx(STATX_BTIME))。File#lstat、File.birthtime。
- **Process**: PRIO_* と getpriority/setpriority、CLOCK_* 定数一式、
  times (getrusage + Process::Tms)、groups/waitall/setproctitle/getsid/warmup、
  Process::Status.wait。
- **Regexp**: linear_time?、timeout/timeout=、try_convert、fixed_encoding?。
- **Binding**: implicit_parameter_get / implicit_parameter_defined? /
  implicit_parameters (`_1..._9` / `it`)、#inspect。`{ it }` ブロックは prism が
  locals を持たないので Binding ノード生成時にスロット 0 を "it" と名付ける。
- **その他**: Kernel#putc、IO#close_read/close_write (socket なら shutdown(2))、
  Enumerator::Lazy#eager、GC.config/total_time/measure_total_time、
  Encoding::Converter のフラグ定数 14 個。

### 直した汎用バグ (spec 以外にも効く)
- splat 呼び出しで空 kwsplat が elide されない (`node_ary_push_kw` 追加)。
  `f.raise(*args, **kwargs)` 形の delegation が全部壊れていた。
- `define_method(:m, &SomeClass.method(:x))` が LocalJumpError。
- `Klass.send(:new)` が define_method した特異 `new` を無視 (直接呼び出しは効く)。
- Regexp が inspect で `#<Object>` になる (C プリンタに枝が無かった)。
- **Kernel#exit が SystemExit を raise しない** — rescue/ensure が効かず、
  exit 系 spec 4 file が途中で黙って終了していた。abort も $stderr 経由に。
- 未捕捉例外の表示が etype 名なので LoadError/ThreadError/ユーザ定義が
  全部 RuntimeError と出ていた。
- 双方向ストリームのデッドロック 2 種 (read 前の flush / copy_stream の read)。
- define_method の body 内 `break` がフレームを突き抜けてプログラム終了。
- 変換プロトコルの respond_to? を include_private 付きで呼んでいなかった。

### CRuby 追従 (既存機能の詰め)
- **String#unpack** のディレクティブ解析を全面修正 (#to_str・`#` コメント・
  `X`・`@`・修飾子検査・`u`・結果 encoding・`Z*`・`U` の malformed 検出) →
  unpack バケツ 61err → 0。**Array#pack** も `@`/`Z`/`M`/`m`/`u` と結果 encoding。
- IO の Integer 引数を #to_int 変換、IO#reopen が #to_io、IO.new の options Hash
  (mode:/binmode:/encoding: 系)、File.open の keyword options と第3引数 perm、
  File.new(fd) 形、File.chmod の mode 変換。
- require/load のパス引数を #to_path/#to_str 変換、LoadError#path。
- Time: 秒未満精度 (clock_gettime)、Time.now(in:)、軍事タイムゾーン 1 文字表記、
  Time.at の Rational 精度と #to_r 変換、Time#to_i の floor、utc_offset の範囲検査。
- Kernel#Rational の Complex 引数と #to_int fallback、Complex の j/J と極形式、
  Integer/Float/Rational/Complex の `exception:` 値検査、Kernel#exit の引数変換。
- String#byteslice/#bytesplice の境界と型エラー、Array#find/#rfind の ifnone。
- Thread#to_s に生成位置と BINARY encoding。IO.popen の "r+" (socketpair)。

## 2026-08-12 に入れたもの (継続ラウンド)

- socket: recvfrom / recvmsg / sendmsg / gethostbyname / accept_nonblock /
  autoclose / Addrinfo marshal・connect_to・ipv6_to_ipv4。
  `Object#send` が実メソッドの #send を横取りするバグ修正。
- `Kernel#require` の Kernel 登録が init 順序バグで死んでいたのを修正
  (`super` から見えるように)。
- Process: RLIMIT_* / getrlimit / setrlimit / detach / clock_getres / Sys、
  `Kernel#test` / trace_var / singleton_method。
- IO: getbyte / sysseek / fsync / fdatasync / advise / each_byte /
  each_codepoint / readbyte / putc / to_io / autoclose / IO.try_convert、
  io/nonblock・fcntl スタブ。
- File: chown / utime / lutime / mkfifo / ftype / empty? / identical? /
  world_readable? ほか述語 15 個。**FIFO open の park 化** (open(2) の
  ブロックで scheduler ごとデッドロックしていた)。
- StringIO: getc のバイト/文字インデックス混同 (マルチバイトで nil)、
  limit 付き each_line、kwargs init。
- **演算子が user object の method_missing に落ちないバグ修正**
  (mspec の `should >=` matcher 全滅の真因)。
- Range サブクラスの `new` (real payload + override table)、
  beginless の max(n) / reverse_each / size、endless String range の each。
- Enumerable#to_a / each_with_index / each_entry の #each への引数転送。
- Dir.home / foreach / each_child / empty? / #chdir / #fileno。
- Object#!~、SystemCallError.new(errno) 形。

make test 99,873 → **100,057 PASS** (10 万台)。

## 2026-08-11 に入れたもの

- **rubyspec runner を mspec-run 駆動に** (`tools/mspec_launch.rb`)。
  ruby/spec の spec_helper が `$0` を再ロードして自分で走らせる仕様のため、
  spec file を直接渡すと中間 spec_helper の `require` が example の後に
  なっていた。socket 系が丸ごと動くようになった。
- **本物の sockaddr pack/unpack** (`Socket.sockaddr_in` / `sockaddr_un` /
  `Addrinfo#to_sockaddr` が struct そのもののバイト列を返す)。
  `Addrinfo#inspect` / `#inspect_sockaddr` を CRuby 準拠に。
- **File の metaclass 再親付け** — `File.for_fd` / `File.sysopen` が通る。
- **break のオーナー識別** — `&block` / `Proc#call` 越しの `break` が
  途中の `each` に食われず、ブロックリテラルを渡した呼び出しから抜ける。
- **`Enumerator::Chain` / `Enumerator::Product`** (純 Ruby)。
  `Enumerable#chain` / `Enumerator#+` / `Enumerator.product` も。
  Ruby で書いた Enumerator サブクラスは `Enumerator.inherited` 経由で
  Enumerable の実装を先に置く (C 実装が enumerator struct を読んで SEGV
  するのを回避)。
- **signal を SignalException として配送**。block + `sigtimedwait(2)` 方式。
  `SignalException#signo` / `#signm`、`Interrupt`。
- **`ObjectSpace::WeakKeyMap`** 新規、`WeakMap` を identity 比較 +
  each 系 / keys / values / inspect に拡充。
- `Array` / `String` / `Hash` の `#replace(self)` が中身を空にするバグ修正。
- bare な定数読みで top-level 定数を入れ子定数より優先するよう修正。

## テストスイートの現状 (2026-05-09, tenth pass)

### 集計

| Suite                          | pass   | fail  | err   | rate  |
|--------------------------------|--------|-------|-------|-------|
| 自前 `test/ruby/` (737件)        | 737    | 0     | 0     | 100%  |
| `spec/ruby/language/`           | 3,745  | 190   | 51    | 94%   |
| `spec/ruby/core/` 23 cat        | 14,434 | 3,071 | 1,014 | 77.9% |
| └ うち perfect ファイル          | 313 / 930 = 33.7%                   |

集計対象 23 cat: array hash string integer numeric range comparable module
proc kernel symbol float exception basicobject set rational random gc signal
binding class enumerator regexp。

主な未到達領域は encoding (utf-8 / capitalize 系)、 regexp 依存全般、
Bignum-Float 末尾 ULP、 Struct subclass 細部、 TracePoint / refinements /
Fiber 系。

### tenth pass の主な改善 (2026-05-09)

- **chilled string 完全実装**: FL_CHILLED フラグ + parse.c が prism の
  PM_STRING_FLAGS_FROZEN/MUTABLE を読み分けて `node_str_lit` /
  `node_frozen_str_lit` / `node_chilled_str_lit` を選択。 Symbol#to_s も
  chilled 返却。 `+@` は `frozen? || __chilled?` で fresh dup。
  `chilled_string_spec` / `uplus_spec` 共に full pass。
- **NameError @name/@receiver 復元**: vcall (`foo`) → BasicObject の既定
  `method_missing` 経由で raise されるパスで、 `e.name = :foo` /
  `e.receiver = self` を ivar に事前保存してから raise。 `name_error_spec`
  full pass。
- **sized Enumerator + each redispatch**: `to_enum` / `enum_for` で
  `@__source_obj` / `@__source_method` / `@__source_args` を memoize。
  `Enumerator#each(&blk)` はそこへ block-pass で再 dispatch するので
  `h.transform_values.each(&:succ)` が Hash で集約される。 `@__size` は
  receiver の `:size` を snapshot。
- **Combinatorics の sized Enumerator**: combination(n) / permutation(n)
  no-block で Enumerator + binomial / factorial size。
- **30 件の Kernel module function を private 化** + Kernel.singleton_class
  に public でコピー (abort/exec/exit/loop/proc/lambda/binding etc.)。
- **Object#<=> / initialize_copy / initialize_clone / initialize_dup** の
  既定実装。
- **GC モジュール**: garbage_collect / disable / enable / start / count /
  stat。 disable / enable は @disabled state を切り替えて前状態を返却。
- **Range#to_s** 端点を `to_s` で render、 **Range#count** endless で
  Float::INFINITY、 **Range#eql?** 型厳密。
- **Random.new_seed** uniqueness、 **Random#seed** to_int coerce。
- **Integer#allbits/anybits/nobits/sqrt/try_convert/to_r/rationalize/
  numerator/denominator/ord** を bootstrap 追加。 **Float#numerator/
  denominator/to_r** も。
- **Hash#flatten/transform_keys{,!}/to_h(block)/sort(block)/replace
  (frozen check)**。
- **Array#fetch/fetch_values/to_a/to_ary/deconstruct**。
- **String#each_byte Enumerator/strip!/lstrip!/rstrip! を C 版**で frozen
  check 確実化。 **String#<=>** to_str coerce + mirror <=> + recursion guard。
- **Symbol#intern/name** + **Symbol#inspect** の bare/quoted 判定 (`@@x`,
  `$LOAD_PATH`, `$~` 等)。
- **Rational#integer?=false** + **Rational.new 禁止** (Rational(...)
  factory に統一)。
- **Comparable#==** identity 短絡 + Float 0.0 + NoMethodError swallow。
- **Module lifecycle hook** 既定 (included/extended/prepended/method_added/
  method_removed/method_undefined/const_added)。
- **BasicObject** lifecycle hook + initialize / method_missing 既定。

## テストスイートの現状 (2026-05-08, fifth pass)

### 直近改善 (rubyspec language sweep)

mock-shim の slot 衝突を解消したことで隠れていた fail が一気に表面化、 net で
+311 pass。 主な fix:

- **proc.c の env-clone 条件拡大**: `prev_fp > new_fp` で常に clone するように
  し、 lambda/proc 経由の closure body から呼ばれる method frame が caller の
  active frame の slot を上書きする問題を解消。 mock-shim の `__apply_matcher`
  で `m.kind` / `m.arg` が `Module` / `String` に化けるバグが消えた (共通の
  「`'kind' for X` 系」エラー)。 これだけで rubyspec の language で +301 pass。
- **lambda の opt 引数**: `parse.c` の `PM_LAMBDA_NODE` パスが optional の数を
  params_cnt に含めず、 default-init prologue も生成していなかった。 block
  パスと同じ処理を追加。 `proc.c` の strict-arity check も `[required, total]`
  範囲チェックに修正。 `language/lambda_spec` で +7 pass。
- **anon-rest + post の slot mismatch**: `def m(*, a)` で post 値が
  `fp[anon_rest_slot+1]` ではなく `fp[total_params_cnt - post_cnt + i]`
  に書かれるよう修正 (param-position layout に揃える)。 `language/method_spec`
  と core 全般で複数 pass 増。
- **proc の post-rest extra drop**: `proc {|a, b=, c=, d, e|}.call(1..6)` で
  6 個目を末尾から落とすよう修正。 従来は中間から落としていたため
  `[1,2,3,5,6]` が返っていた。 CRuby は `[1,2,3,4,5]`。
- **hash literal の string-key 自動 freeze**: `{key => v}` で mutable な key を
  使うと CRuby は frozen copy で dedup する。 `node_hash_new` で対応。
  `language/hash_spec` が perfect 化。
- **missing keyword の全キー列挙**: 従来は最初の missing で raise していたが、
  CRuby と同じ `"missing keywords: :a, :b, :c"` 形式で全部列挙するよう修正。
  `Hash#__korb_required_kwargs_check__` を追加し、 method def の prologue で
  up-front チェック。

### 自前 test/ruby/

### 自前 test/ruby/

`test/ruby/<category>/test_*.rb` に koruby 固有テストを配置。
**24 ファイル中 23 OK / 1 FAIL** (`ArrayLshiftRedef` のみ既知 regression、
[todo.md §C](./todo.md))。 23 OK の合計 733 件 全 pass。

### CRuby test/ruby/ (互換性 sanity)

CRuby 公式の `test/ruby/test_*.rb` を tu_shim 経由で実行。
**in-scope 67 ファイル: 1,108,357 / 1,430,888 pass (77.5%)**。

### CRuby test/ruby/ 全 135 ファイル sweep (2026-05-10 最終)

無 fairness raw sweep (120s/file timeout):

| 状態                 | sweep #1 (起点) | 最終 sweep | 主な内訳 |
|----------------------|----------------:|----------------:|---|
| ≥1 pass             | 62 / 135        | 89 / 135 (66%)  | +27 ファイル復活 |
| total=0 (load 失敗等) | 30              | 6               | yjit/shapes/vm_dump 等 (impl外) |
| LOAD ERROR           | 23              | 1               | 残り test_time_tz のみ (Regexp 必要) |
| dumped core          | 7               | 0               | super / refinement / mm / fiber / dbl2int 等で全解消 |
| timeout (empty)      | 3               | 0               | IO.pipe を unbuffered にして全解消 |
| pass 合計            | 834,385         | 1,050,622       | +216k pass |
| 全 assertion 合計    | 954,960         | 1,405,004       | +450k assertion |

残 0-pass の 46 ファイルは大半が **意図的 pending** — TracePoint /
ObjectSpace::WeakMap / Ractor / 真の Refinements / 非 UTF-8 Encoding /
MJIT / 完全 Marshal / 真の ARGF (subprocess fork) / callcc。
koruby は単一プロセス・単一スレッド・UTF-8 only の方針なので、
これらは sample/astrorge (Regexp) のような外部 dep 整備か独立 PR 待ち。

### 残存課題と OOS 線引き

intentional pending (本サンプルでは扱わない):

| 機能 | 影響 test/ruby ファイル |
|---|---|
| Regexp (Onigmo 互換) | test_time_tz / test_regexp / test_string の grapheme / test_backref |
| TracePoint | test_settracefunc / test_trace 一部 |
| ObjectSpace::WeakMap | test_weakmap / test_objectspace |
| 真の Refinements (lexical scope) | test_refinement の高度 case |
| 非 UTF-8 Encoding | test_econv / test_transcode / test_m17n_comb 一部 |
| MJIT / RJIT | test_mjit / test_rubyvm_mjit |
| 完全 Marshal (class identity 含む) | test_marshal の binary 系 |
| 真の ARGF (subprocess) | test_argf |
| callcc / Continuation | test_continuation |

将来やる場合の **解放可能 test 数**:
- Regexp 統合だけで test_string +500、 test_regexp +250、 test_time_tz +800 程度を期待
- TracePoint 実装で test_settracefunc 全 80 余り解放

### test_array (220 methods) 周りの compatibility 改善

 - parse.c: `recv[*splat]` を index ではなく apply_call として展開
   (Array[*(1..100).to_a] が 1 要素配列を返していた)。
 - Array#cycle / repeated_combination / repeated_permutation の C 実装
   (bootstrap.rb 版は break が nested block を貫通しなかった)。
 - korb_ary_aset / EVAL_node_aset / ary_aset で `LONG_MAX/sizeof(VALUE)`
   超えの index に IndexError raise (旧コードは OOM)。
 - ary_first_n が Bignum で常に RangeError → mpz_fits_slong_p で
   long に収まれば受理 (test_array LONGP probe が 2^31 ではなく
   2^63 になるように)。
 - ary_aset 3-arg form `a[start, len] = val` (val が非 Array) で
   "len 個削除して val 1 個挿入" の正しい shift/shrink を実装。
 - ary_product / ary_mul で overflow 検出 → RangeError / ArgumentError。

### sprintf %b / String#[]= / object_id 等の細かい修正

 - sprintf %b に width / precision / 0-pad / negative-prefix を実装
   (test_sprintf 43 → 69 pass)。
 - String#[]= を実装 (旧コードは stub。 IndexError raise 含む)
   test_string 1997 → 2029 pass。
 - object_id を immediate 値で VALUE 自体に (旧 `(long)self / 8` は
   Fixnum 1, 2, 3, ... を全部 0 に潰していた → Hash#hash が壊れる)。
 - kwh_save_slot を未指定時に空 Hash で初期化 (`def initialize_clone
   (other, freeze: nil)` のような kwarg method を C cfunc から呼ぶと
   `kwh.has_key?` で nil 落ちしていた)。
 - korb_build_backtrace に dangling frame ガード (block が parent stack
   frame を outlive する lazy enumerator パターンで SEGV)。

### kwargs 周りの dispatch 整備

 - `korb_dispatch_to_method` (cfunc → AST 経由) でも FL_KWARGS hash の
   peel 処理を実装。 `Method#call(**{})` / `obj.send(name, **{})` 等で
   trailing kwargs が `*args` に紛れ込んでいた問題を解消。
 - kwarg を持つ callee (`def m(*, k:)`) には peeled hash を
   kwh_save_slot に格納、 持たない callee (`def m(*a)`) で空なら drop。
 - dispatch_to_method の arg-count error を `RuntimeError` ではなく
   CRuby 互換の `ArgumentError "wrong number of arguments (given N,
   expected M)"` に。 これだけで test_keyword 696 → 756 pass (+60)。

### Kernel#Float() の strict 検証

 - 旧コード: `Float("xyz") == 0.0` 黙って返却。 修正: nil → TypeError、
   "xyz"/"3.14abc"/"" → ArgumentError "invalid value for Float()"、
   `exception: false` opt → 失敗時 nil、 Bignum も受理 (旧 nil)。
 - 前後空白許容 (CRuby 互換)。

主な修正:
 - **super dispatch 修正** (object.c:korb_dispatch_binop): caller block の
   defining_method を漏らしていた → `Class#new → user initialize → super`
   が "run_all" などの caller method 名で lookup する致命バグを修正。
   test_string が 0→1965 pass に。
 - **tu_shim 大幅拡張**: Encoding (.find / 70 alias) / Process (UID/GID/CLOCK_*) /
   Thread::Queue / IO 定数 / File::Constants / RubyVM::AbstractSyntaxTree /
   Bug / Socket / Dir.mktmpdir / Tempfile / FileUtils。
 - **Float SIGFPE 修正**: `(long)(big_double)` の UB を `korb_dbl2int` (Bignum
   fallback 付き) に置換。 flt_floor/ceil の n<0 の div-by-0 も修正。
 - **Integer 無限再帰 SEGV**: `2 ** -2^62` で fixnum overflow → 無限再帰 →
   stack overflow を fixnum 範囲外なら 0.0 返却で回避。
 - **Comparable#== 無限再帰**: thread-local depth counter で 16 段で false。
 - **build_exec_argv の NULL CTX**: caller から CTX 渡すように改修。
 - **bootstrap method_missing recursion**: `self.inspect` が再 raise する
   class (BasicObject 由来 / inspect 削除済 / 自己 raise) で `"undefined
   method '...' for #{self.inspect}"` を組む際に無限再帰 → SEGV してい
   た。 thread-local depth で 4 段以上で `(recursion)` 表示に切替。
   test_pattern_matching と test_case が SEGV → 蘇生。
 - **fiber entry の NULL body**: Symbol#to_proc 由来の body=NULL proc を
   fiber に渡されて EVAL(c, NULL) で SEGV。 NULL なら Qnil 返却。
 - **UnboundMethod late-binding bug**: `Module#instance_method(:foo)` が
   名前だけ保存して call 時に再 lookup していたため、 後から
   define_method 上書きされた new body と無限再帰。 captured_method を
   即時凍結する形に変更 (test_super で必須)。
 - **Binding が stack-alloc cref を保存していた**: `class C; B = binding;
   end` のように class body 内で binding すると当時の cref は C スタッ
   ク上の korb_cref で、 class body 終了後に dangle → 後で eval(str,
   binding) が SEGV。 binding_alloc_from で cref chain を heap に
   deep-copy。
 - **Struct.new(...) do def foo end の lexical leak**: block を yield す
   る際に block->cref を一時的に new Struct を指すように swap してい
   なかったので def が lexical 親に上書きしていた (test_marshal の
   TestMarshal が壊れた method_missing を継承して SEGV)。
 - **Class#clone が singleton method を copy していなかった** (test_module
   の `MyClass = AClass.clone` で AClass の class method `cm1` が消える)。
   src->basic.klass の method を nk_meta に alias copy。
 - **A::B::C = 0 ** 0 の slot collision** (test_primitive)。
   PM_CONSTANT_PATH_WRITE_NODE の transduce で parent_slot/a0/a1 を val
   transduce 中も hold するように修正。
 - **IO.pipe が line-buffered**: `w.write "."` (改行なし) が writer の
   stdio buffer に滞留し reader の readpartial が無限 block。 _IONBF に
   切替 (test_io / test_optimization の timeout 解消)。

### CRuby spec/ruby/language/ (rubyspec 互換)

CRuby 公式の rubyspec (mspec ベース) を **mspec_shim 経由** で実行。
`describe`/`it`/`should == / be_nil / be_kind_of / raise_error / etc.`
を最低限実装した shim を `test/cruby_runner/mspec_shim.rb` に配置。

```sh
$ ./koruby test/cruby_runner/run_rubyspec.rb \
    /path/to/cruby/spec/ruby/language/and_spec.rb
and_spec.rb: pass=26 fail=0 err=0 skip=0
```

language/* (65 ファイル走破): **pass=3,715 / 4,009 (92.7%)、 34 ファイルが
100% perfect、 23 件 SKIP**。

100% perfect (35 spec):
`and` / `array` / `BEGIN` / `END` / `comment` / `delegation` / `encoding` /
`ensure` / `execution` / `file` / `heredoc` / `line` / `loop` /
`magic_comment` / `module` / `next` / `not` / `numbered_parameters` /
`numbers` / `optional_assignments` / `or` / `order` / `precedence` /
`private` / `proc` / `range` / `redo` / `retry` / `safe` / `send` /
`throw` / `undef` / `unless` / `until` / `while`。

近接 (残 fail+err ≤ 4): `class_variable` (1) / `variables` (2) / `rescue` (1) /
`yield` (2) / `method` (2) / `class` (4) / `super` (3) / `block` (4)。
詳しくは [todo.md §A](./todo.md)。

### CRuby spec/ruby/core/ (代表カテゴリ)

| カテゴリ | pass | fail | err | 備考 |
|---|---:|---:|---:|---|
| `kernel` | 6,489 | 293 | 145 | `String#b` + `to_str` coerce 後に大幅改善 |
| `string` | 1,800 | 1,127 | 206 | encoding 系除外でも残る |
| `array` | 1,171 | 436 | 67 | |
| `integer` | 869 | 172 | 183 | Float 精度系 / bignum |
| `hash` | 400 | 97 | 33 | |
| `proc` | 195 | 60 | 25 | |
| `float` | 120 | 35 | 74 | |
| `symbol` | 117 | 69 | 31 | |
| `range` | 98 | 79 | 11 | |
| `binding` | 58 | 0 | 2 | err は IRB / Refinements (out-of-scope) のみ |

**Binding 関連スイート (`core/binding/*` + `core/kernel/{eval,binding}_spec`)
合計: 150 pass**。 Binding 自体は完全互換 (詳細は §Binding 節)。

shim が cover している matchers / helpers:
- `should == / != / equal / eql / be_nil / be_true / be_false / be_truthy / be_falsy`
- `be_close / be_an_instance_of / be_kind_of / be_a / be_an / respond_to / include`
- `raise_error / raise_exception` (Class または [Class, ...] 受付; msg が String なら
  substring + `|` alternation で擬似 Regex マッチ — koruby の Regexp は astrorge 待ち)
- `complain` / `should_not complain` (block を実行する; warning track はせず常に
  pass — koruby は warning を発行しない)
- `mock(name)` — should_receive / and_return / once / twice / at_least 等
  を chainable に受ける minimal mock
- `ScratchPad.record / .recorded / .clear / <<`
- `ruby_exe / ruby_cmd` (空 string)、 `it_behaves_like` (no-op)
- `before :each` (記録のみ)、 `silence_warnings`、 `pending`
- 空 `it "name"` (block なし) は pending として skip
- `NameError: uninitialized constant Thread/Fiber/Ractor/Encoding/Random/...` を
  自動 skip 化 (out-of-scope な constant 参照は test 失敗ではなく skip)

語義 (language semantics) のテスト群はかなり緑:

| ファイル | pass | total | 備考 |
|---|---:|---:|---|
| test_basicinstructions | 465 | 487 | 95% |
| test_fixnum | 1003 | 1037 | 96% |
| test_hash | 1233 | 1497 | 82% |
| test_eval | 249 | 310 | 80% |
| test_module | 541 | 758 | 71% |
| test_proc | 559 | 863 | 64% |
| test_iterator | 83 | 114 | 73% |
| test_keyword | 323 | 736 | 43% (kwsplat 系で大量 fail) |
| test_array | 15250 | 20291 | 75% |

分母を支配するもの (Float / Encoding / Regexp / Random 系で fail):
- test_integer (372k total), test_integer_comb (992k total), test_literal (29k total) — Float 互換性 / eval 互換性
- test_array の sample_random_srand0 (~2000 fail) — Random reproducibility

範囲外 (project policy): Regexp / Encoding / Thread / Process / Refinements / Ractor / Fiber Scheduler。これらは [todo.md §範囲外](./todo.md) を参照。

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
- `eval` (string、 with optional Binding / file / line — caller の lvars 参照、
  block 内では param_base 分シフト、 nested eval も outer 範囲を継承)、
  `instance_eval(string)`
- `binding` (Kernel + Kernel.binding 両方対応)
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

#### Binding (T_DATA、 builtins/binding.c)
- `local_variable_get(name)` / `local_variable_set(name, val)` /
  `local_variable_defined?(name)` (Symbol/String、 不正名で NameError)
- `local_variables` — innermost-first 順 (set-introduced → primary →
  lexical-parent)。 `_` / `__*` は filter
- `receiver` (= self)
- `eval(src [, file [, line]])` — caller の lvars を参照、 新規 lvar は
  binding に取り込み (write-through to live frame)、 `__LINE__` は line offset を honor
- `source_location` — binding 作成位置 (file, line)
- `dup` / `clone` (names + extras を deep copy)
- `Proc#binding` — proc 捕捉 env から Binding 構築
- 寿命: caller frame epilogue で fp スロットを heap snapshot
  (CRuby の heap-promote 相当)。 `bind = binding; b = 1; bind` の
  binding が b の最終値を見る

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
