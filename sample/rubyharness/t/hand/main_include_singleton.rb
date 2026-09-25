# main's `include` is its own (singleton, private) method: an #include that a
# module mixed into Object defines must not shadow it (RSpec::Matchers has one).
module M; def include(*) = (puts "M#include"); end
include M
module B1; end; if true then include B1 end; p Object.include?(B1)
module B2; end; begin; include B2; end; p Object.include?(B2)
module B3; end; [1].each { include B3 }; p Object.include?(B3)
module B4; end; 1.then { include B4 }; p Object.include?(B4)
module B5; end; pr = proc { include B5 }; pr.call; p Object.include?(B5)
module B6; end; def self.x = include(B6); x; p Object.include?(B6)
main = self
module B7; end; main.instance_eval { include B7 }; p Object.include?(B7)
module B8; end; main.send(:include, B8); p Object.include?(B8)
p main.singleton_class.private_method_defined?(:include, false)
