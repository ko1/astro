# Set.[] is Ruby (`def self.[](*ary) = new(ary)`), so a subclass's #initialize runs.
require "set"

class CSet < Set
  def initialize(*a)
    @seen = true
    super
  end
end
p CSet.new([1, 2]).instance_variables
b = CSet[1, 2, 2]
p b.instance_variables, b.class, b.to_a.sort, b.size
p CSet[].class, CSet[].to_a

class WrapSet < Set
  def initialize(*a)
    @log = []
    super
  end
  def log = @log
end
w = WrapSet[3, 1, 3]
p w.log, w.to_a.sort, w.class
# NOTE: koruby's Set.new fills the set in C, so a subclass's #add override is not
# dispatched from #initialize (CRuby's Set#initialize goes through #add).  Not
# covered here; see PROGRESS.md.

p Set[1, 2, 2].to_a.sort, Set[].size

# Hash / Array subclasses: CRuby does NOT call #initialize for []
class CH < Hash
  def initialize(*a) = (@seen = true; super)
end
p CH[1, 2].instance_variables, CH[1, 2].class, CH[1, 2]
class FakeHash
  def to_hash = { hello: "world" }
end
p CH[FakeHash.new], CH[FakeHash.new].class
class FakeAry
  def to_ary = [[1, 2], [3, 4]]
end
p CH[FakeAry.new], CH[FakeAry.new].class
p (CH[[[1, 2, 3]]] rescue [$!.class, $!.message])
class CA < Array
  def initialize(*a) = (@seen = true; super)
end
p CA[1, 2].instance_variables, CA[1, 2].class, CA[1, 2]
