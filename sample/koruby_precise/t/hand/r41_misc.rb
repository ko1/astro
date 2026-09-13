# Ruby 4.1 Module#descendants / ENV.fetch_values / MatchData#integer_at / Dir.scan.
# Differential vs CRuby 4.1.0dev.
def t(label)
  print label.ljust(46), " => "
  p(yield)
rescue Exception => e
  puts "#{e.class}: #{e.message}"
end

# descendants has no defined order, so compare as sets of markers.
def marks(mod, table)
  mod.descendants.map { |m| table.fetch(m, m) }.sort_by(&:to_s)
end

puts "-- Module#descendants"
parent = Class.new
child = Class.new(parent)
grandchild = Class.new(child)
tbl = { parent => :parent, child => :child, grandchild => :grandchild }
t("class chain, from parent") { marks(parent, tbl) }
t("class chain, from child") { marks(child, tbl) }
t("class chain, leaf") { marks(grandchild, tbl) }

mod = Module.new
including_mod = Module.new { include mod }
klass = Class.new { include including_mod }
subclass = Class.new(klass)
tbl2 = { mod => :mod, including_mod => :including, klass => :klass, subclass => :subclass }
t("module included transitively") { marks(mod, tbl2) }
t("from the including module") { marks(including_mod, tbl2) }

pmod = Module.new
prepending_mod = Module.new { prepend pmod }
pklass = Class.new { prepend pmod }
t("prepend") { marks(pmod, { prepending_mod => :prepending, pklass => :pklass }) }

prepended = Module.new
mod3 = Module.new
k3 = Class.new { prepend prepended; include mod3 }
im3 = Module.new { prepend prepended; include mod3 }
t("include after a prepend") { marks(mod3, { k3 => :k3, im3 => :im3 }) }

ip = Class.new { prepend Module.new }
ic = Class.new(ip) { prepend Module.new }
t("no internal prepend nodes") { marks(ip, { ic => :child }) }

m4 = Module.new
clone4 = m4.clone
k4 = Class.new { include clone4 }
t("a clone is a different module") { [m4.descendants.include?(k4), clone4.descendants.include?(k4)] }

m5 = Module.new
im5 = Module.new
k5 = Class.new { include im5 }
im5.include(m5)
t("included later") { marks(m5, { im5 => :im5, k5 => :k5 }) }

m6 = Module.new
Class.new { include m6 }
t("does not include self") { m6.descendants.include?(m6) }
t("Comparable excludes itself") { Comparable.descendants.include?(Comparable) }

t("no descendants (Module)") { Module.new.descendants }
t("no descendants (Class)") { Class.new.descendants }
t("returns an Array") { Class.new.descendants.class }

m7 = Module.new
o7 = Object.new
o7.extend(m7)
k7 = Class.new { extend m7 }
t("no singleton classes") do
  [m7.descendants.include?(o7.singleton_class), m7.descendants.include?(k7.singleton_class)]
end

m8 = Module.new
im8 = Module.new { include m8 }
Class.new { include m8; include im8 }
t("one entry per module") { d = m8.descendants; d == d.uniq }
t("Comparable entries are unique") { d = Comparable.descendants; d == d.uniq }

t("ancestors agree (builtins)") do
  mods = [Comparable, Enumerable, Kernel, Numeric, Integer, Float, String]
  mods.all? do |m|
    d = m.descendants
    mods.all? { |o| m.equal?(o) || d.include?(o) == o.ancestors.include?(m) }
  end
end
t("arity") { Module.new.method(:descendants).arity }
t("with an argument") { Module.new.descendants(1) }

puts "-- ENV.fetch_values"
saved = [ENV["r41_foo"], ENV["r41_bar"]]
ENV["r41_foo"] = "oof"
ENV["r41_bar"] = "rab"
t("two names") { ENV.fetch_values("r41_bar", "r41_foo") }
t("no names") { ENV.fetch_values }
t("block for a missing name") { ENV.fetch_values("r41_zz") { |k| "`#{k}' is not found" } }
t("block mixed with a hit") { ENV.fetch_values("r41_zz", "r41_foo") { |k| "`#{k}' is not found" } }
t("missing name") { ENV.fetch_values("r41_bar", "r41_y", "r41_foo", "r41_z") }
t("encoding") { ENV.fetch_values("r41_foo").first.encoding }
t("returns an Array") { ENV.fetch_values("r41_foo").class }
t("non-String name") { ENV.fetch_values("r41_foo", Object.new) }
t("Symbol name") { ENV.fetch_values(:r41_foo) }
t("arity") { ENV.method(:fetch_values).arity }
ENV["r41_foo"] = saved[0]
ENV["r41_bar"] = saved[1]

puts "-- MatchData#integer_at"
md = /(\d{4})(\d{2})(\d{2})/.match("20260308")
t("whole match") { md.integer_at(0) }
t("groups 1..3") { (1..3).map { |i| md.integer_at(i) } }
t("negative indices") { (-4..-1).map { |i| md.integer_at(i) } }
t("past the group count") { md.integer_at(4) }
t("far past the group count") { md.integer_at(99) }
t("far below") { md.integer_at(-99) }
t("non-participating group") { /\b(\d)?\b/.match("THX1138.").integer_at(1) }
t("non-numeric capture") { /(\w)?/.match("THX1138.").integer_at(1) }
t("named group (String)") { /(?<a>\d+)/.match("12").integer_at("a") }
t("named group (Symbol)") { /(?<a>\d+)/.match("12").integer_at(:a) }
t("unknown group name") { /(?<a>\d+)/.match("12").integer_at(:nope) }
t("base 16") { /\w+/.match("0c").integer_at(0, 16) }
t("base 10 of '0c'") { /\w+/.match("0c").integer_at(0) }
t("base 0 prefixes") do
  %w[010 0x10 0d10 0o10 0b10].map { |s| /\w+/.match(s).integer_at(0, 0) }
end
t("base 0 without a prefix") { /\S+/.match("ff").integer_at(0, 0) }
t("base 0 of '08'") { /\S+/.match("08").integer_at(0, 0) }
t("base 0 of '0_10'") { /\S+/.match("0_10").integer_at(0, 0) }
t("base 0 of '-0x10'") { /\S+/.match("-0x10").integer_at(0, 0) }
t("base 16 of '0b11'") { /\S+/.match("0b11").integer_at(0, 16) }
t("base 2 of '0x11'") { /\S+/.match("0x11").integer_at(0, 2) }
t("base 8 of '09'") { /\S+/.match("09").integer_at(0, 8) }
t("underscores") { /\S+/.match("1_0").integer_at(0) }
t("trailing underscore") { /\S+/.match("10_").integer_at(0) }
t("leading underscore") { /\S+/.match("_10").integer_at(0) }
t("leading whitespace") { /.+/.match(" 10 ").integer_at(0) }
t("a plus sign") { /\S+/.match("+10").integer_at(0) }
t("a minus sign") { /\S+/.match("-10").integer_at(0) }
t("a lone sign") { /\S+/.match("-").integer_at(0) }
t("whitespace only") { /\s+/.match("   ").integer_at(0) }
t("trailing junk") { /\S+/.match("0.5").integer_at(0) }
t("a bignum result") { /\S+/.match("1" * 40).integer_at(0) }
t("empty capture, base 10") { /\S*/.match("").integer_at(0) }
t("empty capture, base 1") { /\S*/.match("").integer_at(0, 1) }
t("empty capture, base 37") { /\S*/.match("").integer_at(0, 37) }
t("base 1") { /\w+/.match("10").integer_at(0, 1) }
t("base 37") { /\w+/.match("10").integer_at(0, 37) }
t("base -1") { /\w+/.match("10").integer_at(0, -1) }
t("Float base truncates") { /\w+/.match("10").integer_at(0, 16.9) }
t("base too big for int") { md.integer_at(0, 2**40) }
t("no arguments") { md.integer_at }
t("three arguments") { md.integer_at(0, 10, 1) }
t("arity") { md.method(:integer_at).arity }

puts "-- Dir.scan / Dir#scan"
require "tmpdir"
require "fileutils"
Dir.mktmpdir("r41scan") do |dir|
  File.write(File.join(dir, "a.txt"), "x")
  File.write(File.join(dir, ".dot"), "y")
  Dir.mkdir(File.join(dir, "sub"))
  File.symlink("a.txt", File.join(dir, "link"))
  t("Dir.scan") { Dir.scan(dir).sort }
  t("Dir.scan with a block") do
    acc = []
    r = Dir.scan(dir) { |n, ty| acc << [n, ty] }
    [r, acc.sort]
  end
  t("Dir.scan(encoding:)") { Dir.scan(dir, encoding: "euc-jp").sort.map { |n, ty| [n, ty, n.encoding.to_s] } }
  t("Dir.scan on #to_path") do
    o = Object.new
    o.define_singleton_method(:to_path) { dir }
    Dir.scan(o).sort
  end
  # only the class: CRuby and koruby label the failing syscall differently
  # (dir_initialize vs opendir), and the path carries a pid.
  t("Dir.scan on a missing dir") do
    begin
      Dir.scan(File.join(dir, "nope"))
    rescue SystemCallError => e
      [e.class, e.is_a?(SystemCallError)]
    end
  end
  t("Dir#scan") { d = Dir.new(dir); r = d.scan.sort; d.close; r }
  t("Dir#scan with a block") do
    d = Dir.new(dir)
    acc = []
    r = d.scan { |n, ty| acc << [n, ty] }
    d.close
    [r, acc.sort]
  end
  t("Dir#scan(encoding:)") do
    d = Dir.new(dir, encoding: "euc-jp")
    r = d.scan.sort.map { |n, ty| [n, ty, n.encoding.to_s] }
    d.close
    r
  end
  t("names match #children") { Dir.scan(dir).map(&:first).sort == Dir.children(dir).sort }
end
