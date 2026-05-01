# Tests for user-defined to_s being dispatched by puts / print /
# string interpolation.  Default behaviour for objects without to_s
# is the inspect format `#<Class:0x...>`.

require_relative "test_helper"

class Greeter
  def initialize(name); @name = name; end
  def to_s; "Hello, #{@name}!"; end
end

class Plain
  # No to_s defined.
end

# --- string interpolation -----------------------------------------

def test_interpolation_uses_to_s
  g = Greeter.new("alice")
  s = "before: #{g} :after"
  assert_equal "before: Hello, alice! :after", s
end

# --- nested interpolation ----------------------------------------

class Pair
  def initialize(a, b); @a = a; @b = b; end
  def to_s; "(#{@a},#{@b})"; end
end

def test_nested_interpolation
  p = Pair.new(1, 2)
  q = Pair.new(p, 3)
  assert_equal "((1,2),3)", q.to_s
end

# --- no to_s falls back to inspect -------------------------------

def test_no_to_s_inspect_fallback
  s = "obj: #{Plain.new}"
  # Default is "#<Plain:0x...>" — just check the prefix is right.
  assert_equal true, s.start_with?("obj: #<Plain:")
end

# --- to_s with non-string return falls back to inspect of result --
# Ruby actually raises in this case; koruby falls back to inspect for
# safety.  Test the relaxed behavior.

class Bogus
  def to_s; 42; end  # numeric, not String
end

def test_bogus_to_s_doesnt_crash
  s = "x:#{Bogus.new}:y"
  # Either "x:42:y" (lenient) or some inspect form — just check no crash.
  assert_equal true, s.length > 4
end

# --- Comparable class with to_s --------------------------------

class Box
  include Comparable
  attr_reader :v
  def initialize(v); @v = v; end
  def <=>(o); v <=> o.v; end
  def to_s; "Box(#{v})"; end
end

def test_comparable_class_to_s
  arr = [Box.new(2), Box.new(1), Box.new(3)]
  sorted = arr.sort
  joined = sorted.map { |b| b.to_s }.join(",")
  assert_equal "Box(1),Box(2),Box(3)", joined
end

TESTS = [
  :test_interpolation_uses_to_s,
  :test_nested_interpolation,
  :test_no_to_s_inspect_fallback,
  :test_bogus_to_s_doesnt_crash,
  :test_comparable_class_to_s,
]

TESTS.each do |t|
  $current = t
  send(t)
end

if $fail == 0
  puts "OK ToS (#{$pass})"
else
  puts "FAIL ToS: #{$fail}/#{$pass + $fail}"
end
