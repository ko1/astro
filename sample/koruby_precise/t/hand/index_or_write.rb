h = {}
h[:a] ||= 1
h[:a] ||= 2
p h[:a]
p h
arr = [nil, nil]
arr[0] ||= "x"
arr[0] ||= "y"
p arr
# dedup-cache pattern from optcarrot
cache = {}
def reg(cache, x); cache[x] ||= x; end
p reg(cache, 5)
p reg(cache, 5)
p cache
# single-eval check
$calls = 0
def key; $calls += 1; :k; end
hh = {k: 10}
hh[key] ||= 99
p hh[:k]
p $calls
