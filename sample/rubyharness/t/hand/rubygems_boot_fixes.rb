# Found when koruby started loading rubygems at boot (like CRuby).

# IO.select sees bytes already read ahead into the stream's buffer (expect relies on it)
r, w = IO.pipe
w.print "abc"
p IO.select([r], nil, nil, 1).map(&:size)
p r.getc, r.getc
p IO.select([r], nil, nil, 0.2).map(&:size), r.getc
p IO.select([r], nil, nil, 0.1)
require "expect"
w.print "prompt> hi"
p r.expect("prompt>", 1), r.expect("nothing", 0.2)

# a local Time takes TZ as it is when the Time is made, even after an earlier local time
Time.now
old = ENV["TZ"]
ENV["TZ"] = "America/New_York"
t = Time.at(0)
p t.utc_offset, t.zone, Time.at(0).to_a.last
u = Time.new(2005, 2, 27, 22, 50, 0, -3600)
u.localtime
ENV["TZ"] = old
p u.utc_offset

# module_function finds a method through Object (Kernel#require is Ruby code under rubygems)
m = Module.new { module_function :require }
p m.respond_to?(:require)

# Zlib::GzipReader.wrap passes keyword arguments on
require "zlib"
require "stringio"
gz = StringIO.new(Zlib.gzip("héllo"))
Zlib::GzipReader.wrap(gz, external_encoding: Encoding::UTF_8) { |z| s = z.read; p s, s.encoding }
