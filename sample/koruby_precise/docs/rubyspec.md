# rubyspec 充足率 (koruby_precise, CRuby drop-in 目標)

計測: `ruby tools/rubyspec_run.rb <spec_dir> [jobs]` (shim+spec+trailer 連結方式)。

## 2026-06-24 core baseline
```
files=1823 (file-clean=421)  whole-file-fail/crash=297
examples: pass=7493 fail=1517 err=6291 skip=519
example pass-rate (of pass+fail+err) = 49.0%
```
worst/whole-file 詳細は WORST=1 で再生成。
