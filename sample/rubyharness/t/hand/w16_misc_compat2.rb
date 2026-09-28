# Second batch of shapes from the w16 gem corpus.

# Symbol#to_s is the same frozen String every call (Ruby 4.1)
s = :abc.to_s
p [s.frozen?, s.equal?(:abc.to_s), s.equal?(:abc.name), :abc.id2name.frozen?]
begin
  s << "x"
rescue => e
  p e.class
end

# Range#lazy / #each over a #succ object range stream (biz walks weeks up to year 9999)
class Wk
  include Comparable
  attr_reader :n
  def initialize(n) = @n = n
  def succ = Wk.new(n + 1)
  def <=>(o) = n <=> o.n
end
p Range.new(Wk.new(1), Wk.new(10**9)).lazy.flat_map { |w| [w.n, -w.n] }.first(4)
p Range.new(Wk.new(1), Wk.new(10**9)).each { |w| break w.n }
p (Wk.new(1)..Wk.new(4)).map(&:n), (Wk.new(1)...Wk.new(4)).to_a.map(&:n)
p ("a".."e").lazy.map(&:upcase).to_a

# a long upto / downto / step without a block does not materialize
p 5.upto(10**9).each { |x| break x + 1 }, 5.downto(-10**9).first(2), 5.step(10**9, 2).each { |x| break x }
p 1.upto(200000).size, 200000.downto(1).first(2), 1.upto(200000).inspect, 1.upto(3).to_a
p 1.step(10, 3).to_a, 10.step(1, -3).to_a, (1...10).step(3).to_a

# nil / true / false extend their own class
module NilExt; def nil_ext = :hi; end
nil.extend(NilExt)
p nil.nil_ext, NilClass.include?(NilExt)
[1, :sym].each { |o| begin; o.extend(NilExt); rescue => e; p [e.class, e.message]; end }

# each_codepoint / << Integer in UTF-16 and UTF-32
w = "Gä\u{1F600}".encode(Encoding::UTF_32LE)
r = []; w.each_codepoint { |cp| r << cp }
p r
out = String.new.force_encoding(Encoding::UTF_32LE)
r.each { |cp| out << cp }
p out.bytes.size, out.codepoints, out.valid_encoding?
v = String.new.force_encoding("UTF-16BE"); v << 0x1F600 << 0x41
p v.bytes
[0x110000, 0xD800].each { |x| begin; v << x; rescue => e; p [e.class, e.message]; end }

# Math.sqrt of a Complex, and of -0.0
p Math.sqrt(Complex(-4, 0)), Math.sqrt(1i), Math.sqrt(Complex(3, -4)), Math.sqrt(-0.0)

# IO#seek on a write-only file
path = "/tmp/koruby_hand_seek_#{$$}.bin"
File.open(path, "wb") { |f| f.write("abcdef"); f.seek(2); f.write("Z"); p f.tell }
p File.binread(path)
File.unlink(path)

# Time.new with an explicit nil zone keeps the fraction
p Time.new(1970, 1, 1, 2, 3, 4.5r, nil).subsec, Time.new(1970, 1, 1, 2, 3, 4.5r).subsec

# NoMatchingPatternKeyError#key / #matchee
begin
  {foo: 1} => {fooo:}
rescue NoMatchingPatternKeyError => e
  p [e.key, e.matchee]
end
e = NoMatchingPatternKeyError.new("m", matchee: {a: 1}, key: :b)
p [e.key, e.matchee]
begin; NoMatchingPatternKeyError.new("m").key; rescue ArgumentError => x; p x.message; end

# return / throw out of a block yielded inside #initialize
class Yi; def initialize = yield; end
def ret_from_new = (Yi.new { return 1 }; 2)
p ret_from_new
p catch(:t) { Yi.new { throw :t, 3 }; 4 }
p Yi.new { break 5 }

# a refinement stays active in a keyword method called from a file without `using`
module HashRef; refine(Hash) { def sdesc = sort_by { |_, v| -v } }; end
module UsesRef
  using HashRef
  def self.kw(precision: 2) = {a: 1, b: 2}.sdesc
end
module NoRef; def self.call = UsesRef.kw(precision: 3); end
p NoRef.call

p $LOADED_FEATURES.include?("ruby2_keywords.rb"), $LOADED_FEATURES.include?("enumerator.so")
