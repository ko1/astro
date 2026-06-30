# Integer.sqrt(neg) -> Math::DomainError; Math.frexp coerces Numeric; Array#replace
# frozen check. vs ruby. (DomainError name is namespaced in CRuby; use is_a?.)
begin; Integer.sqrt(-1); rescue => e; p e.is_a?(Math::DomainError); end
p Integer.sqrt(16); p Integer.sqrt(8); p Integer.sqrt(10**40)
class NF < Numeric; def to_f; 8.0; end; end
p Math.frexp(NF.new)
p Math.frexp(8.0)
begin; Math.frexp(Object.new); rescue => e; p e.class; end
begin; [1, 2].freeze.replace([3]); rescue => e; p e.class; end
p [1, 2, 3].replace([4, 5])
