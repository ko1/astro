require_relative "../../test_helper"

# IO.pipe / IO.select / STDIN basics.

def test_pipe_returns_pair
  pair = IO.pipe
  assert_equal 2, pair.size
  pair[1].close
  pair[0].close
end

def test_pipe_write_read
  r, w = IO.pipe
  w.write "hello"
  w.close
  assert_equal "hello", r.read
  r.close
end

def test_pipe_multiple_writes
  r, w = IO.pipe
  w.write "abc"
  w.write "def"
  w.close
  assert_equal "abcdef", r.read
  r.close
end

def test_select_immediate
  r, w = IO.pipe
  w.write "ready"
  w.close
  result = IO.select([r], nil, nil, 0.5)
  assert(result.is_a?(Array))
  assert_equal 3, result.size
  assert_equal [r], result[0]
  r.close
end

def test_select_timeout
  r, w = IO.pipe
  # Don't write anything; select with 50 ms timeout returns nil.
  result = IO.select([r], nil, nil, 0.05)
  assert_equal nil, result
  r.close
  w.close
end

def test_stdin_responds_to_gets
  assert STDIN.respond_to?(:gets)
  assert STDIN.respond_to?(:read)
end

def test_pipe_writer_each_line
  r, w = IO.pipe
  w.puts "a"
  w.puts "b"
  w.close
  lines = []
  r.each_line { |l| lines << l.chomp }
  assert_equal ["a", "b"], lines
  r.close
end

def test_popen_read
  out = IO.popen("echo hello") { |io| io.read }
  assert_equal "hello\n", out
end

def test_popen_no_block_returns_io
  io = IO.popen("printf 'a\\nb\\nc\\n'")
  lines = io.each_line.to_a.map(&:chomp)
  assert_equal ["a", "b", "c"], lines
end

def test_copy_stream_path_to_path
  File.write("/tmp/k_cs_src", "the quick brown fox")
  IO.copy_stream("/tmp/k_cs_src", "/tmp/k_cs_dst")
  assert_equal "the quick brown fox", File.read("/tmp/k_cs_dst")
  File.unlink("/tmp/k_cs_src")
  File.unlink("/tmp/k_cs_dst")
end

def test_copy_stream_with_length
  File.write("/tmp/k_cs_src2", "abcdefghij")
  IO.copy_stream("/tmp/k_cs_src2", "/tmp/k_cs_dst2", 4)
  assert_equal "abcd", File.read("/tmp/k_cs_dst2")
  File.unlink("/tmp/k_cs_src2")
  File.unlink("/tmp/k_cs_dst2")
end

def test_io_fileno_and_tty
  r, w = IO.pipe
  assert(r.fileno.is_a?(Integer))
  assert(r.fileno >= 0)
  # Pipes are not TTYs.
  assert_equal false, r.tty?
  r.close; w.close
end

TESTS = %i[
  test_pipe_returns_pair test_pipe_write_read test_pipe_multiple_writes
  test_select_immediate test_select_timeout
  test_stdin_responds_to_gets test_pipe_writer_each_line
  test_popen_read test_popen_no_block_returns_io
  test_copy_stream_path_to_path test_copy_stream_with_length
  test_io_fileno_and_tty
]
TESTS.each {|t| run_test(t) }
report "IOPipeSelect"
