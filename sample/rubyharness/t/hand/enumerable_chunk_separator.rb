class C; include Enumerable; def each; [1,2,3,3,1].each { |v| yield v }; end; end
p C.new.chunk { |x| x }.to_a
p C.new.chunk { |x| x.even? ? :_separator : 1 }.to_a
p C.new.chunk { |x| x == 2 ? nil : 1 }.to_a
p [1, 1, 2, 3, 3].chunk { |x| x }.to_a
