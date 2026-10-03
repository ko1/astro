# A::B stays correct once some constant somewhere is deprecated (the read is cached unless it is the deprecated one)
module M; X = 1; Y = 2; deprecate_constant :Y; end
module N; Z = 3; end
Warning[:deprecated] = false
r = []
3.times { r << M::X << N::Z << M::Y }
p r
module N; deprecate_constant :Z; end            # deprecated after the read was cached
p N::Z
N.send(:remove_const, :Z); N.const_set(:Z, 4)   # redefined
p N::Z
class C; W = :c; end; class D < C; end
p D::W
p 3.times.map { D::W }
