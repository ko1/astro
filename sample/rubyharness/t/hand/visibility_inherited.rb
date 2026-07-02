# public/private/protected :sym works on an inherited/included method (creates
# a visibility-override entry on the class, keeping super resolution). vs ruby.
module Mixin
  private
  def secret; "secret"; end
  def helper; "help"; end
end
class Host
  include Mixin
  public :secret     # promote the included private method to public
end
p Host.new.secret
begin
  Host.new.helper    # still private
rescue NoMethodError => e
  p e.class
end

# private on an inherited (Object) method
class Quiet
  private :to_s
end
begin
  Quiet.new.to_s
rescue NoMethodError => e
  p e.class
end

# super still resolves through an override entry
class Base
  def greet; "base"; end
end
module Louder
  def greet; super.upcase; end
end
class Child < Base
  include Louder
  public :greet
end
p Child.new.greet
