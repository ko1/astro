# a prelude (core-in-Ruby) frame shows at its caller's position, as CRuby's <internal:> frames do
Dir.chdir("/tmp") { puts caller(0).grep_v(/__chdir/).first(3).map { _1.sub(/^.*\//, "") } }
def f = [1].each { raise "x" }
begin; f; rescue => e; puts e.backtrace.map { _1.sub(/^.*\//, "") }; end
# ::Module.nesting is the lexical nesting, also through a binding
module NA; module NB; p ::Module.nesting, binding.eval("::Module.nesting"); end; end
