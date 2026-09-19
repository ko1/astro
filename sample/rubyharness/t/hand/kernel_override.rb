# ユーザが Kernel のグローバル builtin を再定義できる (rubygems の
# `alias gem_original_require require` + `def require` が要求する)。
module Kernel
  alias orig_require require
  def require(p)
    $log << p
    orig_require(p)
  end
end
$log = []
require "set"
p $log
p defined?(Set) ? :set_ok : :set_missing
p method(:require).owner

module Kernel
  def puts(*a); $hook = a; end
end
puts "hi"
p $hook
