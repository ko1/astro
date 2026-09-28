require 'tmpdir'

Dir.mktmpdir do |dir|
  $LOAD_PATH.unshift dir
  Dir.mkdir(File.join(dir, "hotel"))
  File.write(File.join(dir, "hotel.rb"), "class Hotel; X = true; end\n")
  File.write(File.join(dir, "hotel/pricing.rb"), "class Hotel::Pricing; end\n")
  File.write(File.join(dir, "pm.rb"), "module PM; X = :x; private_constant :X; end\n")

  # `class Hotel::Pricing` loads an Object autoload for the head segment
  Object.autoload(:Hotel, File.join(dir, "hotel.rb"))
  p require("hotel/pricing")
  # the feature is provided after its body ran: the nested require comes first
  p $LOADED_FEATURES.last(2).map { File.basename(_1) }
  p Hotel::Pricing, Hotel::X

  # an autoloaded file that makes the constant private: A::B still refuses it
  module PM; end
  PM.autoload(:X, File.join(dir, "pm.rb"))
  begin; p PM::X; rescue NameError => e; p e.message; end
  p PM.module_eval("X")
end

# `A::B::C = v` reads A::B like a constant: a missing segment raises
module AM; end
begin; eval("AM::N::X = 1"); rescue NameError => e; p e.message; end
p AM.constants
module AM; module N; end; end
AM::N::X = 1
p AM::N::X
