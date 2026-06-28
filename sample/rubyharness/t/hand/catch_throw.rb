p catch(:a) { throw :a, 42 }
p catch(:b) { 7 }
p catch { |t| throw t, "fresh" }
p (catch(:outer) { catch(:inner) { throw :outer, "deep" }; "no" })
p (begin; throw :nope, 1; rescue UncaughtThrowError => e; "caught: #{e.class}"; end)
p (begin; throw :x; rescue => e; e.class.ancestors.include?(ArgumentError); end)
