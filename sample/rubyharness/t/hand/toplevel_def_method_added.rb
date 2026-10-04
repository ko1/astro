# a top-level def is an Object method, so Object.method_added fires for it
$added = []
def Object.method_added(m)
  $added << m if %i[tl_a tl_b wrapped].include?(m)
  super
end
def tl_a = 1
def tl_b(x) = x * 2
p $added
p tl_a, tl_b(3)
p Object.private_method_defined?(:tl_a)
# wrapping a top-level method from the hook (the harness-probe pattern)
def Object.method_added(m)
  if m == :wrapped && !$wrapping
    $wrapping = true
    alias_method :__orig_wrapped, :wrapped
    define_method(:wrapped) { |*a| [:w, __orig_wrapped(*a)] }
  end
  super
end
def wrapped(v) = v + 1
p wrapped(1)
