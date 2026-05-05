# pystro stdlib `itertools` (minimal).

def chain(*iterables):
    for it in iterables:
        for x in it:
            yield x


def _chain_from_iterable(iterable):
    for it in iterable:
        for x in it:
            yield x

# Attach as method-style attribute.  Pystro doesn't have classmethod
# attached to functions, so we install a callable attribute via
# `chain.from_iterable = ...`.  CPython does this with a real
# classmethod on the chain class.
chain.from_iterable = _chain_from_iterable


def count(start=0, step=1):
    n = start
    while True:
        yield n
        n += step


def repeat(value, times=None):
    if times is None:
        while True:
            yield value
    else:
        for _ in range(times):
            yield value


def cycle(iterable):
    saved = []
    for x in iterable:
        yield x
        saved.append(x)
    while saved:
        for x in saved:
            yield x


def islice(iterable, *args):
    if len(args) == 1:
        start, stop, step = 0, args[0], 1
    elif len(args) == 2:
        start, stop, step = args[0], args[1], 1
    elif len(args) == 3:
        start, stop, step = args[0], args[1], args[2]
    else:
        raise TypeError("islice expects 1-3 args")
    if start is None: start = 0
    if step is None: step = 1
    i = 0
    next_emit = start
    for x in iterable:
        if stop is not None and i >= stop:
            break
        if i >= start and (i - start) % step == 0:
            yield x
        i += 1


def takewhile(pred, iterable):
    for x in iterable:
        if not pred(x):
            return
        yield x


def dropwhile(pred, iterable):
    started = False
    for x in iterable:
        if not started and pred(x):
            continue
        started = True
        yield x


def accumulate(iterable, func=None):
    total = None
    started = False
    for x in iterable:
        if not started:
            total = x
            started = True
        else:
            total = (total + x) if func is None else func(total, x)
        yield total


def product(*iterables):
    pools = [list(it) for it in iterables]
    if not pools:
        yield ()
        return
    result = [[]]
    for pool in pools:
        new_result = []
        for prefix in result:
            for v in pool:
                new_result.append(prefix + [v])
        result = new_result
    for combo in result:
        yield tuple(combo)


def combinations(iterable, r):
    pool = list(iterable)
    n = len(pool)
    if r > n:
        return
    indices = list(range(r))
    yield tuple(pool[i] for i in indices)
    while True:
        i = r - 1
        while i >= 0 and indices[i] == i + n - r:
            i -= 1
        if i < 0:
            return
        indices[i] += 1
        for j in range(i+1, r):
            indices[j] = indices[j-1] + 1
        yield tuple(pool[k] for k in indices)


def combinations_with_replacement(iterable, r):
    pool = list(iterable)
    n = len(pool)
    if not n and r:
        return
    indices = [0] * r
    yield tuple(pool[i] for i in indices)
    while True:
        i = r - 1
        while i >= 0 and indices[i] == n - 1:
            i -= 1
        if i < 0:
            return
        v = indices[i] + 1
        for j in range(i, r):
            indices[j] = v
        yield tuple(pool[k] for k in indices)


def compress(data, selectors):
    for d, s in zip(data, selectors):
        if s:
            yield d


def filterfalse(pred, iterable):
    if pred is None:
        for x in iterable:
            if not x:
                yield x
    else:
        for x in iterable:
            if not pred(x):
                yield x


def starmap(func, iterable):
    for args in iterable:
        yield func(*args)


def zip_longest(*iterables, fillvalue=None):
    iters = [iter(it) for it in iterables]
    n = len(iters)
    if n == 0:
        return
    while True:
        result = []
        active = False
        for i, it in enumerate(iters):
            try:
                result.append(next(it))
                active = True
            except StopIteration:
                result.append(fillvalue)
        if not active:
            return
        yield tuple(result)


def groupby(iterable, key=None):
    if key is None:
        key = lambda x: x
    it = iter(iterable)
    try:
        cur = next(it)
    except StopIteration:
        return
    cur_key = key(cur)
    group = [cur]
    for x in it:
        k = key(x)
        if k == cur_key:
            group.append(x)
        else:
            yield (cur_key, iter(group))
            cur_key = k
            group = [x]
    yield (cur_key, iter(group))


def tee(iterable, n=2):
    items = list(iterable)
    return tuple(iter(list(items)) for _ in range(n))


def pairwise(iterable):
    it = iter(iterable)
    try:
        prev = next(it)
    except StopIteration:
        return
    for x in it:
        yield (prev, x)
        prev = x


def permutations(iterable, r=None):
    pool = list(iterable)
    n = len(pool)
    if r is None:
        r = n
    if r > n:
        return
    indices = list(range(n))
    cycles = list(range(n, n-r, -1))
    yield tuple(pool[i] for i in indices[:r])
    while n:
        i = r - 1
        done = False
        while i >= 0:
            cycles[i] -= 1
            if cycles[i] == 0:
                indices[i:] = indices[i+1:] + indices[i:i+1]
                cycles[i] = n - i
                i -= 1
            else:
                j = cycles[i]
                tmp = indices[i]
                indices[i] = indices[-j]
                indices[-j] = tmp
                yield tuple(pool[k] for k in indices[:r])
                done = True
                break
        if not done:
            return


__all__ = [
    "chain", "count", "repeat", "cycle", "islice",
    "takewhile", "dropwhile", "accumulate",
    "product", "combinations", "combinations_with_replacement",
    "permutations", "compress", "filterfalse", "starmap",
    "zip_longest", "groupby", "tee", "pairwise",
]
