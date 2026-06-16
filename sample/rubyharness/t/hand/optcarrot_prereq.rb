# optcarrot prerequisite features (oracle = CRuby).
# When this passes, the namespace / visibility / global-var gaps that block
# optcarrot are covered.

# --- namespaced module + constant path (A::B) ---
module Opt
  VERSION = "1.2"
  class Cpu
    def self.make; "cpu"; end
    def tick; 42; end
  end
  module Inner
    THRESH = 7
  end
end

p Opt::VERSION
p Opt::Cpu.make
p Opt::Cpu.new.tick
p Opt::Inner::THRESH

# sibling reference inside the module resolves unqualified
module Opt
  class Ppu
    def cpu_name; Cpu.make; end
  end
end
p Opt::Ppu.new.cpu_name

# --- method visibility as no-ops (not enforced, but must not error) ---
class Widget
  def pub; helper; end
  private
  def helper; "helped"; end
  public
  def pub2; "pub2"; end
end
p Widget.new.pub
p Widget.new.pub2

# private with a symbol arg
class Gadget
  def go; secret; end
  def secret; "shh"; end
  private :secret
end
p Gadget.new.go

# --- global variables ---
$counter = 0
$counter += 5
p $counter
def bump; $counter += 1; end
bump
p $counter
$name = "opt"
p $name
