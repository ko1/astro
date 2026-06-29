p [[1, 2], [3, 4]].rassoc(4)
p [[1, 2], [3, 4]].rassoc(2)
p [[1, 2], [3, 4]].assoc(3)
class O; def ==(o); o == 99; end; end
p [[1, O.new], [3, 4]].rassoc(99)&.first
p [[1, 2], [3, 4]].rassoc(99)
p [["a", "b"]].assoc("a")
