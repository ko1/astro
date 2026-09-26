# class_eval(String) on an unnamed class: a def inside (and the body itself) see that class's constants.
c = Class.new { self::X = :anon }
p c.class_eval("def m = X; self").new.m
p c.class_eval("X"), c.class_eval("[1].map { X }")
c.class_eval("Y = 2; def y = [Y, X]")
p c.new.y, c.const_get(:Y), Object.const_defined?(:Y)
m = Module.new { self::Z = 3 }
m.module_eval("def self.z = Z"); p m.z
p c.class_eval("x = 5; x + 1"), c.class_eval("self") == c
o = Object.new; o.instance_eval("def w = :w"); p o.w
x = 10; p c.class_eval("x + 1")
module Outer; W = :outer; def self.mk = Class.new.tap { _1.const_set(:V, :v); _1.class_eval("def v = [V, W]") }; end
p Outer.mk.new.v
