# send / public_send / __send__ are listed, so a blank slate can undef them
p [Object.instance_methods.include?(:send),
   Object.instance_methods.include?(:public_send),
   Object.instance_methods.include?(:__send__),
   Object.instance_methods(false),
   Kernel.instance_methods(false).include?(:send),
   BasicObject.instance_methods(false).sort]
p Comparable.instance_methods(false).sort

class Blk
  instance_methods.each { |m| undef_method m unless m =~ /^__|object_id/ }
  def method_missing(meth, *args) = "MM:#{meth}#{args.inspect}"
end
b = Blk.new
p [b.send, b.foo, b.class, b.__send__(:send), b.public_send, b.object_id.is_a?(Integer)]

# no flattened "A::B" names in Object.constants
p Object.constants.select { |c| c.to_s.include?("::") }
p [File::Stat, Object.const_get("File::Stat"), File.const_get(:Stat)]
p Object.constants.map { |c| Object.const_get(c) }.size > 50

# reduce/inject: a lone arg with a block is the SEED, not an operator
h = {a: 1, b: 2}
p h.inject("z") { |s, (k, v)| s + k.to_s }
p h.inject("z") { |s, x| s + x[0].to_s }
p (h.inject("z") do |s, (k, v)| s + k.to_s end)
p h.inject(0) { |s, (k, v)| s + v }
p h.inject { |s, x| s }
p h.reduce(:merge) rescue p [$!.class, $!.message[0,30]]
p h.inject({}, :merge) rescue p [$!.class, $!.message[0,30]]
p [1, 2].inject("z") { |s, x| s + x.to_s }
p [[:a, 1]].inject("z") { |s, (k, v)| s + k.to_s }
p (1..3).inject("z") { |s, x| s + x.to_s }
p ({a: 1}.inject { |s, x| s })
