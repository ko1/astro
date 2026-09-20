# レシーバなしの class_eval(String) も呼び出し元のローカルが見える
# (faraday の memoized が `class_eval <<-RUBY` でメソッドを生やす)。
class Foo2
  def self.gen(key)
    class_eval <<-RUBY, __FILE__, __LINE__ + 1
      remove_method(key) if method_defined?(key, false)
      def #{key}() :#{key}; end
    RUBY
  end
  def abc = :old
end
Foo2.gen(:abc)
p Foo2.new.abc
module Bar2
  def self.gen(n)
    module_eval "def #{n}; :#{n}; end"
  end
end
Bar2.gen(:zz)
class UseBar2; include Bar2; end
p UseBar2.new.zz
class Foo3
  def self.gen(v) = instance_eval("@v = v")
end
Foo3.gen(9)
p Foo3.instance_variable_get(:@v)
# class_eval(String) で生やしたメソッドの定数は、レシーバの lexical scope で
# 解決される (psych の TreeBuilder が Psych::Nodes をこう引く)。
module Ns5
  module Nodes5
    class Seq; def s = :seq; end
  end
  class Builder
    %w[Seq].each do |n|
      class_eval <<~RUBY, __FILE__, __LINE__ + 1
        def make_#{n.downcase}
          Nodes5::#{n}.new
        end
      RUBY
    end
  end
end
p Ns5::Builder.new.make_seq.s
