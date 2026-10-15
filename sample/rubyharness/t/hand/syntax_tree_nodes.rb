# #syntax_tree (Ruby 4.1) of procs, methods and backtrace positions: the Prism node class and its first line.
def show(tag, node)
  puts "#{tag}: #{node.class} #{node.start_line} #{node.respond_to?(:name) ? node.name.inspect : ''}"
end
def rb(&b) = b
show(:proc, proc { 42 }.syntax_tree)
show(:lambda_call, lambda { 42 }.syntax_tree)
show(:passed_block, rb { 42 }.syntax_tree)
show(:rescue_block, proc do; rescue; end.syntax_tree)
show(:stabby, -> { 42 }.syntax_tree)
show(:stabby_it, -> { it }.syntax_tree)
show(:proc_numbered, proc { _1 }.syntax_tree)
def m; end
show(:def, method(:m).syntax_tree)
show(:unbound, method(:m).unbind.syntax_tree)
define_singleton_method(:dm) do
end
show(:define_method, method(:dm).syntax_tree)
show(:location, -> { caller_locations(0, 1)[0] }.call.syntax_tree)
begin
  [1].each { |x| nil.foo(x) }
rescue NoMethodError => e
  show(:raise_location, e.backtrace_locations[0].syntax_tree)
end
