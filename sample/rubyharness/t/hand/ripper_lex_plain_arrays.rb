# Ripper.lex hands back plain Arrays (prism's LexCompat wraps each token in a
# SimpleDelegator; rspec-support indexes token[1] millions of times).
require 'ripper'
toks = Ripper.lex("def a(x) = x + 1 # c\n")
p toks.map(&:class).uniq, toks.map { _1[1] }, toks.first[0], toks.first[3].class
