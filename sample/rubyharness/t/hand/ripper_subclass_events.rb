require "ripper"
class P < Ripper
  def on_int(tok) = (@ints ||= []) << tok
  def on_ident(tok) = super
  def compile_error(msg) = (@err = msg)
  def on_parse_error(msg) = (@perr = msg)
  attr_reader :ints, :err, :perr
end
x = P.new("a = 1 + 22"); r = x.parse; p x.ints, x.error?
y = P.new("a do"); y.parse; p y.error?, (y.err || y.perr).class
class SB < Ripper
  def on_program(stmts) = [:program, stmts]
  def on_stmts_new = []
  def on_stmts_add(list, st) = list << st
  def on_var_ref(v) = v
  def on_ident(t) = t.to_sym
end
p SB.new("foo; bar").parse
p Ripper::PARSER_EVENTS.size > 100, P.new("1").respond_to?(:on_int)
class SB2 < Ripper
  def on_stmts_add(list, st) = (p [:add, st]; list << st)
  def on_stmts_new = []
  def on_ident(t) = (p [:ident, t]; t.to_sym)
  def on_vcall(x) = (p [:vcall, x]; x)
end
p SB2.new("foo").parse
r = Ripper.new("# encoding: euc-jp\n1\n__END__\nx"); p r.encoding, r.end_seen?; r.parse; p r.encoding, r.end_seen?
