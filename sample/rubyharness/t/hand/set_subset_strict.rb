# Set#subset?/superset?/proper_* and </<=/>/>= require a real Set; a non-Set
# arg raises ArgumentError. vs ruby.
require "set"
p Set[1, 2].subset?(Set[1, 2, 3])
p Set[1, 2, 3].superset?(Set[1, 2])
p Set[1, 2].proper_subset?(Set[1, 2, 3])
p Set[1, 2, 3].proper_superset?(Set[1, 2])
p Set[1, 2].subset?(Set[1, 2])
p Set[1, 2].proper_subset?(Set[1, 2])
p(Set[1] <= Set[1, 2])
p(Set[1, 2] >= Set[1])
p(Set[1] < Set[1, 2])
[[], 1, "x", Object.new].each { |x| begin; Set[1].subset?(x); rescue => e; print e.class, " "; end }; puts
[[], 1, "x"].each { |x| begin; Set[1].superset?(x); rescue => e; print e.class, " "; end }; puts
