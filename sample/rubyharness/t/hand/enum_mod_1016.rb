# Array#zip with endless / Float-ended Ranges, block-less Lazy#chunk, Module#const_added reopened
# on Module (2026-10-16)
def t(tag)
  r = yield
  puts "#{tag}: #{r.inspect}"
rescue Exception => e
  puts "#{tag}: #{e.class}: #{e.message}"
end
t(:zip_inf) { s = 0..Float::INFINITY; [s.first(3).zip(s), [1, 2, 3].zip(0..1.5), [1, 2].zip(5..), [1, 2, 3].zip(0...2.0)] }
t(:lazy_zip_inf) { s = 0..Float::INFINITY; s.lazy.zip(s).first(3) }
t(:lazy_chunk_noblock) { c = [1, 2, 3].lazy.chunk; [c.class, c.each { |v| v.odd? }.force] }
$added = []
class Module
  alias_method :__orig_const_added_t, :const_added
  def const_added(name)
    $added << [self, name] if name.to_s.start_with?("ZZT")
    __orig_const_added_t(name)
  end
end
ZZT1 = 1
module ZZT2; end
class Module
  alias_method :const_added, :__orig_const_added_t
end
t(:const_added_module) { $added }
