module M; end
class P; extend M; end
class T < P; end
p T.is_a?(M), M === T, T.kind_of?(M), T.singleton_class.include?(M), T.singleton_class.ancestors.include?(M)
sc = T.new.singleton_class
p M === sc, sc.is_a?(M)
module MM; def mm = :mm; end
class PP; extend MM; end
class TT < PP; end
t = TT.new
sc = t.singleton_class
p sc.is_a?(MM), (sc.mm rescue :err), sc.ancestors.grep(MM).size, sc.singleton_class.superclass
p TT.singleton_class.superclass, sc.singleton_class.ancestors[1, 2]
module S
  class << self
    def append_features(m) = super
  end
end
S.singleton_class.send(:undef_method, :extend_object)
begin; 'foo'.extend S; p :no_raise; rescue NoMethodError => e; p e.message; end
module TX; end
class << TX; undef_method :extend_object; end
begin; Object.new.extend TX; p :no_raise; rescue NoMethodError => e; p e.message; end
