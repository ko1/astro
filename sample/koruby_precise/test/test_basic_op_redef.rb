# Verify the basic-op redefinition guard fires for Array#<<.  Once we
# define a new Array#<<, the EVAL_node_lshift fast path (which would
# otherwise call korb_ary_push directly, bypassing the redefined method)
# must back off.

require_relative "test_helper"

class Array
  def <<(x)
    push(:before)
    push(x)
    push(:after)
    self
  end
end

def test_array_lshift_redef
  $current = :test_array_lshift_redef
  a = []
  a << 42
  assert_equal 3, a.length
  assert_equal :before, a[0]
  assert_equal 42, a[1]
  assert_equal :after, a[2]
end

test_array_lshift_redef

if $fail == 0
  puts "OK ArrayLshiftRedef (#{$pass})"
else
  puts "FAIL ArrayLshiftRedef: #{$fail}/#{$pass + $fail}"
end
