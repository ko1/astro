# Warning[:performance] on redefining Integer#+, super from a module method bound outside its
# hierarchy, RegexpError for a bad interpolated pattern (2026-10-16)
def t(tag)
  r = yield
  puts "#{tag}: #{r.inspect}"
rescue Exception => e
  puts "#{tag}: #{e.class}: #{e.message}"
end
module ModS; def foo_super = super; end
class ParentS; def foo_super = true; end
t(:bind_super) { um = ModS.instance_method(:foo_super); [um.bind(ParentS.new).call, um.bind_call(ParentS.new)] }
t(:dregx_bad) { s = "("; /#{s}/ }
t(:dregx_bad2) { s = ""; /(#{s}/ }
t(:dregx_ok) { t = "b"; /a#{t}c/ =~ "xabc" }
