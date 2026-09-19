# module は instantiable ではない。Module に ::new は無いので NoMethodError。
# (rspec-core が LoadError から DidYouMean.new を作るとき、koruby の空 stub
#  module を掴んでインスタンスを作ってしまい、別のエラーに化けていた)
module Bare; end
begin
  Bare.new
rescue NoMethodError => e
  puts e.message
end
module Outer
  module Inner; end
end
begin
  Outer::Inner.new
rescue NoMethodError => e
  puts e.message
end
# 無名 module
m = Module.new
begin
  m.new
rescue NoMethodError => e
  puts e.message.start_with?("undefined method 'new' for module")
end
# Class は今までどおり
p Class.new.new.class.class
p Module.new.class
c = Class.new(Object) { def initialize(x); @x = x; end; attr_reader :x }
p c.new(5).x
