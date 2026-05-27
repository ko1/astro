require_relative "../../test_helper"

def test_pack_C_star
  assert_equal "ABC", [65, 66, 67].pack("C*")
end

def test_pack_n_star
  s = [0x1234, 0x5678].pack("n*")
  assert_equal 4, s.size
  assert_equal [0x1234, 0x5678], s.unpack("n*")
end

def test_pack_v_star
  s = [0x1234, 0x5678].pack("v*")
  assert_equal [0x1234, 0x5678], s.unpack("v*")
end

def test_pack_N_V
  assert_equal [0x12345678], [0x12345678].pack("N").unpack("N")
  assert_equal [0x12345678], [0x12345678].pack("V").unpack("V")
end

def test_pack_q_Q
  # Use a 60-bit value to fit FIXNUM (62-bit signed)
  v = 0x012345678AABBCCD
  assert_equal [v], [v].pack("Q").unpack("Q")
end

def test_pack_a_A
  assert_equal "abc\0\0", ["abc"].pack("a5")
  assert_equal "abc  ", ["abc"].pack("A5")
end

def test_pack_Z
  assert_equal "abc\0", ["abc"].pack("Z*")
  assert_equal ["abc"], "abc\0\0".unpack("Z*")
end

def test_pack_H
  # H = high nibble first
  assert_equal "\x12\x34", ["1234"].pack("H*")
  assert_equal ["1234"], "\x12\x34".unpack("H4")
end

def test_pack_h
  # h = low nibble first
  assert_equal "\x21\x43", ["1234"].pack("h*")
  assert_equal ["1234"], "\x21\x43".unpack("h4")
end

def test_pack_x
  # x = pad null
  assert_equal "A\x00\x00B", [65, 66].pack("Cx2C")
end

def test_pack_double
  d = [1.5].pack("d")
  assert_equal 8, d.size
  assert_equal 1.5, d.unpack("d")[0]
end

def test_pack_float
  f = [1.5].pack("f")
  assert_equal 4, f.size
  assert_equal 1.5, f.unpack("f")[0]
end

TESTS = [
  :test_pack_C_star, :test_pack_n_star, :test_pack_v_star,
  :test_pack_N_V, :test_pack_q_Q, :test_pack_a_A, :test_pack_Z,
  :test_pack_H, :test_pack_h, :test_pack_x,
  :test_pack_double, :test_pack_float,
]
TESTS.each { |t| run_test(t) }
report "PackUnpack"
