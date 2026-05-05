# Stdlib: time / os / collections / itertools.

import time
import os
from collections import deque, Counter, defaultdict, namedtuple, OrderedDict
from itertools import chain, count, islice, takewhile, dropwhile
from itertools import accumulate, product, combinations, permutations

# time (just smoke).
t0 = time.perf_counter()
time.sleep(0.001)
t1 = time.perf_counter()
print(t1 - t0 >= 0.0)

# os.path.
print(os.path.join("a", "b", "c"))
print(os.path.basename("/foo/bar/baz"))
print(os.path.dirname("/foo/bar/baz"))
print(os.path.splitext("foo.txt"))
print(os.path.splitext("nodot"))

# deque.
d = deque([1, 2, 3])
d.append(4)
d.appendleft(0)
print(list(d))
print(d.popleft())
print(d.pop())
print(list(d))

# Counter.
c = Counter("aabracadabra"[:11])
print(c.most_common(2))

# defaultdict.
dd = defaultdict(int)
for ch in "hello":
    dd[ch] = dd[ch] + 1
print(sorted(dd.items()))

# namedtuple.
P = namedtuple("P", "x y")
p = P(3, 4)
print(p[0], p[1])
print(list(p))

# OrderedDict alias.
od = OrderedDict([("z", 1), ("a", 2), ("m", 3)])
print(list(od.keys()))

# itertools.
print(list(chain([1,2], [3,4])))
print(list(islice(count(), 4)))
print(list(islice(count(10, 2), 5)))
print(list(takewhile(lambda x: x < 4, range(10))))
print(list(dropwhile(lambda x: x < 4, range(10))))
print(list(accumulate([1,2,3,4,5])))
print(list(product([1,2], [3,4])))
print(list(combinations([1,2,3,4], 2)))
print(list(permutations([1,2,3], 2)))
