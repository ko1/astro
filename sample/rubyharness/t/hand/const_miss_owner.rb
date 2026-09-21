# A constant miss reported (and fired const_missing on) the FIRST module in the
# flat table with the same short name, not the cref the read actually sits in.
# With test-unit's Test::Unit::Color loaded, `NAMES_LIST` inside FFaker::Color
# raised "uninitialized constant Test::Unit::Color::NAMES_LIST" and ffaker's
# ModuleUtils#const_missing (which loads the data file) never ran.
module Test
  module Unit
    class Color
      def self.const_missing(cn) = ["WRONG", cn]
    end
  end
end
module MyLib
  module Color
    def self.get = NAMES_LIST
    def self.got(x) = x
    OWN = :own
    def self.own = OWN
  end
end
p (MyLib::Color.get rescue [$!.class, $!.message, $!.name])
p MyLib::Color.own

# ...and the hook must land on the right module
module Hooked
  module Color
    def self.const_missing(cn) = ["right", to_s, cn]
    def self.get = LATE
  end
end
p Hooked::Color.get

# same for a module nested deeper and for an instance method via `extend self`
module Deep
  module Inner
    module Color
      extend self
      def self.const_missing(cn) = ["deep", to_s, cn]
      def get = ALSO_LATE
    end
  end
end
p Deep::Inner::Color.get

# a class body read, a singleton-class body read and a `class << self` method
module Outer2
  class Color
    def self.const_missing(cn) = ["cls", to_s, cn]
    def self.a = FROM_CLASS_METHOD
  end
end
p Outer2::Color.a
# (a `class << self` method's cref is the SINGLETON class in CRuby, where the
#  hook is not defined; koruby still reports the class.  Separate gap.)

# the top-level miss still reports Object
p (NOT_ANYWHERE rescue [$!.class, $!.message, $!.name, $!.receiver])
# an explicitly scoped miss still names the receiver
p (MyLib::Color::STILL_GONE rescue [$!.class, $!.message])
