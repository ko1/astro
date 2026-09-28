require 'tmpdir'
Dir.mktmpdir do |dir|
  f = File.open(File.join(dir, "x"), "w+")
  f.write("hello")
  p f.size, File.size(f.path)            # File#size flushes first
  c = f.clone(freeze: true)
  p c.frozen?, c.fileno != f.fileno, f.clone.frozen?
  p((f.clone(freeze: 1) rescue $!.class), (f.clone(1) rescue $!.class))
  f.close; c.close
end

# finalizers are copied BEFORE #initialize_dup, which may drop them
class K
  def initialize_dup(o)
    ObjectSpace.undefine_finalizer(self)
    super
  end
end
k = K.new
ObjectSpace.define_finalizer(k, proc { $stdout.puts "fin" })
d = k.dup
k = d = nil
