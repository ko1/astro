# `Module.constants` without arguments lists the top-level constants, but only
# for Module itself: Class and the singleton classes of classes inherit that
# method and must answer with their own (usually empty) constants.
module Mx; K = 1; end
class Cx; include Mx; J = 2; end
p Module.constants.include?(:Cx), Module.constants(false).include?(:Cx)
p Class.constants, BasicObject.singleton_class.constants, Cx.singleton_class.constants
p Class.new.singleton_class.constants, Module.new.singleton_class.constants
p Cx.constants.sort, Cx.constants(false), Mx.constants
s = (class << BasicObject.new; self; end); p s.constants
