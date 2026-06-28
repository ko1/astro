# Symbol#inspect — special gvar / operator / setter symbols print bare
[:$+, :$~, :"$-w", :$:, :$?, :$<, :$>, :$0, :$1, :$23, :$_, :$ruby,
 :@iv, :@@cv, :+, :-, :<=>, :[], :[]=, :+@, :foo, :foo?, :foo\!, :foo=,
 :"has space", :"123abc", :"", :"with\"quote"].each { |s| puts s.inspect }
