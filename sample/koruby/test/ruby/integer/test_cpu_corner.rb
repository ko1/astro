require_relative "../../test_helper"

# Corner cases that an NES CPU emulator likely hits.

def test_signed_overflow_8bit
  # Two's complement 8-bit
  v = (255 & 0xFF)
  assert_equal 255, v
  v = (-1 & 0xFF)
  assert_equal 255, v
  v = (256 & 0xFF)
  assert_equal 0, v
end

def test_bit_test
  flags = 0b10101010
  assert_equal 0, flags[0]
  assert_equal 1, flags[1]
  assert_equal 0, flags[2]
  assert_equal 1, flags[3]
  assert_equal 1, flags[7]
  assert_equal 0, flags[8]
end

def test_shift_round_trip
  v = 0x1234
  hi = (v >> 8) & 0xFF
  lo = v & 0xFF
  assert_equal 0x12, hi
  assert_equal 0x34, lo
  assert_equal 0x1234, (hi << 8) | lo
end

def test_carry_check
  # 8-bit add with carry
  a = 0x80; b = 0x80
  result = (a + b) & 0xFF
  carry = ((a + b) >> 8) & 1
  assert_equal 0, result
  assert_equal 1, carry
end

def test_negative_check
  a = 0xFF
  signed = a >= 0x80 ? a - 0x100 : a
  assert_equal -1, signed
  a = 0x7F
  signed = a >= 0x80 ? a - 0x100 : a
  assert_equal 127, signed
end

def test_array_read_write
  # Simulate memory access pattern from CPU
  ram = [0] * 8
  ram[0] = 0x42
  ram[7] = 0xff
  assert_equal 0x42, ram[0]
  assert_equal 0xff, ram[7]
  assert_equal 0, ram[3]
end

def test_array_method_assign
  ram = [0] * 16
  # @ram.method(:[]=).call(2, 99) — like CPU does for poke_ram
  setter = ram.method(:[]=)
  setter.call(2, 99)
  setter[5, 88]    # via []
  assert_equal 99, ram[2]
  assert_equal 88, ram[5]
end

def test_array_aset_via_send
  ram = [0] * 8
  ram.send(:[]=, 1, 7)
  assert_equal 7, ram[1]
end

def test_array_replace_slice
  # ROM does @prg_ref[0x8000, 0x4000] = bank
  buf = [0] * 16
  bank = [10, 20, 30, 40]
  buf[4, 4] = bank
  assert_equal [0, 0, 0, 0, 10, 20, 30, 40, 0, 0, 0, 0, 0, 0, 0, 0], buf
end

def test_byte_unpack
  # bytes from String, like File.binread.bytes
  s = "NES\x1a"
  arr = s.bytes
  assert_equal [78, 69, 83, 26], arr
end

def test_bytes_mask
  s = "ABCD"
  arr = s.bytes
  # mapper extraction: (blob[6] >> 4) | (blob[7] & 0xf0)
  mapper = (arr[2] >> 4) | (arr[3] & 0xf0)
  assert_equal ((67 >> 4) | (68 & 0xf0)), mapper
end

def test_block_destructure_two
  pairs = [[1, 'a'], [2, 'b'], [3, 'c']]
  out = pairs.map {|n, s| "#{s}#{n}" }
  assert_equal ["a1", "b2", "c3"], out
end

def test_complex_callsite
  def add3(a, b, c); a + b + c; end
  args = [10, 20, 30]
  result = add3(*args)
  assert_equal 60, result
end

def test_yield_with_state
  def loop3
    i = 0
    while i < 3
      yield i
      i += 1
    end
    :done
  end
  arr = []
  loop3 {|x| arr << x }
  assert_equal [0, 1, 2], arr
end

# This is the inner CPU loop pattern: send(*[symbol, args])
def test_send_splat
  class C2
    def cmp(a, b)
      a < b ? :lt : a > b ? :gt : :eq
    end
  end
  args = [:cmp, 1, 2]
  c = C2.new
  assert_equal :lt, c.send(*args)
end

TESTS = %i[
  test_signed_overflow_8bit test_bit_test test_shift_round_trip
  test_carry_check test_negative_check
  test_array_read_write test_array_method_assign test_array_aset_via_send
  test_array_replace_slice
  test_byte_unpack test_bytes_mask
  test_block_destructure_two test_complex_callsite test_yield_with_state
  test_send_splat
]
TESTS.each {|t| run_test(t) }
report("CpuCorner")
