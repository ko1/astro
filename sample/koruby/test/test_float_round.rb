# Float#round — round to nearest (not truncate-toward-zero).
# Float#floor / Float#ceil / Float#truncate / Float#to_i are also tested.

require_relative "test_helper"

def test_round_positive_half
  assert_equal 2, 1.5.round
  assert_equal 2, 1.7.round
  assert_equal 1, 1.4.round
  assert_equal 1, 1.0.round
  assert_equal 0, 0.4.round
end

def test_round_negative_half
  assert_equal -2, (-1.5).round   # round half away from zero
  assert_equal -2, (-1.7).round
  assert_equal -1, (-1.4).round
  assert_equal -1, (-1.0).round
  assert_equal 0, (-0.4).round
end

def test_floor_simple
  assert_equal 1, 1.5.floor
  assert_equal 1, 1.7.floor
  assert_equal 1, 1.0.floor
  assert_equal -2, (-1.5).floor
  assert_equal -2, (-1.7).floor
  assert_equal -1, (-0.4).floor
end

def test_ceil_simple
  assert_equal 2, 1.5.ceil
  assert_equal 2, 1.0001.ceil
  assert_equal 1, 1.0.ceil
  assert_equal -1, (-1.5).ceil
  assert_equal 0, (-0.4).ceil
end

def test_to_i_truncates
  assert_equal 1, 1.5.to_i
  assert_equal 1, 1.7.to_i
  assert_equal -1, (-1.5).to_i  # truncate toward zero
  assert_equal -1, (-1.9).to_i
end

def test_truncate
  assert_equal 1, 1.5.truncate
  assert_equal -1, (-1.5).truncate
end

TESTS = [
  :test_round_positive_half, :test_round_negative_half,
  :test_floor_simple, :test_ceil_simple,
  :test_to_i_truncates, :test_truncate,
]

TESTS.each do |t|
  $current = t
  send(t)
end

if $fail == 0
  puts "OK FloatRound (#{$pass})"
else
  puts "FAIL FloatRound: #{$fail}/#{$pass + $fail}"
end
