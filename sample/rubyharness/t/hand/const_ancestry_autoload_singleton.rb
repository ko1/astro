# Constant lookup: a pending autoload in an included module loads before an
# ancestor further up (Object's ::File) answers; inside `class << self` the
# ancestry searched is the singleton class's, not the outer class's.
require "tmpdir"
dir = Dir.mktmpdir("cal")
File.write("#{dir}/lib_file.rb", "module CALib; File = :lib_file; end\n")
File.write("#{dir}/lib_deep.rb", "module CADeep; Deep = :deep; end\n")
module CALib; end
CALib.autoload :File, "#{dir}/lib_file"
module CADeep; end
CADeep.autoload :Deep, "#{dir}/lib_deep"
module CAMid; include CADeep; end
class CAC
  include CALib
  include CAMid
  def m = File
  def d = Deep
  def self.k = File
end
p CAC.new.m, CAC.new.d, CAC.k, CALib.autoload?(:File)
module CAL2; X = :lib; end
class CABase; include CAL2; end
class CABase
  class << self
    def a = (X rescue :NameError)
    def b = (CAL2::X)
    Y = :sing
    def y = Y
  end
  def c = X
end
p CABase.a, CABase.b, CABase.y, CABase.new.c
o = Object.new
class << o
  include CAL2
  def z = X
end
p o.z
require "fileutils"; FileUtils.rm_rf(dir)
