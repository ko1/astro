# A pending `autoload` at a lexical level counts as "the constant lives here",
# so it beats a real constant further OUT.  koruby walked the cref chain twice —
# once for real constants, then once for autoloads — so an outer real constant
# was found first and the inner autoload never fired.  bundler's
# `module Bundler::Plugin; autoload :Index; def index = Index.new` resolved to
# Bundler::Index and blew up with NoMethodError.
require "tmpdir"

def t(l)
  p [l, yield]
rescue Exception => e
  p [l, e.class, e.message.to_s[0, 60]]
end

DIR = Dir.mktmpdir
File.write(File.join(DIR, "inner.rb"), <<~RB)
  module Outer
    module Mid
      class Thing; def who = :inner; end
      DEPTH = :inner
    end
  end
RB
File.write(File.join(DIR, "deep.rb"), "module A1; module B1; module C1; class T; def who = :deep; end; end; end; end\n")

module Outer
  class Thing; def who = :outer; end
  DEPTH = :outer
  module Mid
    autoload :Thing, File.join(DIR, "inner")
    autoload :DEPTH, File.join(DIR, "inner")
    def self.pick_class = Thing
    def self.pick_const = DEPTH
  end
end

t(:inner_class)  { Outer::Mid.pick_class.new.who }
t(:inner_const)  { Outer::Mid.pick_const }
t(:qualified)    { Outer::Mid::Thing.new.who }
t(:outer_intact) { [Outer::Thing.new.who, Outer::DEPTH] }
t(:autoload_q)   { Outer::Mid.autoload?(:Thing) }

# three levels deep: the innermost registration still wins
module A1
  class T; def who = :a1; end
  module B1
    class T; def who = :b1; end
    module C1
      autoload :T, File.join(DIR, "deep")
      def self.pick = T
    end
  end
end
t(:three_deep)   { A1::B1::C1.pick.new.who }
t(:mid_intact)   { [A1::T.new.who, A1::B1::T.new.who] }

# no autoload anywhere → the ordinary lexical walk is unchanged
module P1
  class Q; def who = :p1; end
  module R1
    def self.pick = Q
  end
end
t(:plain_lexical) { P1::R1.pick.new.who }
t(:top_fallback)  { module Z1; def self.pick = String; end; Z1.pick.name }

require "fileutils"
FileUtils.remove_entry(DIR)
