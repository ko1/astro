# sprintf `*` width/precision: sequential, positional (*N$), and negative width
# implying left-justify. vs ruby.
p("%*d" % [5, 42])
p("%-*d" % [5, 42])
p("%*d" % [-5, 42])
p("%.*f" % [2, 3.14159])
p("%1$*2$d" % [42, 5])
p("%2$*1$d" % [5, 42])
p("%*.*f" % [8, 2, 3.14159])
p("%*d|%*d" % [3, 1, 4, 2])
