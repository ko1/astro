# A MODULE may define `new` on its singleton (listen's `module Listen::Thread`
# wraps ::Thread.new that way).  The block-carrying dispatch path raised
# "undefined method 'new' for module" before it looked at the singleton.
module L
  module T
    class << self
      def new(name, &block) = [:lnew, name, block ? block.call : :noblk]
    end
  end
end
p L::T.new("a")
p(L::T.new("b") { 1 })
pr = proc { 2 }
p L::T.new("c", &pr)
k = L::T
p(k.new("d") { 3 })
p L::T.send(:new, "e") { 4 }

module Q
  def self.new(x, &b) = [:q, x, b ? b.call : :noblk]
end
p(Q.new("f") { 5 })

# a module with no singleton `new` still says so
module R; end
begin
  R.new { 1 }
rescue NoMethodError => e
  puts e.message
end
begin
  R.new
rescue NoMethodError => e
  puts e.message
end

# and a metaclass still refuses
class C; end
begin
  C.singleton_class.new
rescue TypeError => e
  puts e.message
end
