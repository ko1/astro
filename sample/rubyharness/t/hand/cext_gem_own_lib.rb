# A gem that ships a feature as a C extension (`bigdecimal.rb` beside
# `bigdecimal.so`, as the bigdecimal gem json_schemer depends on) cannot load
# on koruby, so koruby's own bigdecimal stands in.  CRuby loads the real gem,
# so only koruby runs the stand-in case; a feature koruby does not carry keeps
# loading from the gem dir on both.
require 'tmpdir'
Dir.mktmpdir do |d|
  File.write(File.join(d, "cext_probe_feature.rb"), "CEXT_PROBE = :gem_copy\n")
  File.write(File.join(d, "cext_probe_feature.so"), "")
  $LOAD_PATH.unshift(d)
  require "cext_probe_feature"
  p CEXT_PROBE
  if RUBY_DESCRIPTION.include?("koruby")
    File.write(File.join(d, "bigdecimal.rb"), "raise 'the C-extension gem copy was loaded'\n")
    File.write(File.join(d, "bigdecimal.so"), "")
  end
  require "bigdecimal"
  $LOAD_PATH.shift
end
p BigDecimal("1.5") * 2
