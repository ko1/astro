class TS; def to_str; "abc"; end; end
p ("abc" <=> "abc")
p ("b" <=> "a")
p ("zzz" <=> TS.new)
p ("abc" <=> TS.new)
p ("x" <=> 5)
