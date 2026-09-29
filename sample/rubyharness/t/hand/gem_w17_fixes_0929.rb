# Fixes found by the w17 gem corpus (set, command_kit, trailblazer-activity,
# time_math2, ruby_parser, sass, writeexcel, ruby-handlebars, linguistics, barby, cgi).

# Set: binary ops keep the receiver's subclass, recursion shows Set[...], to_set { } is new
class S2 < Set; end
s = S2[1, 2, 3, 4]
p((s | [9]).class, (s & [2]).class, (s - [2]).class, (s ^ [2, 5]).class)
a = Set[1]; b = Set[2, a]; a << b
p a, b.to_s, S2[1, 2], Set[S2[1]]
p a.to_set.equal?(a), (a.to_set { _1 }).equal?(a), Set[1].to_set { _1 * 2 }, S2[1].to_set.class
e = (t = Set[1, 2]).each; t << 3; p e.size
p Set[1, 2, 3].divide.size
p((Set[].merge(a: 1) rescue $!), Set[].merge({a: 1}))
h = {a: 1}; he = h.each_value; p he.size

# anonymous Module subclass instances and temporary names
class AL < Module; end
class T1; include AL.new; end
p T1.included_modules.first.class, AL.new.inspect.start_with?("#<AL:0x"), Class.new(Module).new.inspect.start_with?("#<#<Class:0x")
c = Class.new(Set); c.set_temporary_name("_MySet"); p c, c.name, c[1, 2]

# const_get: a leading :: only makes the FIRST segment top-level
module NS; module Cmds; end; end
p((Object.const_get("::NS::Cmds::Object", false) rescue $!.class), Object.const_get("::String", false))

# a Struct inside a container prints its qualified name
module TA; class A; Output = Struct.new(:signal); end; end
p [TA::A::Output.new(1)]

# GC.start from a builtin module's method keeps the frame; compaction API exists
module Math; def self.gc_keep(x) = (GC.start; [self, x]); end
p Math.gc_keep(6), GC.compact.keys, GC.verify_compaction_references(expand_heap: true, toward: :empty).class

# #inspect result encoding follows the first element (CRuby rb_enc_copy)
[["x"], [1, "x"], {a: "x"}, {"x" => 1}, [:a, "x"], [], {}, true, nil, 1i, Comparable, [Encoding::UTF_8]].each { |o| print o.inspect.encoding, " " }
puts
p true.to_s.encoding, String.name.encoding

# Range == / hash dispatch the ends' ==
t0 = Time.at(0)
p [(t0...t0 + 1)] == [(Time.at(0)...Time.at(1))], {(t0..t0) => 1}[(Time.at(0)..Time.at(0))]

# Rational(String): exponent, underscores, r suffix, strict errors
%w[1e2 1_000 -1.5e-3 1/2e3 1.5/2 .5 5. 1r 1.5R 1__0 5.e2 1/-2 3/4/5 0x10].each do |x|
  print x, "=", (begin; Rational(x); rescue => err; err.class; end).inspect, " "
end
puts
p((Rational("1/0") rescue $!.class), Rational("1/0", exception: false), Rational("x", exception: false))

# super through a chain of 8+ nested module includes
class Base0; def setup = :base; end
module S0; def setup = super; end
prev = S0
mods = (1..9).map { m = Module.new; m.include(prev); prev = m }
k = Class.new(Base0); k.include(mods[8]); p k.new.setup, k.ancestors.size

# method_defined? / instance_method on a plain module do not see Kernel
module PM; def x; end; end
p PM.public_method_defined?(:send), PM.method_defined?(:send), Kernel.public_method_defined?(:send),
  BasicObject.public_method_defined?(:__send__), (PM.instance_method(:send) rescue $!.class)

# Regexp#== : i/m/x, FIXEDENCODING, encoding and source; not /n or internal bits
p [/\u{df}/] == [Regexp.new('\u{df}')], /a/.eql?(Regexp.new("a")), [/a/n] == [/a/], /a/ == /a/u,
  Regexp.new("a", Regexp::FIXEDENCODING) == /a/, /a/.hash == /a/n.hash

# earlier in this batch
p [Rational(3, 2)].pack("C"), [Rational(7, 2)].pack("N").bytes
so = Object.new; def so.to_str = "a.b"
p Regexp.escape(so)
class SS < String; def ==(o) = true; end
p SS.new("x") == "y", ["x"] == [SS.new("y")]
p "abc".encode("stateless-ISO-2022-JP").b, "\x92\xa4\xa2".b.force_encoding("stateless-ISO-2022-JP").encode("UTF-8")
require "rbconfig/sizeof"
p Encoding::IBM037.ascii_compatible?, RbConfig::LIMITS["INT_MAX"], RbConfig::SIZEOF["long double"]

# NKF: -w16L / -w32L pick little endian; IBM037 (EBCDIC) is a dummy with a real converter
require "nkf"
p NKF.nkf("-w16L0 -m0 -W", "☺a").bytes, NKF.nkf("-w32 -W", "a").bytes, NKF.nkf("-w16B0 -W", "a").encoding
p "Hi é".encode("IBM037").bytes, "\xC1\x81".b.force_encoding("IBM037").encode("UTF-8"), Encoding::IBM037.dummy?
p "abcd".dup.force_encoding("IBM037").to_sym.inspect
