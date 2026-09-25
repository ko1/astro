# frozen_string_literal: true
# A heredoc with no interpolation, and adjacent string literals, are ONE static
# literal in CRuby, so the magic comment freezes them and they are not rebuilt
# per evaluation.  koruby lowered every InterpolatedStringNode to a runtime
# concat chain, so `<<~RUBY` came out unfrozen — rubocop's specs annotate their
# fixtures that way and the difference shows up as a mutable/frozen mismatch.
def t(l)
  p [l, yield]
rescue Exception => e
  p [l, e.class, e.message.to_s[0, 60]]
end

HD = <<~TXT
  alpha
  beta
TXT

def hd = <<~TXT
  alpha
  beta
TXT

ADJ = "ab" "cd"
X = "x"
DYN = <<~TXT
  a#{X}
TXT

t(:heredoc_frozen)  { HD.frozen? }
t(:heredoc_value)   { HD }
t(:heredoc_same)    { hd.equal?(hd) }
t(:adjacent_frozen) { ADJ.frozen? }
t(:adjacent_value)  { ADJ }
t(:dyn_frozen)      { DYN.frozen? }
t(:dyn_value)       { DYN }
t(:plain_frozen)    { "plain".frozen? }
t(:squiggly_dedent) { (<<~TXT).lines.map(&:length) }
     deep
       deeper
  TXT
t(:non_squiggly)    { (<<-TXT).frozen? }
  kept indent
  TXT
t(:enc)             { [HD.encoding, ADJ.encoding, DYN.encoding] }
t(:mutate)          { HD.dup << "!" }
t(:frozen_err)      { HD << "!" }
