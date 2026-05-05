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


__all__ = ["seed", "random", "randint", "choice", "shuffle", "sample", "uniform"]
