# pystro stdlib `random` (minimal, xorshift64-based).

# Mutable seed.  Initialised from time.
import time as _time
_state = [int(_time.time() * 1e6) | 1]


def seed(s):
    _state[0] = s | 1


def _next64():
    x = _state[0]
    x ^= (x << 13) & 0xFFFFFFFFFFFFFFFF
    x ^= (x >> 7)
    x ^= (x << 17) & 0xFFFFFFFFFFFFFFFF
    _state[0] = x
    return x


def random():
    # Float in [0, 1).
    return (_next64() & 0xFFFFFFFFFFFFFF) / float(0x100000000000000)


def randint(a, b):
    # Inclusive on both ends.
    if b < a:
        raise ValueError("empty range")
    span = b - a + 1
    return a + (_next64() % span)


def randrange(start, stop=None, step=1):
    # Half-open like range(): [start, stop).
    if stop is None:
        start, stop = 0, start
    if step == 1:
        if stop <= start:
            raise ValueError("empty range")
        return start + (_next64() % (stop - start))
    width = stop - start
    if step > 0:
        n = (width + step - 1) // step
    else:
        n = (width + step + 1) // step
    if n <= 0:
        raise ValueError("empty range")
    return start + step * (_next64() % n)


def randbytes(n):
    return bytes((_next64() & 0xff) for _ in range(n))


def choice(seq):
    if len(seq) == 0:
        raise IndexError("choice from empty sequence")
    return seq[randint(0, len(seq) - 1)]


def shuffle(seq):
    n = len(seq)
    for i in range(n - 1, 0, -1):
        j = randint(0, i)
        tmp = seq[i]
        seq[i] = seq[j]
        seq[j] = tmp


def sample(population, k):
    if k > len(population):
        raise ValueError("sample larger than population")
    pool = list(population)
    shuffle(pool)
    return pool[:k]


def uniform(a, b):
    return a + (b - a) * random()


def gauss(mu=0.0, sigma=1.0):
    # Box-Muller: two uniforms → one gaussian (we waste one).
    import math as _math
    u1 = random() or 1e-10
    u2 = random()
    z = _math.sqrt(-2 * _math.log(u1)) * _math.cos(2 * _math.pi * u2)
    return mu + sigma * z


def choices(population, weights=None, k=1):
    out = []
    n = len(population)
    if weights is None:
        for _ in range(k):
            out.append(population[randint(0, n - 1)])
        return out
    total = 0
    cum = []
    for w in weights:
        total += w
        cum.append(total)
    for _ in range(k):
        x = random() * total
        # linear scan (binary search would be better)
        for i, c in enumerate(cum):
            if x < c:
                out.append(population[i])
                break
        else:
            out.append(population[-1])
    return out


__all__ = ["seed", "random", "randint", "randrange", "randbytes",
           "choice", "choices", "shuffle", "sample", "uniform", "gauss"]
