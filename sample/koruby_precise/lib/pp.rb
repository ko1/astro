# 'pp' — Kernel#pp already lives in the prelude; this adds the PP entry point.
# koruby has no pretty-printer engine, so #pretty_inspect is #inspect and the
# output is single-line (valid, just not wrapped).
module PP
  # `out` only has to answer #<< (CRuby appends to it), so a String works —
  # temple's grammar error message passes one.
  def self.pp(obj, out = $stdout, width = 79)
    out << obj.inspect << "\n"
    out
  end

  def self.singleline_pp(obj, out = $stdout)
    out << obj.inspect
    out
  end
end
