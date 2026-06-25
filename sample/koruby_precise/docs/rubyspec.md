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
