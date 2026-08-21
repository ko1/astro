# rubyspec 充足率 (koruby_precise, CRuby drop-in 目標)

計測: `ruby tools/rubyspec_run.rb <spec_dir> [jobs]` (shim+spec+trailer 連結方式)。

## 2026-06-24 core baseline
```
files=1823 (file-clean=421)  whole-file-fail/crash=297
examples: pass=7493 fail=1517 err=6291 skip=519
example pass-rate (of pass+fail+err) = 49.0%
```
worst/whole-file 詳細は WORST=1 で再生成。

## 2026-06-25 進捗 (core, fixtures 解決済の計測)
- 起点 49.0% (fixtures 無し) → fixtures 解決で 56.2% (計測精度向上) → 本日の修正後 example pass 7551 / fail 1520 / err 4617, file-clean 394, whole-file-fail 563。
- 本日入れた drop-in 修正: 既定 method_missing(super解決) / Kernel メソッドを全 self から解決 / Object#define_singleton_method / __FILE__ __LINE__ / alias・alias_method / Module#method_defined? / global `$x ||=`。
- 計測注意: ファイルを unlock すると未実装機能由来の fail/err が露出して % は一時的に下がるが、pass 数・runnable files は増える(より正確)。
- 残 worklist (whole-file-fail tally 降順): splat+block call `f(*a){}`(69) / anonymous rest `def m(*)`(27) / class<<self body(8) / block rescue-ensure(7) / `Foo::Bar=`(2) / stdlib(File 187+/IO/Time/Process/Marshal の NameError) / frozen 強制(FrozenError) / encoding。

## 2026-06-25 splat 系構文を全面サポート
- 起点 pass 7551 / file-clean 395 / whole-file-fail 562 → 修正後 **pass 8282 (+731) / file-clean 415 (+20) / whole-file-fail 496 (−66)**、example pass-rate 55.2%→56.3%。
- 実装した splat 構文 (それまで prism node 139 PM_SPLAT_NODE で wfail):
  - `f(*a){blk}` / `recv.m(*a){blk}` (literal & `&:sym` block) → node_call_splat_blk / node_send_splat_blk
  - `f(*a,&p)` / `recv.m(*a,&p)` (forwarded Proc) → node_call_splat_blkproc / node_send_splat_blkproc。**最大の効き目: mspec_shim の method_missing `@actual.send(name,*args,&blk)` が解けて 51 spec file が unblock。**
  - `yield(*a)` (method top-level / block 内 depth>0 両方) → node_yield_splat / node_yield_outer_splat
  - `super(*a)` → node_super_splat
- いずれも @noinline 系 splat-spread node。検証: 単体 CRuby 一致 / make test 89300 全 PASS / STRESS+PURGE green / AOT(interp=AOT=ruby) green。
- 次の worklist 上位 (whole-file-fail, stderr 集計): Module#prepend(94) / Module#private_constant(63) / IO 未実装(60) / Thread 未実装(41) / anonymous rest `def m(*)`(26) / class<<self body 非def(19) / block rescue/ensure(6) / refine(6) / Module#public(6)。stdlib(IO/Thread/Time) と Module 系 method がボリュームゾーン。

## 2026-06-25 (cont.) Module 系メタプロ + class<<self body + class ivars
- pass 8282→**8327** / whole-file-fail 496→**486** / runnable例 14706→14809。
- 実装:
  - **Module#prepend** — KorbClass に `prepended` 配列追加、method lookup を prepend→own→include→super 順に。`super` を self の MRO 線形化 (korb_linearize_mro) で解決し、prepend module の super が class 本体に届くよう修正。ancestors/is_a?/`<` も prepend 反映。explicit `C.prepend(M)` cmethod も。
  - **private_constant / public_constant / private_class_method / public_class_method** — no-op (koruby は可視性を追跡しない)。
  - **Module#const_set / remove_method / undef_method** — const は flat table へ define、method は sentinel mid (UINT32_MAX) で退役 + method_serial++。
  - **`class << recv ... end` 一般 body** — def 限定をやめ、body を class-body entry に compile して self=recv の singleton class で実行 (node_sclass / korb_sclass_body)。attr_accessor/private/alias/const/式すべて可。
  - **class instance variable** (`@x` on a Class) — KorbClass に `class_ivars` hash 追加。korb_ivar_get/set + node_ivar_get/set + instance_variable_get/set が class self を side hash へ routing。`class << self; attr_accessor` が hang していた真因 (VAL2OBJ on class で memory 破壊) を解消。
- 検証: 全部 CRuby 一致 / make test 89300 / STRESS+PURGE / AOT green。
- 累計 (session): pass 7551→8327 (+776) / file-clean 395→415 / whole-file-fail 562→486。
- 残: prepend 等で unblock した file は次の未実装 primitive (caller_locations / little_endian / refine / IO・Thread・Time) に当たり wfail 継続。stdlib がボトルネック。

## 2026-06-25 (cont.2) anonymous rest `def m(*)`
- 匿名 rest param を許可 (parse-time の reject を撤廃、name==0 なら synth local に surplus を収集して捨てる)。post 付き匿名 rest は除外。
- mock/stub fixture で多用される構文で 31 file unblock。pass 8327→**8477 (+150)** / file-clean 415→**422** / whole-file-fail 486→**455**。
- 累計 (session): pass 7551→**8477 (+926)** / file-clean 395→422 / whole-file-fail 562→455。make test 89300 全 green 維持。

## 2026-06-25 (cont.3) forwarded block → &blk param crash 修正
- splat/prepend/anon-rest で unblock した file が次に踏んでいた **既存 SIGSEGV** を修正: forwarded Proc を `&blk` で再受けする時 node_blkparam が KORB_BLK_FWD(0x2) を deref していた。CPROC 同様 cself(転送 proc)を bind。
- comparable/array/enumerable など `context`/mock 多用 spec の crash 解消。pass 8477→**8579 (+102)**、pass-rate 56.6%。
- 累計 (session): pass 7551→**8579 (+1028)** / file-clean 395→422 / whole-file-fail 562→457。
- 別途記録した既存 crash: 自己参照構造の `korb_deep_hash` 無限再帰 (array/hash, hash/hash) は未修正 TODO。

## 2026-06-25 (cont.4) バグ掃討ラウンド (/goal: fix all bugs, GC+AOT+fastpath 注意)
- pass 8579→**8689 (+110)** / file-clean 422→**435** / whole-file-fail 457→**436** / crash 19→**7**。
- 修正バグ (詳細は todo.md): deep_hash 再帰 / cmp_full・eql? 自己参照再帰 (identity short-circuit, fastpath正) / class<<nil・immediate (singleton guard) / respond_to?(:new) on class / 匿名 rest 転送 `def m(*);foo(*)` / CPROC を builtin iterator に渡す crash / NULL-block yield→LocalJumpError / `Hash.new{capture}` の make_proc def_env tag strip / little_endian・big_endian shim guard。
- 全て CRuby 一致・make test 89300・STRESS+PURGE・AOT green。hot-path bench (fib/times/sort) 回帰なし (interp 0.20s / AOT 0.17s)。
- 残 crash 7 (深部/既知): proc/curry(既知) / lazy enumerator 再設計待ち (with_index,to_enum) / fiber machinery / proc/new 未初期化 block cell / set/to_s 自己参照 print (seen-set 要) / enumerator/new timeout。
- 累計 (session 全体): pass 7551→**8689 (+1138)** / file-clean 395→435 / whole-file-fail 562→436。
- 見送り (fastpath/risk 配慮): private/protected 可視性追跡 (送信側 IC fast path に check が要りホットパス劣化のため)。undef_method は inherited-block marker 無しの近似のまま。

## 2026-06-25 (cont.5) kernel fixture gauntlet 突破 → pass +2121
- pass 8707→**10828 (+2121)** / file-clean 436→**455** / whole-file-fail 433→**359** / pass-rate 56.7%→**61.0%**。
- kernel/fixtures/classes.rb (88 kernel spec が require) が連鎖的に複数機能で whole-file-fail していたのを突破:
  1. `Kernel.instance_method(:x)` — koruby は Kernel の instance method を Object に置くので、Kernel lookup を Object へ fall-through (owned by Kernel module)。
  2. `define_method(sym, UnboundMethod)` — module-owned method は subclass check skip + body を Object から解決 (BasicObject subclass にも bind 可)。
  3. `undef foo, :bar` keyword (PM_UNDEF_NODE) — node_undef で self の method を retire。
  4. **required-after-optional `def m(a=1, b)`** (post-without-rest) — parse 拒否撤廃 + korb_invoke_method の post binding を no-rest 対応 (post_base = rest? rest+1 : params_cnt、arity max = params_cnt+post_cnt)。これは汎用機能で多数 spec に波及し +2121 の主因。
- 全て CRuby 一致 / make test 89348 / STRESS+PURGE / corpus に params_undef.rb 追加。
- 累計 (session): pass 7551→**10828 (+3277)** / file-clean 395→455 / whole-file-fail 562→359 / pass-rate 49%→61%。

## 2026-06-25 (cont.6) Module.nesting + namespaced class/module 名
- **Module.nesting**: kp_frame に class_name_sym 追加、transduce_class/module で set。`Module.nesting` を parse-time 特殊化し enclosing class/module の const 名を innermost-first で bake、runtime (node_nesting) で flat const table から live class object を解決して配列化。flat const なので表示名は `Bar`(vs CRuby `Foo::Bar`) だが配列構造・object は正しい。
- **namespaced class/module 名** (`class A::B` / `module A::B`): parse 拒否を撤廃。flat const table なので namespace path を無視し rightmost name (cn->name) を global 定義。
- module/fixtures/classes.rb (≈63 module spec が require) の nesting/namespaced ブロッカーを突破 (次は File stdlib)。make test 89360 / STRESS green。corpus に nesting_namespace.rb 追加。

## 2026-06-25 (cont.7) stdlib 着手: File パス系メソッド
- pass 10855→**11010 (+155)** / file-clean 456→**470** / pass-rate 60.9%→**61.7%**。
- builtins/file.c 新規: File class + class method (expand_path / join / dirname / basename / extname)。純粋な文字列操作 (実 I/O なし)。Math module の登録パターンを踏襲 (korb_init_file)。
- shim の `fixture()` helper (File.expand_path/join/dirname 依存) + File パス spec を unblock。file/basename pass=69 等。
- **GC 罠**: dirname/basename/extname が source heap string への ptr を korb_str_new に渡し、STRESS で alloc 中に source が move して stale read → SEGV。local C buffer に memcpy してから str_new で修正 (join/expand_path は元々 local buffer 構築で安全)。STRESS の存在意義そのもの。
- 累計 (session): pass 7551→**11010 (+3459)** / pass-rate 49%→61.7%。corpus に file_path.rb 追加。

## 2026-06-25 (cont.8) no-op stub で whole-file-fail を unblock
- top blocker の `autoload`(64) / top-level `public`/`private`(6) / `refine`・`using`(6) を no-op stub で unblock (≈76 file)。
- autoload は koruby が file を lazy load できないので no-op (autoload? は nil 返す = CRuby の path と divergent。const は未定義のまま)。refine/using は refinement scoping 無しの no-op。これらは CRuby と挙動が違うので corpus には足さない (diff 不一致になるため)。
- make test 89386 green。次は IO(60)/Thread(41) stdlib。

## 2026-06-25 (cont.9) block body rescue/ensure + 定数パス代入
- **block body の rescue/ensure** (`foo { ... rescue ... }` / `{ begin..ensure..end }`): def body と同様 PM_BEGIN_NODE を transduce するだけ (kp_unsupported を撤廃)。汎用機能で example レベルにも波及。
- **定数パス代入** `A::B = value` (PM_CONSTANT_PATH_WRITE_NODE): flat const table の rightmost name へ。namespaced 名 [[cont.6]] の対。
- 全て CRuby 一致 / make test 89392 / STRESS / AOT green。corpus に block_rescue.rb + nesting_namespace.rb 追記。

## 2026-06-25 (cont.10) class method 継承 (metaclass hierarchy)
- これまで `def self.foo` を持つ Parent を継承した Child で `Child.foo` が呼べなかった (singleton class の super が Class で metaclass 階層が無かった)。
- 修正2点: (1) korb_obj_singleton で class の singleton の super = parent class の singleton (lazy+memoized recursion)。(2) korb_dispatch_class で own singleton 無しの class は最寄り ancestor の singleton を dispatch class に。
- `Child.class_method` (継承)、2段継承、own+inherited mix、`class << self; alias_method :x, :inherited_cm` 全て CRuby 一致。unboundmethod fixture 等の alias_method class_method(13) を解消。汎用機能。
- make test 89399 / STRESS+PURGE / AOT green。corpus に class_method_inherit.rb 追加。

## 2026-06-25 (cont.11) 最後の実 crash 撲滅 + Time class
- **実 SEGV を 0 に**: proc/curry の crash は env_size bug ではなく `instance_exec(&forwarded_proc)` が FWD captured_self を新 receiver で上書きして VAL2PROC(receiver)->env を読んでいたバグ。同系で `define_method(:x, &forwarded_proc)` も修正 (FWD なら cself の proc をそのまま使う)。crash 19→**0** (残 enumerator/new は timeout のみ)。
- **Time class** (builtins/time.c): double epoch backing。Time.now/at/utc/gm/local/mktime/new、year/mon/day/hour/min/sec/wday/yday/usec、to_i/to_f、+ - <=> ==、Comparable、strftime、to_s、utc?/getutc/getlocal。tz は localtime/gmtime、leap sec 無視。corpus に time_basic.rb (deterministic UTC)。
- make test 89406 / STRESS+PURGE / AOT green。

---

## 2026-07-13 現状サマリ（06-25 の ~56% から大幅前進）

06-25 以降、複数セッションで core 全域を掃討し、**core example pass-rate 84.6%** に到達。

```
files=2097 (fully-clean = 873, 42%)
examples: pass=44,673  fail=4,437  err=3,723
example pass-rate (of pass+fail+err) = 84.6%
```

corpus（`make test`）は golden **93,399 件 0 fail / 0 crash**、STRESS+PURGE / AOT すべて green を維持。

## 2026-07-23 language+core 100% 目標グラインド開始（除外域以外）

充足率実測: **language 82.4% / core 86.2% / library 14.9%**（library はほぼ require/gem/native 除外域）。
到達可能失敗プールの最大レバーは **Enumerator/Lazy ~400**、次いで coercion TypeError ~70、MRO/prepend ~48。

本ラウンドの修正:
- **mspec shim: mock の実 class を MockObject に**（MSpecMock→MockObject merge）。inspect/type-name が
  "MockObject" に。array/hash の mock-inspect が数件通る。
- **Range#reverse_each on endless → TypeError**（`can't iterate from NilClass`、従来 RangeError）。
- **Enumerator::Lazy#grep / #grep_v を lazy 化**: 従来 Enumerable#grep で eager materialize（Array 返却・
  infinite source 過剰反復）。deferred chain op 化（`korb_lazy_chain2` で [op,pattern,block]、両 driver で
  `pattern === value` を実 === dispatch + optional block map）。lazy/grep 5→18, grep_v 5→18。
- **Enumerator::Lazy#uniq を lazy 化**: per-drive seen-Hash（両 driver: korb_lazy_apply の Ruby-array op_state、
  LAZY_FEED の rooted seen-Hash 配列）で dedup。lazy/uniq 0→9。
- **Enumerator#find_all を lazy-aware alias(select) に**。
- **`Enumerator::Lazy.new(obj, size)` 構築 + lazy `#size` 伝播**: 従来 "not an enumerator" で全 size test が
  err。Lazy.new 特例（lazy generator 構築 + size 格納）、lazy enum に実 `#size`、chain で伝播（map/collect 保存、
  take(n)=min、drop(n)=max(0,size-n)、grep/select/…→nil、`arr.lazy`=Array len、finite int Range=count）。
  map/select/take/drop/drop_while 群 +~15。注: e->size は非 GC-scanned なので immediate（Fixnum）のみ格納
  （endless range の Infinity は box→stale で SEGV したため nil。STRESS で発見・修正）。
- **Integer/Float の `**` `%` が obj#coerce を honor**（従来 +/-/*// のみ）。integer/pow 38→39。
- **`Method#to_s`/`#inspect` に param signature + source location**: `#<Method: C#m(x, y=..., *z, k:, **n, &b) file:line>`
  形（param_info から req/opt=.../\*rest/key:/\*\*kwrest/&block、top-level は global fn table fallback、alias は
  `#renamed(original)`）。method/to_s 9→21、unboundmethod/to_s 7→10。
- **`Symbol#to_proc#to_s` に `(&:name)`**。
- **`Enumerator::Lazy#flat_map` を lazy 化**: source-path の driver（LAZY_FEED macro）を再帰的 per-value
  processor `korb_lazy_run` に書き換え。flat_map は Array block-result を 1段 flatten し各要素を
  ops[oi+1..] へ再投入（fanout）、非 Array は単一値として通す。全 op（map/select/…/grep/uniq）が同関数経由。
  lazy/flat_map 0→12。STRESS で fanout 検証。
- **regression 自己修正**: 上記 lazy grep/uniq/flat_map が `mode != 0` で chain していたため plain generator
  (`Enumerator.new{}`, mode 3) が非駆動 Enumerator を返していた（enumerable/uniq が検出）。lazy mode(1/4)
  のみ chain、0/2/3 は to_a+Array method に修正。enumerable/uniq 0→6。
- **`Range#size`**: Integer 始点で終点 `Float::INFINITY` を Infinity に（従来 nil）。
- **`Enumerator::Lazy#with_index` を lazy 化**: korb_lazy_run に index counter（op_state, offset 初期化）で
  [value, idx] or block(value, idx) を生成。size 保存。lazy/with_index 1→5。
- enumerator category **219→298**（本セッション +79）。lazy の source-path op はほぼ完備
  （map/select/reject/filter_map/take/drop/*_while/compact/grep/grep_v/uniq/find_all/flat_map/with_index）。
- **残（Enumerator, 要 driver 統合 or 構造）**: generator-path（mode 4, korb_lazy_apply）の flat_map fanout は
  未（collector-aware な korb_lazy_run へ両 driver 統合が要）、zip（多源）、next/peek/rewind（external iteration）、
  multi-value yield。他カテゴリの残は introspection-fidelity（curry の parameters/source_location、Kernel を
  instance method 化）、Data/Struct init override（構築 refactor）、encoding/mock（除外・shim 高リスク）。
- **確認済の壁**: mock respond_to fidelity（shim で to_str 等を real method 定義 or respond_to_missing?=true が
  coercion を誤らせる）は default 変更を試すも integer/float coercion が -46 regress → revert（shim は高リスク確定）。
  Data/Struct#initialize override は default initialize が C コードで callable super 無し → 構築 refactor 要（architectural）。
  flat_map/with_index/zip の lazy 化は fanout/index/多源で構造的。Enumerator の残はこれら。
- enumerator category **219→283**（本セッション +64）。err 119→92。全て corpus 93,399/0 + STRESS + fuzzer 875/0。
- **残（Enumerator, 構造的/edge）**: flat_map/with_index/zip の lazy 化（1入力→多出力/index/多源）、
  generator-driving edge（take(0) は no-yield 等）、endless range の Infinity size（要 GC-scanned size field）。
- **残（横断, 低リスクだが thin）**: coercion TypeError の per-method 追加、error-class mismatch、
  各カテゴリ 1-3 test/file の long tail（Set#== の eql? 厳密性等、subtle）。mock respond_to fidelity と
  encoding は shim/除外域。

## 2026-07-16〜21 大型機能 + 言語クラッシュ掃討 + 健全性ファジング

- **Marshal を CRuby 4.8 wire format に全書き換え**（`prelude/marshal.rb`）: object links(`@`)・symbol table/links(`;`)・String encoding wrapper(`I`+`:E`)・Class(`c`)/Module(`m`)・`_dump`(`u`)/`marshal_dump`(`U`)・subclass(`C`)/extend(`e`)・Regexp(`/`)・Data(`S`)・Exception(:mesg/:bt)・compare_by_identity・nested const 解決・load proc。**dump 0→146 / load 53→235 pass**（round-trip byte 一致）。→ **Marshal byte-exact は「除外」から外れた**（主流オブジェクトグラフは一致）。
- **send/block-path の `Class#new` が builtin singleton `new`(CFUNC) を honor**（Regexp/Time/File/Dir）。shared-example が `send(:new)` で construct するため **core +2,302**（time/new 49→2222 等）。
- **言語のクラッシュ3ファイルを実行ベース bisect で根治**（`describe/it→xit` で構造保持二分）:
  - `$stdout=obj; print/puts` の無限再帰(redirect 時 Kernel#print 自己ループ)→ buffer+`.write`。
  - massign/array-literal RHS の user `<<`(node_shl の send_cached frame 衝突)。
  - proc 内 `rescue <captured-var>`(node_rescue が matcher を slots+1 で eval、chain 不整合)→ matcher を chain+1 bake + 非Module→TypeError。
  → **language 68.8%→76.1%**。
- **`defined?`** の regexp match global(`$~/$&/$1..`)・nil 代入 ivar/global(231→276)。
- **`!=` override** を honor + `Object#!=` の identity 比較 latent バグ修正（差分ファザーが検出）。
- **多重代入の index ターゲット** `h[:a],h[:b]=1,2`（`[]=` desugar）。
- **定数解決を MRO ancestry 順に**（flat first-match→superclass/module 優先が CRuby 一致）。constants 91→102。
- **`super` が全パラメータ種別（req/opt/rest/post/kw/kwrest）と incoming block を forward**（`def m(*rest);super;end` / `super(args)` の暗黙 block、rest の in-body 変更も反映）。super 48→108。
- **差分ソートネスファザー `tools/fuzz_soundness.rb` 新設**: massign/binop/rescue/closure/pattern-match/Enumerable/Hash/Range/super/method_missing/Struct/Data + ランダム式木 × 5文脈を ruby と diff、`--stress` で GC crash 検出。**875 snippets 0 crash / 0 semantic diff**。static 点検（node.def の slot/frame offset 型バグ）も残存無しを確認。
- 全修正: corpus 93,399/0 + STRESS+PURGE clean + ruby一致 + fuzzer 875/0 で検証済。
- **残る到達可能ギャップ**: `super` の block forward は depth==0 のみ（nested block 内 super は未）、`X::Foo`(X 非module)→TypeError（explicit-path/bare-read の node 区別要）、method/massign の mock-protocol coercion（除外）、require/load/autoload・const_source_location・eval 定数スコープ（infra）。

## 2026-07-21 パラメータ束縛 + キーワード引数の CRuby 追従

引数束縛まわりの残ギャップを潰した（method_spec 188→205、lambda/proc/keyword_arguments に波及）:

- **block/lambda の `opt + rest + post`**: `->(a, b=1, *c, d).call(1,2)` が `b=2,d=nil` になっていた
  （前方 loop が optional を greedy に食い、post 用の引数を奪う）。optional は `npost` 個を予約した後に
  だけ束縛し、post は末尾 `npost` 個から取るよう修正 → `[1,1,[],2]`。method 側は元々正しく、block 側の
  `korb_block_yield_full` だけの不整合だった。
- **匿名 `**`**: `def m(**)` / `def m(a, **)` / `def m(a:, **)` が「unknown keyword」で落ちていた
  （名前なし kwrest が `kwrest_slot=-1` = strict と区別できなかった）。匿名 `**` に専用 sentinel
  `kwrest_slot=-2` を与え、strict fast path を外して余剰キーを黙って捨てる。
- **`**nil`**: keyword 構文の呼び出しを「no keywords accepted」で拒否し、位置引数の Hash リテラルは
  位置引数のまま残す（`def m(a, **nil); m({a:1})` → `a={a:1}`）。sentinel `kwrest_slot=-3`。
- **空 keyword splat の脱落（CRuby 3.0+）**: `m(**{})` / `m(**h)`（h 空）が `{}` を位置引数として渡していた
  （0-arg メソッドで「given 1」、`def m(*a)` が `[{}]`）。trailing が keyword bundle の呼び出しに
  `node_call_kws`（implicit self）/`node_send_kws`（receiver）を新設。dispatch 時に staged kwargs Hash が
  空なら `cur=slots-1, argc-1` で呼び、base と self@base[-1] を据え置いたまま空 Hash を callee scratch に落とす。
  非空 bundle は従来通り。keyword_arguments 24→26。
- **index op-assign が代入値を返す + `&&=`**: `recv[k] op= v`/`recv[k] ||= v` が `[]=` の戻り値
  （CRuby は代入した RHS）を返していた（`c[:a]||=12`→7）。新値を temp に計算→`[]=`→temp を式の値に。
  `recv[k] &&= v`（PM_INDEX_AND_WRITE、未実装だった）も追加。optional_assignments 21→36。
- **constant op-assign**: `X ||=/&&=/op= v` と `A::B` path 形が未実装だった（"M0 unsupported"）。desugar:
  `||=`→`defined?(X) && X || (X=v)`（`&&` が undefined 時に read 前で短絡→NameError 回避、flat const table の
  nil=空 sentinel で nil 定数は未定義扱い→代入）、`&&=`→`X && (X=v)`（read 先行→undefined は NameError）、
  `op=`→`X = X op v`。path は static owner（`A::B`/`A::C::B`）対応、dynamic module part は未対応。
  optional_assignments 36→52。（副産物として `X = nil; X` が NameError になる flat-const-table の
  architectural 制約を確認—op-assign は defined? 短絡で回避。）
- **lambda を block として渡して yield すると arity を強制**: `m(&lam); yield ...` が lambda を
  block 扱い（lenient）していた—引数過不足を nil bind/drop、単一 Array を auto-splat。lambda は
  positional arity を厳格化し auto-splat しない。FWD 経路の proc は captured_self から届くので
  fast/full 両 yield 経路で is_lambda を見て ArgumentError（opt/rest/post 考慮）+ auto-splat 抑止。
  plain block/proc は不変。yield 38→42（fail 4→0）。**続けて `lambda#call` 経路も**: `->(a,*b){}.call([1,2,3])`
  が array を splat していた（yield-fix は FWD 経路のみ、proc.call は proc の env を直渡しで fwd=false）。
  korb_block_yield_full に明示 is_lam を通し rest/opt lambda の proc.call を _full 経由に。
  （simple lambda は arity check が先に落ちるので rest/opt のみ要対応。）proc 28→29、lambda 71→73。
- **block/lambda `**nil` は trailing Hash を位置引数に**: parser は既に kwrest_slot -3 を付けていたが
  korb_block_yield_full が Hash を kwargs として剥がして捨てていた。method 側 rule に合わせ -3 で剥がさない。
  proc 29→30、lambda 73→81。
- **defined? の充足**: (1) `defined?(A::B)` を owner-aware に（新 node_defined_cpath—flat rightmost 名 probe
  では `Undefined::Z`/`M::NotOnM` が "constant" 誤判定）。(2) `defined?((expr))` の括弧を unwrap（最内の
  最終 statement に適用、`defined?((a,b=1,2))`→"assignment"）。defined 278→281。
  残: super/class-variable/yield-in-block は frame offset 依存で保留、literal Array 要素の再帰 defined? も未。
- **多重代入は coerce 後の Array でなく元の RHS を返す**: `(a, b = obj)`（`obj#to_ary`→[1,2]）が
  [1,2] を返していた（CRuby は obj）。massign 3 ノードが slots[0] を to_ary 結果で上書きして返していたのを、
  元 RHS を slots[0]（cursor 下＝rooted）に残し copy を slots[1] で coerce、het/splat は ivar/splat-array
  scratch を 1 slot 上げる。variables 133→143。
- **massign の非 local splat target**: `@x, *y = ...` / `CONST, *rest = ...` / `h[k], *rest = ...` が
  "non-local splat target" で未対応だった。target が全 depth-0 local でなければ desugar—検証済の
  all-local `node_massign_splat` で synth local に massign し、各 synth を実 target（@ivar/CONST/$g/
  recv.setter=/recv[k]=）へ plumb（新 `assign_target_from_synth`）。massign の戻り（元 RHS）を temp に
  退避して式値に。variables 143→144。
- **frozen class/object への def は FrozenError**: frozen class 内 `def m`、`def frozen_obj.m`、
  `class << B(frozen); def m` が黙って method 追加して symbol を返していた。node_def/node_singleton_def に
  frozen guard。singleton class への def は attached object の変更なので `korb_check_def_frozen` が
  sklass table で singleton→attached を辿って判定。def_spec の frozen-def 群が pass。
  同型の gap を frozen-mutation 全面掃討: `alias` / `attr_reader/writer/accessor`（builtin + parse-time
  node_attr）/ `remove_method` / `include` / `prepend` / `extend`（singleton 経由）/ `const_set` /
  `@@cvar =` + `class_variable_set`（korb_cvar_set 集約）。undef_method/define_method/ivar_set は既存。
  case/when の `===` も修正（下記）。
- **private/protected NoMethodError が実 receiver class を名指し**: 全 user object で "an instance of Object"
  と誤報していた（`korb_a_type_name` は vm を持たず class 名を引けない）。undefined-method 側が既に使う
  `korb_recv_desc` に置換 → `Foo.new.priv` が "an instance of Foo"（namespaced/anonymous も正しく）。
- **case/when の誤 identity short-circuit を撤去**: node_caseeq が when値と subject が同一 object なら
  === dispatch 前に true を返していた。Class/Module when値では誤り（`case Symbol; when Symbol` は
  `Symbol === Symbol` = is_a? = false）、NaN も同 object で false。identity check を撤去し immediate/値型は
  korb_value_eq、Class/Range/Regexp/Proc/user は実 === に。case 43→44。
- 全修正: corpus 93,399/0 + STRESS+PURGE clean + ruby一致 + fuzzer 875/0 で検証済。
- **既知の architectural gap（本ラウンドで確認、未修正）**:
  - flat const table の nil は「削除済/予約」marker（`remove_const`=set.c で val=nil、Comparable/Enumerable/
    Numeric の slot 予約も nil）。よって `X = nil; X` / 定数 target が nil を受けると NameError になる
    （`SINGLE_RHS_1, SINGLE_RHS_2 = 1` の 2 つ目等）。修正には KORB_UNDEF sentinel 導入が必要で
    remove_const/bootstrap/const_get 全体に波及するため保留。
  - **default definee (cref) 不在**: koruby は `def` の定義先を self で決める（class body/top-level では正しい）。
    しかし method 内の nested `def`（`def o; def i; end; end`）は self=instance→非class→`korb_method_define`
    で global 関数として leak（本来は enclosing method の owner class の instance method、かつ nested def は
    常に public）。同様に `Foo.instance_eval { def bar }` は class receiver の singleton へ行くべきが誤る。
    正しくは「実行中 method の owner を cref とする」default-definee 概念が要る（frame に cref を通す）。
    `force_public` だけでは定義先 bug を解けないため保留。`class_eval { def m }`（instance method）は正動作。
- **残ギャップ**: `**{a:1}`/`m("a"=>1)` の keyword 構文分類（位置 Hash 扱い→架構級, version-sensitive）、
  lambda の `(lambda)` inspect marker（`&l` 捕捉で is_lambda が落ちる、block-forward ABI に is_lambda を
  通す必要）、method_spec の splat×#to_a は mspec mock の respond_to 挙動依存（除外）。

### この間に入った主なもの
- **本物の正規表現**（`libastrogre` 経由）: Regexp/MatchData、`=~`/`$~`/`$1..`、scan/match/split/sub/gsub
  （`\1`+block）、名前付きキャプチャ、lookaround、POSIX class、`/i`・`/m`。**matchdata 96% / regexp 55.6%**
  （残は encoding 依存 edge）。→ regex は「意図的除外」ではない（[[project_koruby_regexp_deferred]] 修正済）。
- **数値**: bignum(GMP) 各種、Rational#rationalize/round(half:)、Complex、CRuby 互換 MT19937。
- **Array 集合演算を #hash+#eql? プロトコル化**（&/|/-/uniq(!)/intersect?、identity short-circuit）。
- **Hash**: rehash（recompute+dedup）、Hash.new(&pr) identity、default_proc 保持、merge。
- **String**: count/delete/squeeze/tr codepoint 対応、bytesplice 境界、lines/each_line chomp `\r\n`、
  `[]` の TypeError、to_c 文法、sprintf `%<name>`/`%{name}`。
- **Module**: public/private/protected 戻り値+Array 引数、undef_method Frozen/Name、include? TypeError、
  append_features/prepend_features、MRO 線形化 super、class method 継承。
- **Kernel/Object**: send/public_send 無引数 ArgumentError、clone(freeze:) 検証、caller/backtrace、
  Object#inspect(addr+ivars)、public_methods/private_methods（global builtin 含む）。
- **stdlib**: ENV/ARGV/File/Dir/IO/StringIO/Marshal(一部)、Time sub-second、pack/unpack(P/p/U/w/M)。
- mspec shim 修正が大きく効いた: `it_behaves_like`(+11.8k)、`should_not complain`。

### 残りの失敗分布（dir 別 fail 合計, 2026-07-13）
```
string 743  module 443  kernel 419  [encoding 386]  [io 364]  [marshal 338]
time 222  [file 146]  array 141  enumerator 99  exception 93  hash 73
[thread 69]  proc 68  [dir 64]  [process 59]  regexp 57  enumerable 54
```
`[...]` = 意図的除外 / インフラ隣接（encoding 変換・Marshal byte format・IO/File/Dir・Thread/Fiber/Process/Signal）。
string の多くも encoding 依存。**到達可能な伸びしろ**は module / kernel / array / enumerator / exception / proc の
long tail（mock protocol、message 整形、deep-MRO nested super、sized Enumerator など、各々アーキテクチャ級）。

### 意図的除外（rubyspec 100% から外す領域）
encoding transcoding / Unicode case、真の並行性（Thread 同期モデル・Ractor・Fiber scheduler）、
gem/require/autoload/native 拡張。encoding 依存 regex もここ（regex 本体は対応済み）。
（**Marshal byte-exact は 2026-07 に CRuby 4.8 wire format へ全書換して除外から外した** — 主流オブジェクトグラフは byte 一致。残るのは fixture の encoding tag 依存・Time#_dump binary format など encoding/edge のみ。）

## 2026-08-13〜14 実 mspec sweep ラウンド（75.9% → 80.2%）

計測は **本物の mspec を無改造 spec に噛ませる** `tools/mspec_real_run.rb`
（shim の `tools/rubyspec_run.rb` は `it_behaves_like` や mock が独自実装で
pass を水増しするので、実数はこちらで見る。DUMP は **ENV** で渡す：
`DUMP=core.tsv ruby tools/mspec_real_run.rb ~/ruby/src/master/spec/ruby/core 12`）。

```
起点   files=2144 clean=946   examples=22,263 pass=16,906 fail=3,372 err=1,985  → 75.9%
現在   files=2144 clean=1,026 examples=22,326 pass=17,908 fail=3,111 err=1,307  → 80.2%
whole-file-fail 15 → 9    make test 100,098 → 100,354 PASS
```

**⚠ sweep 中に `make` を走らせない。** バイナリを奪い合って whole-file-fail が
15 → 1,579 に化ける（実測）。同じ理由でバックグラウンドのビルドと sweep は同時 1 本。

### 見つかった汎用バグ（spec 以外にも効くもの）

- **splat 呼び出しで空 kwsplat が elide されない** — `m(*a, **h)` の splat 経路
  （`build_array`）が keyword bundle を positional に押し込んでいた。`f.raise(*args, **kwargs)`
  形の delegation が軒並み壊れる。`node_ary_push_kw` を追加して固定 arity 経路と規則を揃えた。
- **`define_method(:m, &SomeClass.method(:x))` が LocalJumpError** — Method#to_proc 由来の
  Proc は node_entry を持たず、captured_self に「Proc 自身」を要求するのに、DM 経路が
  receiver スロットを渡していた。
- **`Klass.send(:new)` が define_method した特異 `new` を無視** — mid_new カスケードが
  override として ISEQ/CFUNC しか認めていなかった（直接呼び出しは効いていたので send 経由だけの差）。
- **Regexp が inspect で `#<Object>`** — C の inspect プリンタに `KORB_OBJ_REGEXP` の枝が無く、
  `p /x/` や配列/Hash 内の Regexp が全部 `#<Object>` になっていた（`Regexp#inspect` 自体は正しい）。
- **`Kernel#exit` が exit(3) 直叩き** — SystemExit を raise しないので rescue も ensure も効かず、
  テストフレームワークが exit を捕まえられない。SystemExit 化して main.c で終了コードに変換。
- **未捕捉例外の表示が etype 名** — LoadError/ThreadError やユーザ定義サブクラスが全部
  RuntimeError と表示されていた（exc_class を見るように）。
- **双方向ストリームのデッドロック 2 種** — read(2) でブロックする前に自分の書き込みバッファを
  flush していなかった／`IO.copy_stream` が `#read(n)`（n バイト揃うまで待つ）を使っていた。
- **`define_method` の body 内 `break`** — lambda 意味論なのにフレームを突き抜けてプログラムごと終了していた。
- **変換プロトコルの `respond_to?`** — CRuby (rb_check_funcall) は private も見えるよう
  2 引数で尋ねる。koruby は 1 引数だった。

### 新規実装

IO::Buffer（`prelude/io_buffer.rb`、binary String + offset 表現、valid? は親 generation と
bounds で判定）/ IO.copy_stream / autoload（const_missing 経由＋`$LOADED_FEATURES`）/
Fiber の raise・transfer・kill・storage（`Fiber[]` / `#storage` / `Fiber.new(storage:)`）/
File::Stat の権限述語・dev_major 系・birthtime(statx) / File#lstat / Kernel#putc /
Process の priority・CLOCK_*・times(getrusage)・groups・waitall / Regexp の linear_time?・
timeout・try_convert・fixed_encoding? / GC.config 系 / Binding の implicit parameter API
(`_1..._9` / `it`) / IO#close_read・close_write / Enumerator::Lazy#eager / Process::Status.wait。

### CRuby 追従（既存機能の詰め）

String#unpack のディレクティブ解析を全面的に合わせた（`#to_str`・`#` コメント・`X`・`@`・
修飾子検査・`u`・結果 encoding・`Z*`・`U` の malformed 検出）ので unpack バケツは 61err → 0。
Array#pack も `@`/`Z`/`M`/`m`/`u` と結果 encoding（CRuby の enc_info 状態機械）を合わせた。
ほかに IO の Integer 引数 `#to_int`・IO.new の options Hash・File.open の keyword options と perm・
require/load のパス変換と LoadError#path・Time の秒未満精度（clock_gettime）と `in:`・軍事 TZ・
Time.at の Rational 精度・Kernel#Rational/Complex/exit の引数変換・String#byteslice/bytesplice の境界。

### 残っている構造的ギャップ

`docs/todo.md` に詳細。要点だけ：

- **`eval(str)` が呼び出し元のローカルを見ない**。parse 時に `eval(str, binding)` へ
  desugar すれば同一フレーム分は直る（実装・計測済み: kernel/eval 17fail+16err → 14+12）が、
  **body root が `node_binding` のメソッドが AOT bake されない**バグに当たって optcarrot の
  AOT ゲートが落ちるため revert して保留。`@noinline` を付けても exempt されなかった。
- **Binding が外側スコープのローカルを列挙しない**（closure chain 未走査）。
- `core/io/copy_stream_spec.rb` が今ラウンドの IO 変更で whole-file timeout に退行。
  単体で切り出したケースは全部 CRuby 一致で通るのに spec 実行だと無限ループする。
- ObjectSpace.each_object は GC のヒープ走査 API が要る（未着手）。
- Encoding::Converter の実 transcoding、Unicode normalization、TracePoint は従来どおり除外域。

### 残 error バケツ（core, err 数）

```
kernel 106  string 93  io 75  file 72  [tracepoint 69]  time 61  module 61
enumerator 54  [encoding 50]  thread 45  process 35  regexp 34
```
`[...]` = 除外域。単一ファイル最大は tracepoint/enable 32（棚上げ）、time/new 29、
objectspace/each_object 19、kernel/eval 16。

## 2026-08-20 core sweep (実 mspec)

```
files=2144 clean=1071  whole-file-fail=6
examples=22468 pass=19145 fail=2215 err=1108
example pass-rate = 85.2%
```
起点 (同日朝) pass=18740 / 83.4% → **19145 / 85.2% (+405)**。language も
2134 / 75.9% → 2225 / 79.2%。計測は
`DUMP=<path> ruby tools/mspec_real_run.rb ~/ruby/src/master/spec/ruby/core 12`。

この日の効いた修正 (大きい順):
- **String のエンコーディング枠 5 → 29** (ヘッダ索引を 3bit → 5bit)。
  core/encoding/compatible_spec 109F → 6F。
- **IO.popen の `err: [:child, :out]`** を実装。mspec の `ruby_exe(..., args: "2>&1")`
  が空文字列を返していたため、stderr を見る spec が全て落ちていた。
- **IO の external/internal encoding** を CRuby の rb_io_ext_int_to_encs と同型に
  (set_encoding 49F→3F、external 16F→0、internal 18F→0)。
- **Regexp.union のエンコーディング交渉** (21F3E → 0)、Regexp#fixed_encoding?。
- **Time.new(String)** を time_init_parse と同型に (22 → 2)。
- **END { } / at_exit の終了処理** (END_spec 0/14 → 13/14、at_exit_spec 3 → 12/13)。
- Marshal の深さ制限・特異クラス ivar・非 ASCII ivar 名、Module#const_added、
  eval 文字列の cref、respond_to_missing? の既定、IO.pipe のエンコーディング引数。

- **キーワード引数を Hash ヘッダの印 (KORB_FL_KWARGS) で判定**するようにし、
  Ruby 3 の「末尾の Hash は位置引数」を実装。keyword_arguments_spec 15 → 4F、
  ruby2_keywords_spec 14 → 3F、hash/ruby2_keywords_hash_spec 6 → 1F。
- 文字列連結 (+ / << / []=) のエンコーディング交渉と Encoding::CompatibilityError、
  \u エスケープを含むリテラルは UTF-8、ソースの magic comment のエンコーディングを
  実際に使う (magic_comment_spec 30 → 0F)。
- BOM (IO#set_encoding_by_bom と "rb:BOM|enc")。

残りの上位 (in-scope): io/write_spec と string/encode_spec は **実 transcoding** 待ち、
module/refine と refinement/* は refinement 未実装、kernel/caller は
フレームに現在行を持たせる設計変更待ち (docs/todo.md)。

## 2026-08-21 core sweep (実 mspec)

```
files=2144 clean=1081  whole-file-fail=6
examples=22468 pass=19254 fail=2140 err=1074
example pass-rate = 85.7%
```
(同日の途中経過は 19172 / 85.3%。以降 IO の読み取りエンコーディング・
lineno/$. ・Kernel#Complex・Process::Status.wait を入れて 19238。)
language は 2243 / 2810 = 79.8% (clean 17)。

08-20 の 19145 から: キーワード引数の印の穴埋め (super 転送 / Data / Struct)、
ruby2_keywords、組込みサブクラスの #allocate (Marshal の 'C' ラッパもこれ経由に)、
Float の step を ruby_float_step と同じ数え方に、sprintf の結果エンコーディングと
%s の文字単位の精度/幅、Fiber storage 一式、const_source_location のスコープ付き名、
Process::Status.wait が $? を変えない。

IO 系のまとめ (08-21 後半): 読み取り結果に **ストリームのエンコーディング** を付ける
(internal || external、rep に memo して set_encoding で無効化)、gets のバイト上限が
文字を割らない、#lineno を readlines/rewind と噛み合わせる、each_line/foreach の
$. と $_、ブロック無し each_line/foreach の Enumerator は #size が nil。
Kernel#Complex は組込み以外の Numeric を #real? 経由で扱い、String 引数を
解析前に検査する (これで complex/to_r・to_i・to_f 等の fixture も通るようになった)。

さらに: IO.new/IO.open のモード・エンコーディング二重指定を ArgumentError に、
Kernel#load が $LOAD_PATH (と #to_path) を探す、定数再定義警告を
"file:line: warning:" の形に、Module#autoload? が祖先の登録を見る、
Marshal.dump の String が extend を落とさない。

## 2026-08-21 最終 (sweep_0821d)

```
files=2144 clean=1079  whole-file-fail=7
examples=22444 pass=19242 fail=2129 err=1073
example pass-rate = 85.7%
```
このダンプは 12 並列のゆらぎを踏んでいる。単体で流し直すと
core/process/exec_spec (24 例, 並列時 WFAIL)、core/process/status/wait_spec (10 例)、
core/io/buffer/map_spec (25 例) はいずれも全通し。それを戻すと 19269 / 85.9% で、
0821c (19254) から実質の退行は無い。subprocess と mmap を使うファイルは
並列度 12 だと 20 秒のタイムアウトに触れることがある、と覚えておく。

0821c からの差分: sprintf/format の結果エンコーディングは 7bit でない引数だけが
決める + $VERBOSE 時のみ "too many arguments for format string" (%{} と %<> は
名前付きとして数える)、IO#write は空文字列だけなら書き込み可否を見ない。

### プロセス起動まわり (0821d の後)

sweep_0821d で退行に見えた 2 件は本物だった (単体実行を CRuby で流していて
気付くのが遅れた。単体は `./koruby_precise tools/mspec_launch.rb <spec>`)。
- io/buffer/map_spec: 書き込み可否を `file.write("")` で探っていたのを、
  直前に入れた「空文字列の write は権限を見ない」が無効化していた。
- process/status/wait_spec: `pgroup: true` を無視していたため、
  グループを分けた子と分けない子の終了順の競争になっていた (3 回に 2 回落ちる)。

そこから spawn/exec/system を CRuby の規約に揃えた (commit af41bbef)。
単体実行で spawn_spec 62→91/93、exec_spec 18→23/24、system_spec 7→14/15、
kernel/spawn_spec 1→3/3。残りは spawn の close_others 系 1F1E と
system の「shebang の無い実行可能ファイルを sh にフォールバック」1F。
