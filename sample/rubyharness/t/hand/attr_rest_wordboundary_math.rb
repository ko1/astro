# a method whose body is a bare @ivar is an attr_reader only with no parameters at all
class L; def initialize = @v = 3; def v(*) = @v; def w(*a) = @v; def x(**) = @v; def y = @v; end
p L.new.v(5), L.new.w(1, 2), L.new.x(a: 1), L.new.y, (L.new.y(1) rescue $!.class)
re = %r{\A([[[:word:]]\.]+\b)}i
p re.match("ρ * 2"), re.match("abc"), /\A[[:word:]]+/.match("ρx"), /\A\w+/.match("ρ"), /ρ\b/.match("ρ * 2"), /\A[[[:word:]]\.]+/i.match("ρ")
p "ρ".match?(/[[:word:]]/), "ρ".match?(/[[:alpha:]]/i), "Ρ".downcase
p "ρx ab".scan(/\b\w+\b/), "日本語 test".scan(/\b/).size, "é!".match?(/é\b/), "aé".match?(/a\B/), "x é y".gsub(/\b/, "|"), "été".scan(/\b/).size
p "héllo wörld".split(/\b/), "a_b1 c".scan(/\b\S/)
p Math.respond_to?(:copysign), Math.methods(false).size
