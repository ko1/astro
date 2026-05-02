require_relative "../../test_helper"

# Method#parameters / Proc#parameters return [[kind, name?], ...].
# koruby doesn't preserve param names on the method record so it
# emits anonymous form [[kind]] which CRuby accepts for unnamed
# params.

class ParamHost
  def zero_arg; end
  def one_arg(a); end
  def two_args(a, b); end
  def with_rest(*a); end
  def with_block(&b); end
  def opt_only(a = 1); end
end

def test_zero_arg
  assert_equal [], ParamHost.new.method(:zero_arg).parameters
end

def test_one_arg
  result = ParamHost.new.method(:one_arg).parameters
  assert_equal 1, result.length
  assert_equal :req, result[0][0]
end

def test_two_args
  result = ParamHost.new.method(:two_args).parameters
  assert_equal 2, result.length
  assert_equal :req, result[0][0]
  assert_equal :req, result[1][0]
end

def test_with_rest
  result = ParamHost.new.method(:with_rest).parameters
  assert(result.any? { |p| p[0] == :rest })
end

def test_with_block
  result = ParamHost.new.method(:with_block).parameters
  assert(result.any? { |p| p[0] == :block })
end

def test_opt_only
  result = ParamHost.new.method(:opt_only).parameters
  # Either [:opt] or [:opt, :a] — accept either shape.
  assert_equal 1, result.length
  assert_equal :opt, result[0][0]
end

# ---------- Proc#parameters ----------

def test_lambda_two_arg_params
  l = ->(a, b) { }
  assert_equal [[:req], [:req]], l.parameters
end

def test_proc_one_arg_params
  p = proc { |x| }
  # Proc is lenient — emits :opt instead of :req.
  result = p.parameters
  assert_equal 1, result.length
  assert(result[0][0] == :opt || result[0][0] == :req)
end

def test_proc_with_rest
  p = proc { |*x| }
  result = p.parameters
  assert(result.any? { |pp| pp[0] == :rest })
end

# ---------- cfunc method ----------

def test_cfunc_method
  # `5.method(:abs)` → cfunc with argc=0
  result = 5.method(:abs).parameters
  assert_equal 0, result.length
end

def test_cfunc_varargs
  # Array#push is varargs (argc=-1)
  result = [].method(:push).parameters
  assert(result.any? { |p| p[0] == :rest })
end

TESTS = [
  :test_zero_arg, :test_one_arg, :test_two_args,
  :test_with_rest, :test_with_block, :test_opt_only,
  :test_lambda_two_arg_params, :test_proc_one_arg_params, :test_proc_with_rest,
  :test_cfunc_method, :test_cfunc_varargs,
]
TESTS.each { |t| run_test(t) }
report "MethodParameters"
