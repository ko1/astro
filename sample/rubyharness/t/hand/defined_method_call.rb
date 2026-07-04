# defined?(recv.meth) evaluates the receiver and reports "method" only when it
# actually responds publicly (nil for private / undefined / receiver raising). vs ruby.
class C; def foo; end; private def bar; end; end
c = C.new
p defined?(c.foo)
p defined?(c.bar)
p defined?(c.nope)
p defined?("str".upcase)
p defined?("str".no_such)
p defined?([1, 2].size)
p defined?(nil.foo)
p defined?(1 + 2)
p defined?(self.object_id)
