# up/down/swap/capitalize with :ascii only transform ASCII letters, leaving
# Latin-1+ unchanged; without the option Latin-1 is mapped. vs ruby.
p "tÉ".downcase(:ascii)
p "té".upcase(:ascii)
p "École".downcase(:ascii)
p "École".downcase
p "hello WÖRLD".swapcase(:ascii)
p "àbC".upcase(:ascii)
p "àbC".upcase
s = "ÀBc"; s.downcase!(:ascii); p s
