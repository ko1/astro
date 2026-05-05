# pystro stdlib `bisect` — binary search on a sorted list.

def bisect_left(a, x, lo=0, hi=None):
    if hi is None: hi = len(a)
    while lo < hi:
        mid = (lo + hi) // 2
        if a[mid] < x: lo = mid + 1
        else:          hi = mid
    return lo


def bisect_right(a, x, lo=0, hi=None):
    if hi is None: hi = len(a)
    while lo < hi:
        mid = (lo + hi) // 2
        if x < a[mid]: hi = mid
        else:          lo = mid + 1
    return lo


bisect = bisect_right


def insort_left(a, x, lo=0, hi=None):
    pos = bisect_left(a, x, lo, hi)
    a.insert(pos, x)


def insort_right(a, x, lo=0, hi=None):
    pos = bisect_right(a, x, lo, hi)
    a.insert(pos, x)


insort = insort_right
