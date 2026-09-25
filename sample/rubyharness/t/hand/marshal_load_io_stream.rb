# `Marshal.load(io)` must consume ONLY the bytes of the object it decodes: a
# stream can carry several dumps back to back, and the reader has to stop at the
# end of each one.  koruby did `data = io.read` (slurp), so a pipe that stays
# open blocked forever — the `parallel` gem hands jobs to forked workers exactly
# that way, which is how rubocop's HTML/Markdown formatter specs hung.
def t(l)
  p [l, yield]
rescue Exception => e
  p [l, e.class, e.message.to_s[0, 50]]
end

require "stringio"

t(:two_from_stringio) do
  io = StringIO.new(Marshal.dump([1, 2]) + Marshal.dump({ a: 1 }), "rb")
  [Marshal.load(io), Marshal.load(io)]
end

t(:pos_after_load) do
  d = Marshal.dump(:x)
  io = StringIO.new(d + "TAIL", "rb")
  v = Marshal.load(io)
  [v, io.read]
end

t(:three_over_pipe) do
  r, w = IO.pipe
  w.binmode
  [[:a], "s", 42].each { |o| w.write(Marshal.dump(o)) }
  r.binmode
  got = 3.times.map { Marshal.load(r) }
  w.close; r.close
  got
end

t(:pipe_stays_open) do
  r, w = IO.pipe
  w.binmode; r.binmode
  w.write(Marshal.dump("first"))
  v = Marshal.load(r)          # must return without waiting for EOF
  w.write(Marshal.dump("second"))
  v2 = Marshal.load(r)
  w.close; r.close
  [v, v2]
end

t(:eof_is_eoferror) do
  r, w = IO.pipe
  w.close
  Marshal.load(r)
ensure
  r.close
end

t(:truncated) do
  io = StringIO.new(Marshal.dump([1, 2, 3]).byteslice(0, 4), "rb")
  Marshal.load(io)
end

t(:string_arg_still_ok) { Marshal.load(Marshal.dump({ "k" => [1, :s, nil] })) }
t(:empty_string)        { Marshal.load("") }
t(:nested_big)          { Marshal.load(Marshal.dump((1..200).to_a)).size }
