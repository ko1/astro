# A Regexp literal is one object per literal (CRuby); StringScanner's per-pattern cache keys
# on that identity.  Interpolated and /o literals, and String patterns (frozen literal,
# mutable, mutated between scans) behave as CRuby.  Run under both and diff.
def f = /x/i
p f.equal?(f), f.frozen?, 3.times.map { /\s+/ }.uniq(&:object_id).size
def g(v) = /a#{v}/
p g(1).equal?(g(1)), g(1) == g(1)
def h(v) = /b#{v}/o
p h(1).equal?(h(2)), h(3).source

require "strscan"
s = StringScanner.new("abc  def{x}")
p s.scan(/\w+/), s.skip(/\s*/), s.scan("def"), s.skip("{"), s[0], s.pos
pat = +"x"
p s.scan(pat), s.matched
pat.replace("}")
p s.scan(pat), s.eos?, s.pre_match
s = StringScanner.new("line1\nline2")
p s.scan(/^line\d/), s.skip(/\n/), s.scan(/^line\d/), s.scan(/\Aq/)
s = StringScanner.new("\xff abc".b)
p s.skip(/./n), s.scan(/ (abc)/n), s[1]
s = StringScanner.new("ab\xffc")
begin; s.scan(/ab/); rescue ArgumentError => e; p e.class; end
p s.scan("ab")
