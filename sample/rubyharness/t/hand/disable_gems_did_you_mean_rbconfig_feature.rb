# --disable-gems leaves DidYouMean undefined (no autoload), as in CRuby; the built-in
# rbconfig feature is listed as <rubyarchdir>/rbconfig.rb in $LOADED_FEATURES.
require "rbconfig"
p $".grep(/rbconfig/).any? { |f| f.end_with?("/rbconfig.rb") && f.start_with?("/") }, require("rbconfig")
exe = RbConfig.ruby
p IO.popen([exe, "--disable-gems", "-e", "p [defined?(DidYouMean), Object.autoload?(:DidYouMean)]"], &:read)
p IO.popen([exe, "--disable-did_you_mean", "-e", "p [defined?(DidYouMean), Object.autoload?(:DidYouMean)]"], &:read)
