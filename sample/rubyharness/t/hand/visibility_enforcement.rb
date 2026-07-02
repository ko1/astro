# inherited private
class Base
  private
  def helper; :base_helper; end
  public
  def use; helper; end
end
class Sub < Base
  def use2; helper; end          # inherited private, implicit → ok
end
p Sub.new.use
p Sub.new.use2
begin; Sub.new.helper; rescue NoMethodError => e; p [:sub_helper, e.message.include?("private")]; end

# protected across instances of same class + subclass
class Acct
  def initialize(b); @b = b; end
  def >(o); balance > o.balance; end   # calls o.balance (protected) — ok, same class
  protected
  def balance; @b; end
end
p Acct.new(100) > Acct.new(50)
begin; Acct.new(100).balance; rescue NoMethodError => e; p [:balance, e.message.include?("protected")]; end

# module private
module Helpers
  private
  def secret_util; :util; end
end
class UsesMod
  include Helpers
  def run; secret_util; end        # module's private, implicit → ok
end
p UsesMod.new.run
begin; UsesMod.new.secret_util; rescue NoMethodError => e; p [:mod_secret, true]; end

# attr private
class Conf
  def initialize; @v = 42; end
  private
  attr_reader :v
  public
  def read; v; end                 # private attr reader, implicit → ok
end
p Conf.new.read
begin; Conf.new.v; rescue NoMethodError; p :conf_v_blocked; end

# send bypasses; public_send does not
class Sec
  private
  def hidden; :hidden; end
end
p Sec.new.send(:hidden)
p Sec.new.__send__(:hidden)
begin; Sec.new.public_send(:hidden); rescue NoMethodError; p :public_send_blocked; end

# private setter via self. (must be allowed)
class Box
  def set(x); self.val = x; end     # self.val= private setter → allowed
  def get; @val; end
  private
  def val=(x); @val = x; end
end
b = Box.new; b.set(99); p b.get
begin; Box.new.val = 1; rescue NoMethodError; p :setter_blocked; end

# respond_to? interplay
o = Sec.new
p o.respond_to?(:hidden)
p o.respond_to?(:hidden, true)
