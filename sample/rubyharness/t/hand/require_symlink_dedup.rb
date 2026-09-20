require "tmpdir"
# 同じファイルに symlink 経由で二度たどり着いても require は 1 回
# (CRuby と同じ)。$LOADED_FEATURES と __FILE__ は「実際に使った方」のパス。
base = File.join(Dir.tmpdir, "korb_sl_#{Process.pid}")
Dir.mkdir(base); Dir.mkdir(File.join(base, "real")); Dir.mkdir(File.join(base, "link"))
File.write(File.join(base, "real", "sl_m.rb"), "$sl_n = ($sl_n || 0) + 1\n")
File.symlink(File.join(base, "real", "sl_m.rb"), File.join(base, "link", "sl_m.rb"))
$LOAD_PATH.unshift File.join(base, "link")
$LOAD_PATH.unshift File.join(base, "real")
p require("sl_m")
$LOAD_PATH.shift
p require("sl_m")
p $sl_n
p $LOADED_FEATURES.grep(/sl_m/).map { |f| f.sub(base, "") }
File.unlink(File.join(base, "link", "sl_m.rb"))
File.unlink(File.join(base, "real", "sl_m.rb"))
Dir.rmdir(File.join(base, "link")); Dir.rmdir(File.join(base, "real")); Dir.rmdir(base)
