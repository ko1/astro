# String / Symbol / Hash-label inspect escape what Onigmo does not call printable
# (C1 controls, U+2028/2029, unassigned, noncharacters) as \uNNNN.
p " ", " ", " ", "​", "\u0085", "﻿", "á", "\u{1F600}", "\u{E000}", "\u{FFFF}", "\u{10FFFF}", "͸"
p "é <", :"a ", ["\u0085x"], {" " => 1}, "\u{1F600}\u{E000}"
p :äöü, :"a b", {" ": 1}, :"été"
