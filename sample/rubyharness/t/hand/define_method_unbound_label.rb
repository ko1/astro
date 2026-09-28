module Src; def orig = caller_locations(0, 1).first.label; end
class Tgt; define_method(:alias_name, Src.instance_method(:orig)); end
class Tgt2; define_method(:orig, Src.instance_method(:orig)); end
o = Object.new; o.singleton_class.define_method(:x, Src.instance_method(:orig))
p Tgt.new.alias_name, Tgt2.new.orig, o.x
class Tgt3; def m2 = caller_locations(0,1).first.label; alias_method :m3, :m2; end
p Tgt3.new.m3
