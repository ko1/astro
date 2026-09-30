# StringScanner with end anchors ($ \Z \z) in the pattern matches at the scan pointer like
# any other pattern; ^ / \A still refer to the rest.  Run under both and diff.
require "strscan"
s = StringScanner.new("ab # c\ncd\n\nef")
p s.scan(/\w+/), s.skip(/(?:[ \n]+|\#.*$)*/), s.pos, s.scan(/\w+$/), s.scan(/\n$/), s.scan(/\n/)
p s.scan(/\w+\z/), s.eos?
s = StringScanner.new("xy\nz")
p s.scan(/xy\Z/), s.scan(/xy$/), s.scan(/\n/), s.scan(/z\Z/)
s = StringScanner.new("aab")
s.pos = 1
p s.scan(/^a/), s.scan(/\Aa/), s.check(/b$/), s.scan(/b\z/)
s = StringScanner.new("q\nr")
s.pos = 2
p s.scan(/^r$/), s.pre_match, s.matched
