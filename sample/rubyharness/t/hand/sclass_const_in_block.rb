# Constants of a `class << self` body are visible from blocks written in it.
module M
  class << self
    Y = 3
    F = proc { Y }
    G = lambda { |k| [k, Y] }
    def m = [1].map { Y }
    H = [1].map { Y }
  end
end
p M.singleton_class::F.call, M.singleton_class::G.(1), M.m, M.singleton_class::H
class K; class << self; Z = :z; def z = Z; attr_accessor :q; end; end
p K.z, [K].map { K.singleton_class::Z }
