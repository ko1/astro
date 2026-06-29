p [[1, 2], [3, 4]].assoc(3)
p [[1, 2], [3, 4]].rassoc(4)
class TA; def to_ary; [2, 3]; end; end
p [[1, 5], TA.new].assoc(2)
p [[1, 5], TA.new].rassoc(3)
p [[1, 2], "notarray", [3, 4]].assoc(3)
p [[1, 2]].assoc(9)
