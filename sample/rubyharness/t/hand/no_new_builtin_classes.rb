# ::new is undefined on Integer, Float, Rational, Complex, Symbol, nil/true/false and BigDecimal.
require "bigdecimal"
ks = [NilClass, TrueClass, FalseClass, Integer, Float, Symbol, Rational, Complex, BigDecimal]
p ks.map { _1.respond_to?(:new) }, ks.map { (_1.new rescue $!.class) }
p BigDecimal("1.5").to_s, Rational(1, 3), Object.respond_to?(:new), Class.new(Object).new.class.class
p((BigDecimal.new("1") rescue $!.class), (Integer.new rescue $!.message))
