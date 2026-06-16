# Basic-operator redefinition must be honored (fairness / drop-in): redefining
# Integer#+ etc. on a builtin numeric class deopts the node fastpaths so the
# user method actually runs — matching CRuby's basic-op-redefined behavior.

# baseline: fastpaths intact before any redef
p 2 + 3          # 5
p 10 - 4         # 6
p 2 * 8          # 16
p 7 < 9          # true

# redefine Integer#+ → literal arithmetic must use it
class Integer
  def +(o)
    42
  end
end
p 2 + 3          # 42
p 100 + 1        # 42

# other (non-redefined) Integer ops still work normally
p 10 - 4         # 6
p 2 * 8          # 16
p 7 < 9          # true

# Float arithmetic unaffected by Integer#+ redef
p 1.5 * 2.0      # 3.0
p 1.0 + 2.0      # 3.0  (Float#+ not redefined)

# redefine a comparison too
class Integer
  def <(o)
    :less_stub
  end
end
p 3 < 5          # :less_stub
p 9 > 2          # true  (> not redefined)

# == / != redefinition
class Integer
  def ==(o)
    :eq_stub
  end
end
p 1 == 1         # :eq_stub
p 1 != 1         # true  (!= default still negates value-eq; not redefined)

# unary minus redefinition
class Integer
  def -@
    :neg_stub
  end
end
p(-5)            # :neg_stub
p 1.0 + 2.0      # 3.0  (Float ops still native)
p(-2.0)          # -2.0 (Float#-@ not redefined)
