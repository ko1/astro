require "ripper"
src = "# -*- coding: euc-jp -*-\n_(\"\xA4\xA2\")\n".dup.force_encoding("EUC-JP")
Ripper.lex(src).each { |(_, ev, tok)| p [ev, tok, tok.encoding] if ev == :on_tstring_content || ev == :on_ident }
src2 = "_(\"\xA4\xA2\")\n".dup.force_encoding("EUC-JP")
Ripper.lex(src2).each { |(_, ev, tok)| p [ev, tok, tok.encoding] if ev == :on_tstring_content }
p Ripper.lex("a = 'x'".b).map { _1[2].encoding }.uniq
p Ripper.tokenize("_(\"\xA4\xA2\")".dup.force_encoding("EUC-JP")).map(&:encoding).uniq
