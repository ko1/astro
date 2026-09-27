# Exception#== converts another class's object with #exception first (a delegator compares equal).
require "delegate"
e = RuntimeError.new("boom")
duck = Object.new; duck.define_singleton_method(:exception) { |*| e }
p e == duck, e == SimpleDelegator.new(e), e == RuntimeError.new("boom"), e == RuntimeError.new("x"), e == ArgumentError.new("boom"), e == 1, e == nil
bad = Object.new; def bad.exception = raise("no"); p e == bad
