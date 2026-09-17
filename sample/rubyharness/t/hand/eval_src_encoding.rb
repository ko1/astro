# eval は「渡した String の encoding」で始まる。マジックコメントが効くのは
# 1 行目 (shebang があれば 2 行目) にあるときだけ。
p eval("'x'".b).encoding.name
p eval("'x'".force_encoding("EUC-JP")).encoding.name
p eval("'x'").encoding.name
p eval("# encoding: UTF-8\n'x'".b).encoding.name
p eval("# frozen_string_literal: true\n# encoding: UTF-8\n'x'".b).encoding.name
p eval("# just a comment\n# encoding: UTF-8\n'x'".b).encoding.name
p eval("#!/usr/bin/ruby\n# encoding: UTF-8\n'x'".b).encoding.name
p eval("# -*- coding: EUC-JP -*-\n'x'".b).encoding.name
p eval("# CODING: Big5\n'x'".b).encoding.name
p eval("#encoding:UTF-8\n'x'".b).encoding.name

# 文字列以外の経路も同じ source encoding を見る
p eval("__ENCODING__".b).name
p eval("# encoding: EUC-JP\n__ENCODING__".b).name

b = binding
p eval("'y'".b, b).encoding.name
p eval("# encoding: UTF-8\n'y'".b, b).encoding.name
