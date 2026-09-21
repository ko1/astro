# `super` inside a block that BECOMES a method body.  The parser bakes the name
# of the def the block was written in (it cannot know the block's fate), so the
# runtime has to notice that the body is this define_method entry — including
# when the `super` sits in a further block nested inside it.  rspec's
# `RSpec::Matchers.define ... match { }` is built exactly this way.
module Fallback
  def matches?(x) = [:fallback_matches, x]
  def match(x) = [:fallback_match, x]
end

class Matcher
  include Fallback
  @user = Module.new
  class << self
    def user_mod = @user
    def match(&blk)
      define_user_override(:matches?, blk) { |actual| super(actual) }
    end
    def match_nested(&blk)
      define_user_override(:matches?, blk) { |actual| [1].map { super(actual) }.first }
    end
    def define_user_override(name, user_def, &our_def)
      @user.send(:define_method, name, &user_def)
      include @user
      define_method(name, &our_def)
    end
  end
end

Matcher.match { |actual| [:user_block, actual] }
p Matcher.new.matches?(42)
Matcher.match_nested { |actual| [:user_nested, actual] }
p Matcher.new.matches?(7)
p Matcher.ancestors.first(1)

# a literal define_method block still resolves its own name
class A0
  def go(x) = [:a0, x]
end
class B0 < A0
  define_method(:go) { |x| super(x + 1) }
end
p B0.new.go(1)

# and `super` in a block inside an ordinary method still uses that method
class A1
  def run(x) = [:a1, x]
end
class B1 < A1
  def run(x)
    [1].map { super(x * 3) }.first
  end
end
p B1.new.run(2)

# __method__ inside a define_method body
class C1
  define_method(:named) { __method__ }
end
p C1.new.named
