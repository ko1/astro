# A shallow closure must not truncate the env chain a deeper sibling needs.
def take(&b) = b
def run(&b) = b.call

r = []
[1, 2].each do |m|
  ['a'].each do |l|
    take { l }                       # captures depth 1 only
    r << take { [m, l] }             # needs depth 2
  end
end
p r.map(&:call)

r = []
[1, 2].each do |m|
  run do
    ['a'].each do |l|
      take { l }
      r << take { [m, l] }           # depth 3 through a proc-called frame
    end
  end
end
p r.map(&:call)

r = []
[1, 2].each do |m|
  ['a'].each do |l|
    take { l }
    r << take { m }
  end
end
p r.map(&:call)

# three siblings, increasing depth, innermost written last
out = []
[10, 20].each do |a|
  ['x'].each do |b|
    [:s].each do |c|
      take { c }
      take { [b, c] }
      out << take { [a, b, c] }
    end
  end
end
p out.map(&:call)

# writes through the chain reach the right scope
acc = []
[1, 2].each do |m|
  ['a'].each do |l|
    take { l }
    acc << take { m += 100; m }
  end
end
p acc.map(&:call)

# class_exec + define_method, the rspec `let` shape
def mkgroup(&blk)
  k = Class.new
  k.define_singleton_method(:let) { |n, &b| define_method(n, &b) }
  k.define_singleton_method(:sub) { |&b| g = mkgroup(&b); (@subs ||= []) << g; g }
  k.define_singleton_method(:subs) { @subs || [] }
  k.class_exec(&blk)
  k
end
top = mkgroup do
  [:to_a, :entries].each do |meth|
    sub do
      ('a'..'c').each do |letter|
        let(:values) { ('a'..letter).to_a }
        let(:result) { [meth, letter] }
      end
    end
  end
end
p top.subs.map { |g| g.new.result }

# a module whose singleton includes a #== must use it, not Module#==
module Cmp
  def ==(other) = "CMP"
  def !=(other) = "NCMP"
end
module Emp
  class << self
    include Cmp
  end
end
p [Emp == [], Emp != [], Emp == Emp, Emp.send(:==, 1)]
class Kls
  class << self
    include Cmp
  end
end
p [Kls == [], Kls == Kls]
p [String == String, String == Integer, Comparable == Comparable]
