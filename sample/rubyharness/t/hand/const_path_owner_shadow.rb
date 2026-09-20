# 定数表はフラットなので、`class ERB::Compiler` の ERB が別名前空間の
# Temple::ERB に当たっていた。焼いたパスの頭は top-level の名前として引く。
module Temple2
  module ERBX
    class Parser; end
  end
end
class ERBX; end
class ERBX::Compiler
  class Scanner; def hi = :hi; end
  S2 = Scanner
end
p ERBX::Compiler::S2.new.hi
p ERBX.constants, Temple2::ERBX.constants, ERBX::Compiler.constants
# 囲むクラスの名前も (末尾だけでなく) パス全体で焼く
module NsA
  class Inner; end
end
class Inner; end
module NsA
  class Inner
    class Deep2; def d = :deep; end
  end
end
p NsA::Inner::Deep2.new.d
p Inner.constants, NsA::Inner.constants
# `module A::B` の A は「囲む名前空間から外向きに」解決する。
# coderay の patterns.rb が `module CodeRay; module Scanners; module Ruby::Patterns`
# と書く一方、koruby には top-level の Ruby があるので両方要る。
module Rby; end                        # top-level の同名 (koruby には実在の Ruby もある)
module CodeRay2
  module Scanners2
    class Rby; end
    module Rby::Patterns
      KEY = :key
    end
  end
end
p CodeRay2::Scanners2::Rby::Patterns::KEY
p Rby.constants, CodeRay2::Scanners2::Rby.constants
