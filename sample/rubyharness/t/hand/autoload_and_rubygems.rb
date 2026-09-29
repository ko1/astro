# An autoload for "zed_lp" registered inside sub/zed_lp.rb names the $LOAD_PATH file,
# not the file registering it (rubygems/openssl.rb's `autoload :OpenSSL, "openssl"`).
require "tmpdir"
Dir.mktmpdir do |d|
  Dir.mkdir(File.join(d, "sub"))
  File.write(File.join(d, "zed_lp.rb"), "module ZedLp; X = 1; end\n")
  File.write(File.join(d, "sub", "zed_lp.rb"), "autoload :ZedLp, \"zed_lp\"\np defined?(ZedLp::X)\n")
  $LOAD_PATH.unshift d
  require "sub/zed_lp"
  p ZedLp::X
end
# rubygems is loaded at startup and a bundled gem is found by name
p defined?(Gem::Specification), $LOADED_FEATURES.any? { _1.end_with?("/rubygems.rb") }
p require("minitest"), Gem.loaded_specs.key?("minitest"), Gem::Version.new("1.10") > Gem::Version.new("1.9")
