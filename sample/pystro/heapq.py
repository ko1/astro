# pystro stdlib `heapq` — binary min-heap on a list.

def heappush(heap, item):
    heap.append(item)
    _siftdown(heap, 0, len(heap) - 1)


def heappop(heap):
    last = heap.pop()
    if heap:
        result = heap[0]
        heap[0] = last
        _siftup(heap, 0)
        return result
    return last


def heappushpop(heap, item):
    if heap and heap[0] < item:
        tmp = heap[0]
        heap[0] = item
        item = tmp
        _siftup(heap, 0)
    return item


def heapreplace(heap, item):
    result = heap[0]
    heap[0] = item
    _siftup(heap, 0)
    return result


def heapify(x):
    n = len(x)
    for i in range(n // 2, -1, -1):
        _siftup(x, i)


def nlargest(n, iterable, key=None):
    items = list(iterable)
    if key:
        items.sort(key=key, reverse=True)
    else:
        items.sort(reverse=True)
    return items[:n]


def nsmallest(n, iterable, key=None):
    items = list(iterable)
    if key:
        items.sort(key=key)
    else:
        items.sort()
    return items[:n]


def merge(*iterables, key=None, reverse=False):
    iters = [iter(it) for it in iterables]
    items = []
    for i, it in enumerate(iters):
        try:
            items.append((next(it), i, it))
        except StopIteration:
            pass
    if reverse:
        items.sort(reverse=True, key=lambda x: x[0] if key is None else key(x[0]))
    else:
        items.sort(key=lambda x: x[0] if key is None else key(x[0]))
    while items:
        v, i, it = items[0]
        yield v
        try:
            nv = next(it)
            items[0] = (nv, i, it)
            items.sort(key=lambda x: x[0] if key is None else key(x[0]),
                       reverse=reverse)
        except StopIteration:
            items.pop(0)


def _siftdown(heap, startpos, pos):
    item = heap[pos]
    while pos > startpos:
        parent_pos = (pos - 1) >> 1
        parent = heap[parent_pos]
        if item < parent:
            heap[pos] = parent
            pos = parent_pos
            continue
        break
    heap[pos] = item


def _siftup(heap, pos):
    end = len(heap)
    startpos = pos
    item = heap[pos]
    childpos = 2 * pos + 1
    while childpos < end:
        rightpos = childpos + 1
        if rightpos < end and not (heap[childpos] < heap[rightpos]):
            childpos = rightpos
        heap[pos] = heap[childpos]
        pos = childpos
        childpos = 2 * pos + 1
    heap[pos] = item
    _siftdown(heap, startpos, pos)
