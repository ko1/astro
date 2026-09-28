require 'tmpdir'

# a pending autoload in an ancestor beats a defined constant further up (C::X),
# and a module body's bare constant reaches Object's autoloads
Dir.mktmpdir do |dir|
  File.write(File.join(dir, "bx.rb"), "class B; X = :B; end\n")
  File.write(File.join(dir, "cm.rb"), "module ContainerModule; def cm = :cm; end\n")
  eval <<~RUBY
    class A; X = :A; end
    class B < A; end
    B.autoload(:X, File.join(dir, "bx.rb"))
    class C < B; end
    p C::X, B.autoload?(:X)
    Object.autoload(:ContainerModule, File.join(dir, "cm.rb"))
    module IncludeModule; include ContainerModule; end
    p IncludeModule.ancestors.take(2)
  RUBY
end
