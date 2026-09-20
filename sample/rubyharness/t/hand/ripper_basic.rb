# Ripper — koruby では prism の Prism::Translation::Ripper が実装。
# CRuby と同じ s 式・同じトークン列を返すことを見る。
#
# 既知の差: prism の lex_compat は :on_sp を出さない (prism 自身の仕様)。
# ここでは on_sp を落としてから比べる。
require "ripper"

SRC = <<~'RUBY'
  x = "abc"
  def foo(a, b = 1, *c, d:, **e, &f)
    a&.bar(*c) { |y| y ** 2 }
  end
  h = { a: 1, "b" => 2 }
  case x
  in [1, *rest] then rest
  end
  %w[a b].each_with_object([]) { |s, acc| acc << s }
RUBY

p Ripper.sexp(SRC)
p Ripper.sexp("a = 1 + 2")
p Ripper.sexp("1 +")                # 構文エラーは nil
p Ripper.sexp_raw("a")
p Ripper.lex("def m(a) nil end").reject { |_, type, _, _| type == :on_sp }
p Ripper.tokenize("a+1")
st = Ripper.lex("def m; end")[0][3]
p st.class.to_s, st.to_s, st.to_int, st.inspect
p Ripper::EXPR_BEG, Ripper::EXPR_FNAME, Ripper::EXPR_LABELED

# Prism の Ruby API 一式も載っている (CRuby の default gem "prism" と同じ)
require "prism"
p Prism.parse_success?("1 + 1")
p Prism.parse_failure?("1 +")
r = Prism.parse("def m(a) = a")
p r.value.class.to_s
p r.value.statements.body.first.class.to_s
p r.value.statements.body.first.name
p Prism.lex("a = 1").value.map { |tok, _| tok.type }
