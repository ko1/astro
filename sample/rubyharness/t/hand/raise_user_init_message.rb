# raise Klass, obj with a user #initialize: the message is what that initializer sets (super() → class name);
# a command starting with VAR=value, a # comment or leading blanks goes through the shell
class MyException < StandardError
  def initialize(object) = (super(); @object = object)
end
begin; raise MyException, [1, 2]; rescue => e; p e.message, e.instance_variable_get(:@object); end
class E2 < StandardError
  def initialize(a) = super("got #{a}")
end
begin; raise E2, 5; rescue => e; p e.message; end
begin; raise E2.new(6), "override"; rescue => e; p e.message; end
begin; raise StandardError, "m"; rescue => e; p e.message; end
class E3 < StandardError; def self.exception(*a) = (p [:exc, a]; new("x")); end
begin; raise E3, 1; rescue => e; p e.message; end
begin; raise E3; rescue => e; p e.message; end
class Y < StandardError; def initialize(a) = nil; end
begin; raise Y, "zz"; rescue => e; p e.message; end
class Z < StandardError; def initialize(m = "dflt") = super; end
begin; raise Z; rescue => e; p e.message; end
begin; raise Z, "given"; rescue => e; p e.message; end
p `A=1 echo $A-x`, `  echo lead`, `echo a # comment`
p((begin; `A=1 ./nonexist-zz 2>/dev/null`; $?.exitstatus; rescue => e; e.class; end))
