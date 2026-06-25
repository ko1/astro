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
