# Symbol#inspect — special gvar / operator / setter symbols print bare
syms = [:$+, :$~, :"$-w", :$:, :$?, :$<, :$>, :$0, :$1, :$23, :$_, :$ruby,
        :@iv, :@@cv, :+, :-, :<=>, :[], :[]=, :+@, :foo, :foo?, :foo=,
        :"has space", :"123abc", :"", :"with\"quote"]
syms << "foo!".to_sym
syms.each { |s| puts s.inspect }
