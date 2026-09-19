# CRuby の Method は捕まえた時点の定義を保持する。あとから singleton を生やしても
# #call はそれに乗り換えない (rspec-support の Mutex.method(:new) がこれに依存)。
class Foo; end
M1 = Foo.method(:new)
class Foo
  def self.new; M1.call; end
end
p Foo.new.class

class Bar
  def greet; :orig; end
end
b = Bar.new
m = b.method(:greet)
def b.greet; :singleton; end
p m.call
p m.owner
p b.greet

# UnboundMethod は取り出したクラスから解決する (bound 側の singleton 鎖ではない)
p Array.instance_method(:filter) == Array.instance_method(:select)
p Array.instance_method(:select).owner
p [].method(:filter) == [].method(:select)

# public :m でコピーが出来ても super は元のモジュールから辿る
module PA
  private
  def derp(msg) = "A"
end
module PB
  private
  def derp = "B" + super("x")
end
class PC
  include PA
  include PB
  public :derp
end
p PC.new.derp
p PC.new.method(:derp).owner
p PC.new.method(:derp).super_method.owner

# rspec-support の実パターン
class MyMutex < ::Mutex
  NEW_METHOD = MyMutex.method(:new)
  def self.new; NEW_METHOD.call; end
end
p MyMutex.new.class
p MyMutex.new.synchronize { :locked }
