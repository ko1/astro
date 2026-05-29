# In-place quicksort over a list of ints.
xs:[int] = None
i:int = 0

def qsort(a:[int], lo:int, hi:int) -> object:
    pivot:int = 0
    i:int = 0
    j:int = 0
    t:int = 0
    if lo >= hi:
        return None
    pivot = a[hi]
    i = lo
    j = lo
    while j < hi:
        if a[j] < pivot:
            t = a[i]
            a[i] = a[j]
            a[j] = t
            i = i + 1
        j = j + 1
    t = a[i]
    a[i] = a[hi]
    a[hi] = t
    qsort(a, lo, i - 1)
    qsort(a, i + 1, hi)
    return None

xs = [5, 2, 9, 1, 5, 6, 3, 8, 7, 0, 4]
qsort(xs, 0, len(xs) - 1)
i = 0
while i < len(xs):
    print(xs[i])
    i = i + 1
