# Regexp engine: empty iterations that change a capture keep looping, the absence operator (?~),
# nested repeat warnings, Regexp.timeout / Regexp.new(timeout:) (2026-10-16)
def t(tag)
  r = yield
  puts "#{tag}: #{r.inspect}"
rescue Exception => e
  puts "#{tag}: #{e.class}: #{e.message}"
end
t(:empty_mid) { [/(a|\2b|())*/.match("aaabbb").to_a, /(a|\2b|()){2,4}/.match("aaabbb").to_a,
                 /(a|\2b|\3()|())*/.match("aaabbb").to_a, /(a|\2b|\3()|()){2,4}/.match("aaabbb").to_a] }
t(:empty_count) { /(a|\2b|()){20,24}/.match("a" * 20 + "b" * 5).to_a }
t(:empty_lazy) { [/(a|\2b|())*?/.match("aaabbb").to_a, /(a|\2b|\3()|())*?/.match("aaabbb").to_a] }
t(:empty_stuck) do
  [/(|a|\2b|())*/, /(a||\2b|())*/, /(a|\2b||())*/, /(a|\2b|()|)*/, /(()|a|\3b|())*/,
   /(a|()|\3b|())*/, /(a|\2b|()|())*/, /(a|\3b|()|())*/].map { _1.match("aaabbb").to_a }
end
t(:empty_plain) { [/(a*)*/.match("aaa").to_a, /(?:|a)*/.match("aaa").to_a, /()\1(()|a)*/.match("aaa").to_a] }
t(:absent) do
  [Regexp.new("(?~foo)").match("hello").to_a, "foo".scan(Regexp.new("(?~foo)")), "abfooc".scan(Regexp.new("(?~foo)")),
   "aaab".match(Regexp.new("(?~aaa)")).to_a, "/* x */ y */".match(Regexp.new('/\*(?~\*/)\*/')).to_a,
   "xaby".scan(Regexp.new("(?~ab)")), "abcab".scan(Regexp.new("(?~a.?b)"))]
end
t(:absent_empty) { ["abc", "bbc"].flat_map { |s| [Regexp.new("(?~)"), Regexp.new("(?~a*)"), Regexp.new("(?~x?)")].map { _1.match(s).to_a } } }
t(:nested_repeat) { r = eval('/foo(A{0,1}+)Abar/'); [r.match("fooAAAbar").to_a, eval('/x{2}+/').match("xxxxx").to_a] }
t(:timeout_global) do
  Regexp.timeout = 0.001
  begin; /^(a*)*$/ =~ "a" * 200000 + "x"; rescue Regexp::TimeoutError => e; [e.class, e.message]; end
ensure
  Regexp.timeout = nil
end
t(:timeout_after) { [/^(a*)*$/ =~ "a" * 100 + "x", /a/ =~ "xa", Regexp.timeout] }
t(:timeout_kw) do
  Regexp.timeout = 3
  re = Regexp.new("^a*b?a*$", timeout: 0.001)
  begin; re =~ "a" * 1000000 + "x"; rescue Regexp::TimeoutError => e; [e.class, re.timeout]; end
ensure
  Regexp.timeout = nil
end
t(:timeout_kw_ok) { re = Regexp.new("b", timeout: 1); [re =~ "ab", re =~ "ab"] }
t(:level0_backref) do
  mirror = /^ (?<mirror> (?: (?<first_char>.) \g<mirror> \k<first_char-0> )? ) $/x
  broken = /^ (?<mirror> (?: (?<first_char>.) \g<mirror> \k<first_char> )? ) $/x
  [mirror.match("abccba")&.[](0), mirror.match("abccbd"), mirror.match("abba")&.[](0), broken.match("abccba"), broken.match("aaaa")&.[](0),
   /(?<a>x)\k<a-0>/.match("xx")&.[](0), (Regexp.new('(?<a>x)\k<a-1>') rescue $!.class)]
end
