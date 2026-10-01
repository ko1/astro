# Receiverless calls whose self is a builtin (String / Array / Hash / Integer / nil / Symbol): the
# call site's cache keys on the dispatch class, so a private helper, a singleton, a later
# redefinition, method_missing, a raising builtin and a block-taking builtin all behave as in CRuby.
class String
  def helper = "h:#{self}"
  def use_helper = helper
  def use_size = size                       # builtin C method, no receiver
  def use_upcase_raise = Integer("zz#{self}")
  private def priv = :priv
  def use_priv = priv
  def use_each = each_char.map { _1 }       # block-taking builtin goes the full way
end
class Array
  def sum2 = sum + length
end
class Integer
  def method_missing(n, *a) = n == :ghost ? [:ghost, self] : super
  def respond_to_missing?(n, p = false) = n == :ghost || super
  def call_ghost = ghost
  def dbl = self * 2
  def use_dbl = dbl
end
class NilClass; def nil_helper = :nh; def use_nh = nil_helper; end
class Symbol; def sym_helper = to_s.size; def use_sh = sym_helper; end
s = "ab"
def s.helper = "singleton"
r = []
3.times do
  r << ["xy".use_helper, s.use_helper, "abc".use_size, "q".use_priv, "hé".use_each,
        [1, 2].sum2, 5.call_ghost, 7.use_dbl, nil.use_nh, :abcd.use_sh]
end
p r.uniq
begin
  "1".use_upcase_raise
rescue ArgumentError => e
  p e.message, e.backtrace_locations.first(2).map(&:label)
end
class String; def helper = "redef:#{self}"; end
class Integer; def dbl = :redef; end
p "xy".use_helper, 7.use_dbl, s.use_helper
class String; remove_method :helper; end
p(("xy".use_helper rescue $!.class))
