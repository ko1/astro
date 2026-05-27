require_relative "../../test_helper"

class K
  def pub; "pub-#{secret}"; end
  def secret; "shh"; end
  private :secret
end

def test_private_blocks_explicit_receiver
  k = K.new
  raised = false
  begin
    k.secret
  rescue NoMethodError, RuntimeError => e
    raised = true
  end
  assert_equal true, raised
end

def test_private_callable_via_self
  # Implicit self (inside another method) should reach private.
  assert_equal "pub-shh", K.new.pub
end

class P
  def cmp(other); compare(other.value); end
  protected
  def value; @v ||= 42; end
  def compare(v); v == value ? :eq : :ne; end
end

# We can't easily test all the protected paths from the outside, but
# at least confirm the modifier doesn't break method dispatch.
def test_protected_internal_works
  assert_equal :eq, P.new.cmp(P.new)
end

def test_protected_external_raises
  raised = false
  begin
    P.new.value
  rescue NoMethodError, RuntimeError
    raised = true
  end
  assert_equal true, raised
end

TESTS = [
  :test_private_blocks_explicit_receiver,
  :test_private_callable_via_self,
  :test_protected_internal_works,
  :test_protected_external_raises,
]
TESTS.each { |t| run_test(t) }
report "Visibility"
