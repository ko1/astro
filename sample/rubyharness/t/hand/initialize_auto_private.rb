# CRuby makes `initialize`, `initialize_copy`, `initialize_clone`,
# `initialize_dup` and `respond_to_missing?` private AT DEFINITION TIME, so an
# explicit `public def ...` afterwards still wins.  koruby only pretended: the
# method entry stayed public and just the *listings* (#private_instance_methods)
# special-cased those names, so #method_defined? / #private_method_defined? /
# #respond_to? all disagreed with the listing.
def t(l)
  p [l, yield]
rescue Exception => e
  p [l, e.class, e.message.to_s[0, 50]]
end

class R
  def pub = 1
  private def priv = 2
  def initialize; end
  def initialize_copy(o); end
  def respond_to_missing?(n, p) = false
  public def initialize_dup(o); end
end
module MM; def initialize; end; end

r = R.new
t(:listing)      { R.private_instance_methods(false).sort }
t(:public_list)  { R.public_instance_methods(false).sort }
t(:defined)      { [R.method_defined?(:initialize), R.private_method_defined?(:initialize)] }
t(:defined_copy) { [R.method_defined?(:initialize_copy), R.private_method_defined?(:initialize_copy)] }
t(:defined_rtm)  { [R.method_defined?(:respond_to_missing?), R.private_method_defined?(:respond_to_missing?)] }
t(:explicit_pub) { [R.public_method_defined?(:initialize_dup), R.private_method_defined?(:initialize_dup)] }
t(:respond)      { [r.respond_to?(:initialize), r.respond_to?(:initialize, true)] }
t(:respond_copy) { [r.respond_to?(:initialize_copy), r.respond_to?(:initialize_copy, true)] }
t(:obj_respond)  { [Object.new.respond_to?(:initialize), Object.new.respond_to?(:initialize, true)] }
t(:call_direct)  { r.initialize }
t(:send_ok)      { r.send(:initialize) }
t(:module_priv)  { MM.private_instance_methods(false) }
t(:dm)           { c = Class.new { define_method(:initialize_copy) { |o| } }; c.private_method_defined?(:initialize_copy) }
t(:alias_priv)   { Class.new { def foo; end; alias initialize foo }.private_instance_methods(false) }
t(:normal_priv)  { [R.method_defined?(:priv), R.private_method_defined?(:priv), r.respond_to?(:priv)] }
t(:pub_stays)    { [R.method_defined?(:pub), r.respond_to?(:pub)] }
t(:new_works)    { R.new.class }
t(:dup_works)    { r.dup.class }
t(:struct_priv)  { Struct.new(:a).private_instance_methods(false).sort }
# `def self.initialize` is a SINGLETON method and stays public in CRuby, so the
# auto-private rule must not reach singleton classes.
t(:singleton)    { c = Class.new { def self.initialize; end }
                   [c.singleton_class.private_instance_methods(false), c.respond_to?(:initialize)] }
t(:attr_named)   { Class.new { attr_accessor :initialize }.private_instance_methods(false).sort }
t(:sing_attr)    { c = Class.new { class << self; attr_accessor :initialize; end }; c.respond_to?(:initialize) }
t(:module_dm)    { Module.new { define_method(:initialize_dup) { |o| } }.private_method_defined?(:initialize_dup) }
