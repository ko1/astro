# `class Top::Mid::Leaf` has to fire the autoload registered for the INTERMEDIATE
# constant `Top::Mid`.  koruby bakes a definition's whole dotted path into one
# symbol and resolves it with a walker that takes no CTX, so it could not run the
# require; only the final component was handled.  (pry does
# `class Bundler::UI::Shell` right after `require "bundler"`.)
require "tmpdir"

def t(l)
  p [l, yield]
rescue Exception => e
  p [l, e.class, e.message.to_s[0, 60]]
end

DIR = Dir.mktmpdir
File.write(File.join(DIR, "mid.rb"), <<~RB)
  module Top
    module Mid
      class Leaf; def hi = :hi; end
      module Deep; class Deeper; def d = :d; end; end
      VALUE = 7
    end
  end
RB
File.write(File.join(DIR, "two.rb"), <<~RB)
  module Two
    module Inner
      class Thing; def who = :two; end
    end
  end
RB

module Top
  autoload :Mid, File.join(DIR, "mid")
end
module Two
  autoload :Inner, File.join(DIR, "two")
end

t(:registered)  { [Top.autoload?(:Mid).is_a?(String), Two.autoload?(:Inner).is_a?(String)] }

# reopening through the autoloaded namespace triggers it and keeps the file's body
class Top::Mid::Leaf
  def extra = :extra
end
t(:reopen)      { [Top::Mid::Leaf.new.hi, Top::Mid::Leaf.new.extra] }
t(:const_value) { Top::Mid::VALUE }
t(:autoload_q)  { Top.autoload?(:Mid) }

# a module definition path works the same way, and so does a deeper one
module Two::Inner::Extra
  def self.hi = :extra_mod
end
t(:module_path) { [Two::Inner::Extra.hi, Two::Inner::Thing.new.who] }
t(:deep_path)   { Top::Mid::Deep::Deeper.new.d }

# a genuinely missing intermediate is still a NameError, not a silent create
t(:missing)     { (class Nope::Missing::X; end) rescue [$!.class, $!.message[0, 40]] }
t(:plain_class) { class PlainTop; class Nested; def n = :n; end; end; PlainTop::Nested.new.n }

require "fileutils"
FileUtils.remove_entry(DIR)
