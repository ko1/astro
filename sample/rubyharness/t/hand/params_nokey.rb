# A **nil parameter reports as [:nokey] in #parameters. vs ruby.
p proc { |**nil| }.parameters
p lambda { |x, **nil| }.parameters
def m(a, **nil); end
p method(:m).parameters
class C; define_method(:d) { |**nil| }; end
p C.instance_method(:d).parameters
