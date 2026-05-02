require_relative "../../test_helper"

# File.open with block, File.write, IO#gets / each_line / write.
# Exercises the simple FILE* wrapper added in 2026-05-02.

PATH = "/tmp/k_filetest_#{$$ rescue 12345}.txt"

def setup_file(content)
  File.write(PATH, content)
end

def cleanup_file
  File.write(PATH, "") rescue nil
end

def test_file_write_returns_byte_count
  n = File.write(PATH, "hello")
  assert_equal 5, n
end

def test_file_read_round_trips
  setup_file("hello\nworld")
  assert_equal "hello\nworld", File.read(PATH)
end

def test_file_open_block_returns_block_value
  setup_file("a\nb\n")
  result = File.open(PATH) { |f| 42 }
  assert_equal 42, result
end

def test_file_each_line
  setup_file("foo\nbar\nbaz\n")
  lines = File.open(PATH) { |f|
    out = []
    f.each_line { |l| out << l }
    out
  }
  assert_equal ["foo\n", "bar\n", "baz\n"], lines
end

def test_file_gets
  setup_file("first\nsecond\n")
  pair = File.open(PATH) { |f| [f.gets, f.gets] }
  assert_equal ["first\n", "second\n"], pair
end

def test_file_gets_eof_returns_nil
  setup_file("only\n")
  result = File.open(PATH) { |f|
    f.gets
    f.gets
  }
  assert_equal nil, result
end

def test_file_open_for_write
  File.open(PATH, "w") { |f|
    f.puts "x"
    f.puts "y"
  }
  assert_equal "x\ny\n", File.read(PATH)
end

# ---------- const_missing hook ----------

class CMHost
  def self.const_missing(name)
    "missing-#{name}"
  end
end

def test_const_missing_qualified
  assert_equal "missing-Foo", CMHost::Foo
end

cleanup_file

TESTS = [
  :test_file_write_returns_byte_count,
  :test_file_read_round_trips,
  :test_file_open_block_returns_block_value,
  :test_file_each_line,
  :test_file_gets,
  :test_file_gets_eof_returns_nil,
  :test_file_open_for_write,
  :test_const_missing_qualified,
]
TESTS.each { |t| run_test(t) }
report "FileIO_ConstMissing"
