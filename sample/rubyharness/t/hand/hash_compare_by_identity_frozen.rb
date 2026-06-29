h = {}
h.compare_by_identity
p h.compare_by_identity?
def t; yield; rescue FrozenError; "FE"; end
p t { {}.freeze.compare_by_identity }
