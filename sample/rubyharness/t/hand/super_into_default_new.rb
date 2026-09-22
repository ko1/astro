# `super` from a user `new` reaches the DEFAULT allocator, and must skip every
# `new` at or before the defining class in the receiver's singleton MRO — not
# just that class.  An `extend`ed module's `new` sits EARLIER in that MRO and is
# a sibling of the defining class, so re-dispatching to it recurses forever
# (concurrent-ruby's Synchronization::SafeInitialization over its Object.new).
module SafeInit
  def new(*args, &blk)
    $log << :safe
    super(*args, &blk)
  end
end

class Base
  class << self
    def new(*args, &blk)
      $log << :base
      super(*args, &blk)
    end
  end
  attr_reader :a
  def initialize(a = nil) = @a = a
end

class Sub < Base
  extend SafeInit
end

p Sub.singleton_class.ancestors.first(3)

$log = []
o = Sub.new(7)
p [$log, o.class, o.a]

# the plain `def self.new` form, and one with no module in between
class Plain < Base; end
$log = []
p [Plain.new(1).a, $log]

# a module extended into a class that has no `new` of its own
class Bare
  extend SafeInit
  def initialize(*a) = @a = a
  attr_reader :a
end
$log = []
p [Bare.new(1, 2).a, $log]

# two extended modules stack in MRO order
module SafeInit2
  def new(*args, &blk)
    $log << :safe2
    super(*args, &blk)
  end
end
class Two < Base
  extend SafeInit
  extend SafeInit2
end
$log = []
p [Two.new(3).a, $log]
