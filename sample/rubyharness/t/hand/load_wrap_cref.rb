# load(file, true): the wrap module is the cref for the loaded file only.
# A method REACHED from it keeps its own lexical cref, and `self` is a clone of
# main, so whatever was extended onto main still answers inside the wrapper.
dir = File.join(Dir.tmpdir, "korb_lw_#{Process.pid}")
Dir.mkdir(dir) unless Dir.exist?(dir)
lib = File.join(dir, "lw_lib.rb")
File.write(lib, <<~RB)
  module LW
    def self.helper = "real-helper"
    class Bar
      def run = LW.helper
    end
  end
RB
wrapped = File.join(dir, "lw_wrapped.rb")
File.write(wrapped, <<~RB)
  module LW
    INSIDE = 1
  end
  puts ::LW::Bar.new.run
  puts greeting
RB

require lib

module MainExt
  def greeting = "from-main-singleton"
end
self.extend MainExt

load wrapped, true

p LW::Bar.new.run
p LW.singleton_class.method_defined?(:helper)

File.delete(lib, wrapped)
Dir.rmdir(dir)
