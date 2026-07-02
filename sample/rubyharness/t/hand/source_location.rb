# Method / UnboundMethod / Proc #source_location → [file, line] of the definition
# (nil for C-defined). Single file ⇒ exact line numbers. vs ruby.
def top_fn
  42
end
p method(:top_fn).source_location
class Klass
  def imeth; end
end
p Klass.instance_method(:imeth).source_location
p Klass.new.method(:imeth).source_location
pr = proc { |x| x }
p pr.source_location
la = ->(y) { y }
p la.source_location
p method(:puts).source_location            # builtin → nil
p [].method(:size).source_location         # builtin → nil
