# class inside `class << obj` per run, default $LOAD_PATH marks, $0 = sets the process title (2026-10-16)
def t(tag)
  r = yield
  puts "#{tag}: #{r.inspect}"
rescue Exception => e
  puts "#{tag}: #{e.class}: #{e.message}"
end
C4 = []
OBJS = [Object.new, Object.new]
2.times do |i|
  obj = OBJS[i]
  $spec_i = i
  class << obj
    class X
      C4 << self
      CONST = ($spec_i + 1)
      def foo = CONST
    end
  end
end
t(:sclass_nested) { [C4.map { |k| k.new.foo }, C4[0] == C4[1], C4.map { |k| k.const_get(:CONST) }] }
t(:gem_prelude_index) { require "rbconfig"; i = $:.index(RbConfig::CONFIG["sitelibdir"]); $:[i..].all? { |p| p.instance_variable_defined?(:@gem_prelude_index) } }
t(:dollar0) { $0 = "lang-misc-title-test"; `ps -ocommand= -p#{$$}`.include?("lang-misc-title-test") }
t(:setproctitle) { Process.setproctitle("lang-misc-title-2"); `ps -ocommand= -p#{$$}`.include?("lang-misc-title-2") }
