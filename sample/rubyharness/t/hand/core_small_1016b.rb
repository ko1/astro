# MatchData#string identity, defined?(Kernel) below BasicObject, an undef'd singleton_method_added,
# main#include in load(file, true), @@x through eval(str, binding) inside instance_exec (2026-10-16)
def t(tag)
  r = yield
  puts "#{tag}: #{r.inspect}"
rescue Exception => e
  puts "#{tag}: #{e.class}: #{e.message}"
end
t(:md_string_same) { m = /(.)(.)(\d+)(\d)/.match("THX1138."); [m.string.equal?(m.string), m.string.frozen?] }
class BOS < BasicObject; def self.kd = defined?(Kernel); def self.bd = defined?(BasicObject); end
t(:bo_defined) { [BOS.kd, BOS.bd] }
t(:sma_undef) {
  begin; Class.new { class << self; undef_method :singleton_method_added; end; def self.foo; end }; rescue NoMethodError => e; e.name; end
}
module WrapIncM; end
t(:wrapped_include) {
  path = File.join(__dir__, "wrapped_include_tmp_#{$$}.rb")
  File.write(path, "include WrapIncM\n")
  begin
    load(path, true)
    Object.ancestors.include?(WrapIncM)
  ensure
    File.unlink(path)
  end
}
class BB; @@super_secret = "password"; def get_binding; a = 1; binding; end; end
class EnvX; end
t(:cvar_binding_in_exec) { b1 = BB.new.get_binding; EnvX.new.instance_exec { eval("@@super_secret", b1) } }
