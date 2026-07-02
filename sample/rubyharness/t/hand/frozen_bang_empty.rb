# select!/reject!/sort_by! raise FrozenError upfront even on an empty frozen array. vs ruby.
def chk; yield; :no_raise; rescue FrozenError; :frozen; end
p chk { [].freeze.select! { |x| true } }
p chk { [].freeze.reject! { |x| true } }
p chk { [].freeze.sort_by! { |x| x } }
p chk { [].freeze.filter! { |x| true } }
p chk { [1, 2].freeze.select! { |x| x > 1 } }
p chk { [].freeze.map! { |x| x } }
p chk { [].freeze.sort! }
# non-frozen still works
a = [3, 1, 2]; a.sort_by! { |x| -x }; p a
b = [1, 2, 3, 4]; b.select! { |x| x.even? }; p b
c = [1, 2, 3, 4]; c.reject! { |x| x.even? }; p c
