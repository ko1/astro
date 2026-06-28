require "set"
p (Set[1,2,3,4,5,6].divide { |i| i % 3 }.map { |s| s.to_a.sort }.sort)
p (Set[1,2,3,4].divide { |a, b| (a - b).abs == 1 }.map { |s| s.to_a.sort }.sort)
