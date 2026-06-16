# Drop-in compat: reopening / redefining builtin classes must work via normal
# method dispatch (CRuby semantics).  Operator fastpaths (+,-,*,<) are excluded
# on purpose — those bypass dispatch and are a separate concern.

# add a brand-new method to a builtin class
class Array
  def second
    self[1]
  end
end
p [10, 20, 30].second        # 20

class String
  def shout
    upcase + "!"
  end
end
p "hi".shout                 # "HI!"

class Integer
  def double
    self * 2
  end
end
p 21.double                 # 42

class Hash
  def pair_count
    size
  end
end
p({a: 1, b: 2}.pair_count)  # 2

# redefine an existing (non-operator) builtin method
class String
  def length
    999
  end
end
p "abc".length              # 999

# inheritance: a subclass of a builtin sees both inherited + reopened methods
class MyArr < Array
  def total
    sum
  end
end
m = MyArr.new
m.push(1); m.push(2); m.push(3)
p m.total                   # 6
p m.second                  # 2  (reopened Array#second is inherited)

# included module method still reachable after reopen
p [3, 1, 2].sort            # [1, 2, 3]   (Enumerable/Comparable intact)
p 5.clamp(1, 3)             # 3
