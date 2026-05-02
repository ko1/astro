require_relative "../../test_helper"

# super tests inspired by CRuby's test_super.rb.

class S1Base
  def f(x); "base[#{x}]"; end
end
class S1Sub < S1Base
  def f(x); "sub:#{super}"; end
end

def test_super_with_arg
  assert_equal "sub:base[1]", S1Sub.new.f(1)
end

class S2Base
  def f(x, y); [x, y]; end
end
class S2Sub < S2Base
  def f(x, y); super(x * 2, y * 2); end
end

def test_super_with_explicit_args
  assert_equal [4, 6], S2Sub.new.f(2, 3)
end

class S3Base
  def f; "base-f"; end
end
class S3Sub < S3Base
  def f; "sub-f " + super; end
end

def test_super_no_args
  assert_equal "sub-f base-f", S3Sub.new.f
end

class S4Base
  def f(*args); args; end
end
class S4Sub < S4Base
  def f(*args); super; end
end

def test_super_splat_forwarding
  assert_equal [1, 2, 3], S4Sub.new.f(1, 2, 3)
end

# Three-level chain
class S5A
  def f; "A"; end
end
class S5B < S5A
  def f; super + "B"; end
end
class S5C < S5B
  def f; super + "C"; end
end

def test_super_three_level
  assert_equal "ABC", S5C.new.f
end

# super through module
module M6
  def f; "M[" + super + "]"; end
end
class S6Base
  def f; "B"; end
end
class S6Sub < S6Base
  prepend M6
  def f; "S[" + super + "]"; end
end

def test_super_through_prepended_module
  # M6.f wraps Sub.f wraps Base.f
  assert_equal "M[S[B]]", S6Sub.new.f
end

# super with block
class S7Base
  def f
    yield 10
  end
end
class S7Sub < S7Base
  def f(&blk); super(&blk); end
end

def test_super_with_block
  r = S7Sub.new.f { |x| x * 2 }
  assert_equal 20, r
end

TESTS = [
  :test_super_with_arg, :test_super_with_explicit_args,
  :test_super_no_args, :test_super_splat_forwarding,
  :test_super_three_level, :test_super_through_prepended_module,
  :test_super_with_block,
]
TESTS.each { |t| run_test(t) }
report "Super"