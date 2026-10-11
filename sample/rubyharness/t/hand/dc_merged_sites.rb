# Two implicit-self calls of the same shape in one method body (then / elsif / else arms), each to a different method.
# Baked, gcc may merge their call instructions; the direct-call binding (acct48) must still reach each arm's own
# method (Journey::Parser#parse_expression: parse_group became parse_terminal on the second parse).
class Px
  def initialize(toks) = (@toks = toks; @i = 0)
  def tok = @toks[@i]
  def adv = @i += 1
  def expr
    if tok == :star
      star
    elsif tok == :lparen
      group
    else
      term
    end
  end
  def star = (adv; [:star])
  def group = (adv; [:group])
  def term = (adv; [:term, @toks[@i - 1]])
  def all = (r = []; r << expr while tok; r)
end
toks = [:a, :lparen, :b, :star, :lparen, :c]
r = nil
200.times { r = Px.new(toks).all }
p r
