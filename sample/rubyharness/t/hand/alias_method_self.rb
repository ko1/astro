# alias_method :m, :m (graphql define_accessor_method) must not wipe the entry it copies
class K; define_method(:x) { 1 }; alias_method :x, :x; def y = 2; alias_method :y, :y; attr_reader :z; alias_method :z, :z; def initialize = @z = 3; alias q y; alias y y; end
p K.new.x, K.new.y, K.new.z, K.new.q
