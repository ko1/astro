p Enumerator.product([1,2],[3,4]).to_a
p Enumerator.product([1,2],[3,4],[5,6]).to_a.size
p Enumerator.product([1]).to_a
out = []; Enumerator.product([1,2],[:a,:b]) { |c| out << c }; p out
