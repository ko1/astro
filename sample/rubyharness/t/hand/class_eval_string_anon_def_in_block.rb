# class_eval(String) on a class with no name, called from a named class's method:
# a def inside a block there still reads the receiver's constants.
TOP = Module.new
class Compiler
  def compile(scope) = scope.class_eval("::TOP.class_eval do\ndef hey(locals)\nfoo = [] ; foo << %Q{Hey \#{CONSTANT}!} ; foo.join\nend;end;", "(__TEMPLATE__)", 0)
end
person = Class.new { self::CONSTANT = "Bob" }
Compiler.new.compile(person)
p TOP.instance_method(:hey).bind_call(Object.new, {})
