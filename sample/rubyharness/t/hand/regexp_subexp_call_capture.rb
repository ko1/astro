# A group re-entered by a recursive \g<name> call keeps the OUTER instance's
# start for its own end (Onigmo pairs them by level); nested calls return to
# their own callers.
p "(a(b))".match(/(?<p>\((?:[^()]|\g<p>)*\))/)[:p]
p "x(a(b)c)y".match(/(?<p>\((?:[^()]|\g<p>)*\))/)[0]
p "((()))".match(/(?<p>\(\g<p>*\))/)[:p]
p "1+(2*(3+4))".match(/(?<e>\d|\(\g<e>(?:[+*]\g<e>)*\))(?:[+*]\g<e>)*/)[0]
m = "(x(y))".match(/(?<p>\((?:[^()]|\g<p>)*\))/); p m.begin(:p), m.end(:p), m.pre_match
