require_relative "../../test_helper"

# Symbol case-folding that was missing.

def test_symbol_capitalize
  assert_equal :Hello, :hello.capitalize
  assert_equal :Abc,   :abc.capitalize
  assert_equal :Abc,   :ABC.capitalize     # only first char up, rest down
end

def test_symbol_swapcase
  assert_equal :hELLO, :Hello.swapcase
end

TESTS = [
  :test_symbol_capitalize,
  :test_symbol_swapcase,
]
TESTS.each { |t| run_test(t) }
report "SymbolMore"
