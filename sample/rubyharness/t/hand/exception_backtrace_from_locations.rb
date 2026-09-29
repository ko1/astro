# set_backtrace(Locations): #backtrace is the Strings (CRuby), #backtrace_locations the Locations;
# rake's Backtrace.collapse does `elem =~ pattern` on #backtrace.  A missing constant's NameError
# (const_missing) keeps its positions.
def pos = caller_locations(0)
e = RuntimeError.new("x")
e.set_backtrace(pos)
p e.backtrace.map(&:class).uniq, e.backtrace_locations.map(&:class).uniq, e.backtrace.equal?(e.backtrace)
p e.backtrace.grep(/exception_backtrace/).size > 0
begin
  NoSuchConstHere
rescue NameError => ne
  p ne.backtrace.map(&:class).uniq, ne.backtrace_locations&.map(&:class)&.uniq, ne.backtrace_locations[0].lineno
end
