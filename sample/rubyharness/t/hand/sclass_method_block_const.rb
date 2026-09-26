# A constant read in a block inside a method defined in `class << x` resolves
# against that singleton cref, as in the method body (rubocop's
# OptionsValidator: a private_constant of class << self read in names.each).
module Outer
  class V
    class << self
      K = %w[a b].freeze
      private_constant :K
      def m1 = K
      def m2(xs) = xs.select { |x| K.include?(x) }
      def m3(xs) = (xs.each { |x| next if K.include?(x); return x }; nil)
    end
  end
end
p Outer::V.m1, Outer::V.m2(%w[a c]), Outer::V.m3(%w[a z])
class W; class << self; J = 1; def j = [1].map { J }; end; end
p W.j
module M
  OUT = :out
  class C
    IN = :in
    class << self
      S = :s
      def f = [1].map { [S, IN, OUT] }.first
      def g = [1].flat_map { [2].map { S } }
    end
  end
end
p M::C.f, M::C.g
o = Object.new
class << o
  Q = :q
  def h = [1].map { Q }
end
p o.h
class D; class << self; def k = [1].map { self }; end; end
p D.k == [D]
