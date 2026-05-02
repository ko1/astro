# `alias` is a keyword, not a method call.  Redefining Module#alias_method
# must NOT affect what `alias` does.  Runs in its own process because
# the redef is permanent.

require_relative "../../test_helper"

# Monkey-patch alias_method to record calls without aliasing.
$called = false
class Module
  def alias_method(*args)
    $called = true
    nil  # do nothing — defer to no aliasing happening
  end
end

class C
  def hello; 42; end
  alias greet hello              # KEYWORD — should still alias regardless
end

class D
  def hello; 42; end
  alias_method :greet, :hello    # METHOD CALL — hits the redefined version
end

def test_keyword_alias_works
  $current = :test_keyword_alias_works
  c = C.new
  assert_equal 42, c.greet  # keyword alias must have created the alias
end

def test_method_alias_was_redefined
  $current = :test_method_alias_was_redefined
  assert_equal true, $called  # the redefined alias_method was invoked
  d = D.new
  raised = false
  begin
    d.greet  # never aliased — should raise NoMethodError
  rescue
    raised = true
  end
  assert_equal true, raised
end

test_keyword_alias_works
test_method_alias_was_redefined

if $fail == 0
  puts "OK AliasRedef (#{$pass})"
else
  puts "FAIL AliasRedef: #{$fail}/#{$pass + $fail}"
end
