p Hash[[[1, 2], [3, 4]]]
def t; yield; rescue => e; "#{e.class}: #{e.message}"; end
p t { Hash[[[1, 2], 3]] }
p t { Hash[[[1, 2, 3]]] }
p t { Hash[[[1, 2], []]] }
p Hash[[[1]]]
