# Method#to_proc must report the METHOD's signature, not a symbol-proc's.
class K
  def call(skip_tests:) = skip_tests
  def two(a, b = 1, *r, k:, j: 2, **kw, &blk) = nil
  def none = 1
  attr_accessor :v
end
k = K.new
p k.method(:call).parameters
p k.method(:call).to_proc.parameters
p k.method(:two).to_proc.parameters
p k.method(:none).to_proc.parameters
p k.method(:v).to_proc.parameters
p k.method(:v=).to_proc.parameters
p k.method(:call).to_proc.lambda?
p [k.method(:call).to_proc.arity, k.method(:two).to_proc.arity]
def take(&b) = b
p take(&k.method(:call)).parameters
p :upcase.to_proc.parameters
p proc { |a, b: 1| }.parameters
p K.instance_method(:two).parameters
# a builtin method
p [].method(:push).to_proc.parameters
p 1.method(:+).to_proc.parameters
# the dry-cli shape: slice the args a callable accepts
def args_for(callable, args)
  ps = callable.parameters
  return args if ps.any? { |t, _| %i[keyrest req opt rest].include?(t) }
  args.slice(*ps.filter_map { |t, n| n if t == :key || t == :keyreq })
end
p args_for(k.method(:call), { skip_tests: true, quiet: false })
