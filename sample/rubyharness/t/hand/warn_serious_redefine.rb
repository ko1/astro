# CRuby warns when a program redefines or undefines the identity primitives.
# (koruby's warnings carry no "file:line:" prefix yet, so this test only pins
#  which names warn and with what text — the prefix is a separate gap.)
$stderr.sync = true
def only_warnings
  r, w = IO.pipe
  old = $stderr
  $stderr = w
  yield
  $stderr = old
  w.close
  r.read.split("\n").map { |l| l.sub(/\A.*?warning: /, "warning: ") }.sort
ensure
  $stderr = old
end
p(only_warnings do
  %i[__send__ __id__ object_id send public_send initialize class].each do |nm|
    Class.new.send(:undef_method, nm) rescue nil
  end
end)
p(only_warnings do
  %i[__send__ __id__ object_id send initialize].each do |nm|
    Class.new.send(:define_method, nm) { 1 }
  end
end)
p(only_warnings do
  Class.new { def __send__(*) = 1 }
  Class.new { def __id__ = 1 }
  Class.new { def object_id = 1 }
  Class.new { def initialize = 1 }
  Class.new { def send(*) = 1 }
end)
o = Object.new
p(only_warnings { def o.__send__(*) = 1 })
