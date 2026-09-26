# Value patterns are evaluated above the match node's slots; a const_missing
# fired from one (and anything else walking the frame) must see the method
# frame the pattern is in, at any nesting depth.
Object.extend(Module.new { def const_missing(n) = (p [n, caller_locations(1, 1).first.label]; Array) })
def m
  X
  [1] => Y
  case [1]; in Z(a) then end
  case 5; in W then end rescue nil
  [2] in V
  case [[1]]; in [[Integer => i]] then p i end
  case [[1]]; in [NN(b)] then p b end
  x = 3; y = 4
  case [[3, [4]]]; in [[^(x), [^(y + 0)]]] then p :pinned end
  case {a: {b: 7}}; in {a: {b: ^(x + 4)}} then p :hash_pin end
end
m
