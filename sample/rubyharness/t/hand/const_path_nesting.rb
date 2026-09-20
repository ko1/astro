# `module A::B::C` は A::B::C 一つとして nest する。名前だけを焼くと、
# 同名の別 C (Dry::Container::Config と Zeitwerk::Loader::Config) に
# 定数が落ちていた。
module Dq; class Cont; class Config; D1 = 1; end; end; end
module Zw; class Loader; end; end
module Zw::Loader::Config
  p Module.nesting
  UNDEFINED = Object.new.freeze
  private_constant :UNDEFINED
  def self.peek = UNDEFINED
end
p Zw::Loader::Config.peek.class
p Zw::Loader::Config.constants, Dq::Cont::Config.constants
class Zw::Loader::Klass
  p Module.nesting
  K1 = 2
end
p Zw::Loader::Klass::K1
module Outer3; class P3; end; end
module Outer3
  class P3::Q3
    p Module.nesting
    Y3 = 3
  end
end
p Outer3::P3::Q3::Y3
