# a prelude (core-in-Ruby) frame shows at its caller's position, as CRuby's <internal:> frames do
Dir.chdir("/tmp") { puts caller(0).first(3).map { _1.sub(/^.*\//, "") } }
def f = [1].each { raise "x" }
begin; f; rescue => e; puts e.backtrace.map { _1.sub(/^.*\//, "") }; end
# ::Module.nesting is the lexical nesting, also through a binding
module NA; module NB; p ::Module.nesting, binding.eval("::Module.nesting"); end; end
# an exception raised inside prelude code: its frames sit at the caller's position
begin; Marshal.load("\x04\bc\tNope"); rescue ArgumentError => e; puts e.backtrace.last(1).map { _1.sub(/^.*\//, "") }; end
# a never-raised exception: full_message names only the call site; a broken message still prints
e = StandardError.new("A\xf3B"); p e.full_message(highlight: false).b.sub(/^.*\//, ""), e.detailed_message
