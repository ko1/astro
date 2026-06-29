p [1, 2, 3].include?(2)
p [1, 2, 3].include?(5)
p ["a", "b"].include?("a")
class O; def ==(o); o == 99; end; end
p [1, O.new, 3].include?(99)
p [1, 2, 3].include?(99)
p [[1, 2], [3, 4]].include?([1, 2])
