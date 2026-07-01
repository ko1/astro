# Module#constants lists constants defined directly in the module/class
# (owner-tagged in the const table via the lexical nesting baked at parse time).
# Globals ($) sharing the table are excluded. vs ruby.
module M
  class Base; end
  DEFAULT = 42
  class Sub < Base; end
  PI = 3.14
  module Inner
    X = 1
    Y = 2
    class E; end
  end
end
p M.constants.sort
p M::Inner.constants.sort
p M.constants.include?(:DEFAULT)
p M.constants.include?(:X)
p M::Inner.constants.include?(:X)
p M::Inner.constants.include?(:DEFAULT)
p M.constants.class
module Empty; end
p Empty.constants
# globals sharing the table don't leak in
module WithGlobal; $leaked = 1; REAL = 2; end
p WithGlobal.constants
