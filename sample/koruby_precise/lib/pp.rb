# 'pp' — Kernel#pp already lives in the prelude; this adds the PP entry point.
# koruby has no pretty-printer engine, so #pretty_inspect is #inspect and the
# output is single-line (valid, just not wrapped).
module PP
  def self.pp(obj, out = $stdout, width = 79)
    out.write(obj.pretty_inspect)
    out
  end

  def self.singleline_pp(obj, out = $stdout)
    out.write(obj.inspect)
    out
  end
end
