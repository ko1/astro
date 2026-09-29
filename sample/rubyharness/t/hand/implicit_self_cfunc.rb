# A builtin called bare on self (itself / frozen? / instance_variable_get ...) is invoked directly
# from the implicit-self call path; results, errors and backtraces match CRuby.  Run under both and diff.
p itself.to_s, frozen?, respond_to?(:puts, true), instance_variables, self.class
class A
  def initialize; @x = 1; end
  def go = [itself.class, frozen?, instance_variable_get(:@x), respond_to?(:go), is_a?(A), hash == hash, dup.class]
  def raise_it = instance_variable_get(1)
end
p A.new.go
begin; A.new.raise_it; rescue => e; p e.class, e.message, e.backtrace.first(2); end
def m = __method__
p m, [1, 2].map { itself }.size
p method(:itself).owner, singleton_methods.sort.first(2)
