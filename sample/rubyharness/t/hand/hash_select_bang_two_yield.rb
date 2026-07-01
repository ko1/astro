# Hash#select!/reject!/keep_if/delete_if yield |key, value| as TWO values (a
# 1-param block gets the key), like select/reject. vs ruby.
g1 = []; {a: 1, b: 2}.select! { |x| g1 << x; true }; p g1
g2 = []; {a: 1, b: 2}.reject! { |x| g2 << x; false }; p g2
g3 = []; {a: 1, b: 2}.keep_if { |x| g3 << x; true }; p g3
g4 = []; {a: 1, b: 2}.delete_if { |x| g4 << x; false }; p g4
# ordinary 2-param mutation unaffected
h = {a: 1, b: 2, c: 3}; h.select! { |k, v| v > 1 }; p h
h2 = {a: 1, b: 2, c: 3}; h2.reject! { |k, v| v > 1 }; p h2
h3 = {a: 1, b: 2, c: 3}; h3.keep_if { |k, v| k != :b }; p h3
h4 = {a: 1, b: 2, c: 3}; h4.delete_if { |k, v| v == 2 }; p h4
h5 = {a: 1}; p h5.select! { |k, v| true }
