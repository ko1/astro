# 裸の定数参照は、字句的に外側のモジュールに登録された autoload も見る。
# koruby は top-level に同名の定数があるとそちらを先に拾っていた
# (prelude が `module DidYouMean; end` を置いているので rspec-core が踏む:
#  rspec は RSpec::Core::DidYouMean を autoload で登録している)。
require "tmpdir"
$dir = Dir.mktmpdir("korb_al")
File.write(File.join($dir, "thing.rb"), <<~SRC)
  module App
    module Core
      class Thing
        def initialize(x); @x = x; end
        def x; @x; end
      end
    end
  end
SRC
# autoload した先が「登録したスコープ」ではなく別の字句スコープに定義する形
File.write(File.join($dir, "cb.rb"), '$cb.call')

module Thing; end      # top-level の同名 stub
module App
  module Core
    autoload :Thing, File.join($dir, "thing.rb")
    class User
      def bare;   Thing.new(1).x; end
      def scoped; App::Core::Thing.new(2).x; end
    end
  end
end
p App::Core::User.new.bare
p App::Core::User.new.scoped
p App::Core::Thing.class
p ::Thing.class
p App::Core::User.new.bare

module P
  $cb = -> {
    class Lex
      X = :defined_in_current
    end
  }
  autoload :X, File.join($dir, "cb.rb")
  class Lex
    p X
    p Lex::X
  end
  p self.autoload?(:X)
  p const_defined?(:X)
  begin; X; rescue NameError; p :NameError; end
end
