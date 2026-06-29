def t; yield; rescue ArgumentError; "AE"; end
p "hello".upcase
p "HELLO".downcase(:ascii)
p "hello".upcase(:ascii)
p t { "hello".upcase(:fold) }
p t { "hello".downcase(:fold) }
p t { "hello".upcase(:bogus) }
p t { "hello".upcase(:turkic, :lithuanian) }
p t { "hello".upcase(:turkic, :bogus) }
p t { "hello".upcase(:ascii, :turkic) }
p "Hello World".swapcase
p t { "x".swapcase(:fold) }
p "hello".capitalize
p t { "x".capitalize(:fold) }
p t { "x".upcase(:a, :b, :c) }
