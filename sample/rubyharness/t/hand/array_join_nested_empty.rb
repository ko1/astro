# a nested array is one element of the join, even when empty
p [[], [], ["a","b"], [], []] * "\n", [[], []].join("-"), [[], [[]], "x"].join(","), [[1,[2,[]]], []].join("|")
