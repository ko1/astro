a = [1, 2].freeze
p a.dup.frozen?
p a.clone.frozen?
p a.clone(freeze: false).frozen?
p a.clone(freeze: true).frozen?
b = [3, 4]
p b.clone.frozen?
p b.clone(freeze: true).frozen?
s = "hi".freeze
p s.clone.frozen?
p s.clone(freeze: false).frozen?
class MyArr < Array; end
p MyArr[1].freeze.clone.class
