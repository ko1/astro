# require/require_relative/load: real file loading, load-once dedup, LoadError on
# a missing explicit path, built-in feature booleans. vs ruby. Self-contained.
dir = "/tmp/koruby-req-#{Process.pid rescue 0}"
Dir.mkdir(dir) unless Dir.exist?(dir)
lib = "#{dir}/mylib.rb"
File.write(lib, "MYLIB_CONST = 7\ndef mylib_fn; :ok; end\n$mylib_count = ($mylib_count || 0) + 1\n")
p require(lib)              # true (loaded)
p MYLIB_CONST              # 7
p mylib_fn                 # :ok
p require(lib)              # false (already loaded)
p $mylib_count             # 1 (body ran once)
p load(lib)                # true (load always re-runs)
p $mylib_count             # 2
p require("set")           # false (core-loaded)
p require("stringio")      # true (loads once)
p require("stringio")      # false (now loaded)
begin; require("#{dir}/missing"); rescue LoadError; p :loaderr; end
File.delete(lib); Dir.rmdir(dir)
