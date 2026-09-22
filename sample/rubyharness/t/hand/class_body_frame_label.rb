# A class/module/singleton body is a frame of its own: CRuby labels it
# <class:C> / <module:M> / <singleton class>, and code called from the body sees
# that label in caller_locations.
module Outer
  class C
    def self.probe = caller_locations[0]
    L1 = probe
    class << self
      L3 = C.probe
    end
  end
  module M
    def self.probe = caller_locations[0]
    L2 = probe
  end
end

[Outer::C::L1, Outer::M::L2, Outer::C.singleton_class::L3].each do |l|
  p [l.label, File.basename(l.path), l.absolute_path == File.expand_path(__FILE__)]
end

class D
  def self.method_added(name)
    (@seen ||= []) << caller_locations[0].label
  end
  def a; end
  def b; end
  def self.seen = @seen
end
p D.seen

# a block written in a class body names the body as its owner
class E
  B = proc { caller_locations(0)[0].label }
end
p E::B.call
