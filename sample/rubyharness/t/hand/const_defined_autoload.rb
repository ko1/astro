# const_defined? counts an autoload pending in an ancestor or Object (inherit) without
# loading it, and a scoped "A::B" resolves the inner A, which runs its autoload
# (optimist gates on Module.const_defined?("DidYouMean::JaroWinkler")).
require "tmpdir"
dir = Dir.mktmpdir
File.write(File.join(dir, "cd_al_foo.rb"), "module CdAlFoo; class Bar; end; end\n")
$LOAD_PATH.unshift(dir)
Object.autoload(:CdAlFoo, "cd_al_foo")
loaded = -> { $LOADED_FEATURES.count { |f| f.end_with?("cd_al_foo.rb") } }
p Module.const_defined?("CdAlFoo"), loaded.(), Module.const_defined?(:CdAlFoo), Module.const_defined?("CdAlFoo", false)
p Comparable.const_defined?(:CdAlFoo), Comparable.const_defined?(:CdAlFoo, false), loaded.()
p Module.const_defined?("CdAlFoo::Bar"), loaded.(), Object.const_defined?("CdAlFoo::Nope")
p Module.const_defined?("DidYouMean::JaroWinkler")
$VERBOSE = nil; Ractor.new {}; $VERBOSE = false; p Ractor.new { 1 }.value   # the experimental warning is spent silently
