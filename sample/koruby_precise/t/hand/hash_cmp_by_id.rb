h = {}.compare_by_identity
a = [1,2]; b = [1,2]
h[a] = 1; h[b] = 2
p h[a]; p h[b]; p h.size; p h.compare_by_identity?
g = {}
g[a] = 1; g[b] = 2
p g.size; p g.compare_by_identity?
