# Reopening Integer#=== is seen by case/when: both the Fixnum binary split (8+ literal
# whens) and the plain when-chain send the real ===.
class Integer
  def ===(x)
    puts "called #{self}"
    self == 1
  end
end
p(case 8; when 1,2,3,4,5,6,7,8 then :hit; else :miss end)
p(case 3; when 1 then :one; when 2 then :two; else :else end)
