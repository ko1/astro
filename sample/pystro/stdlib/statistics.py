# pystro stdlib `statistics` (minimal).

import math as _math


def mean(data):
    data = list(data)
    if not data:
        raise StatisticsError("mean requires at least one data point")
    return sum(data) / len(data)


def fmean(data):
    return mean(data)


def median(data):
    data = sorted(data)
    n = len(data)
    if n == 0:
        raise StatisticsError("no median for empty data")
    if n % 2 == 1:
        return data[n // 2]
    return (data[n // 2 - 1] + data[n // 2]) / 2


def median_low(data):
    data = sorted(data)
    n = len(data)
    if n == 0:
        raise StatisticsError("empty")
    return data[(n - 1) // 2]


def median_high(data):
    data = sorted(data)
    n = len(data)
    if n == 0:
        raise StatisticsError("empty")
    return data[n // 2]


def mode(data):
    counts = {}
    for x in data:
        counts[x] = counts.get(x, 0) + 1
    best, best_count = None, -1
    for x, c in counts.items():
        if c > best_count:
            best, best_count = x, c
    if best is None:
        raise StatisticsError("no mode for empty data")
    return best


def variance(data):
    data = list(data)
    n = len(data)
    if n < 2:
        raise StatisticsError("variance requires at least two data points")
    m = mean(data)
    return sum((x - m) ** 2 for x in data) / (n - 1)


def pvariance(data):
    data = list(data)
    n = len(data)
    if n < 1:
        raise StatisticsError("pvariance requires at least one data point")
    m = mean(data)
    return sum((x - m) ** 2 for x in data) / n


def stdev(data):
    return _math.sqrt(variance(data))


def pstdev(data):
    return _math.sqrt(pvariance(data))


def quantiles(data, *, n=4):
    data = sorted(data)
    L = len(data)
    if L < 2:
        raise StatisticsError("quantiles requires at least two data points")
    out = []
    for i in range(1, n):
        h = (L - 1) * i / n
        lo = int(h)
        hi = lo + 1 if lo + 1 < L else lo
        frac = h - lo
        out.append(data[lo] + (data[hi] - data[lo]) * frac)
    return out


class StatisticsError(ValueError):
    pass


def multimode(data):
    counts = {}
    for x in data:
        counts[x] = counts.get(x, 0) + 1
    if not counts:
        return []
    mx = max(counts.values())
    return [x for x, c in counts.items() if c == mx]


def harmonic_mean(data):
    data = list(data)
    n = len(data)
    if n == 0:
        raise StatisticsError("harmonic_mean requires at least one data point")
    s = 0.0
    for x in data:
        if x <= 0:
            raise StatisticsError("harmonic_mean requires positive numbers")
        s += 1.0 / x
    return n / s


def geometric_mean(data):
    data = list(data)
    n = len(data)
    if n == 0:
        raise StatisticsError("geometric_mean requires at least one data point")
    p = 1.0
    for x in data:
        if x <= 0:
            raise StatisticsError("geometric_mean requires positive numbers")
        p *= x
    return p ** (1.0 / n)


__all__ = ["mean", "fmean", "median", "median_low", "median_high",
           "mode", "multimode", "variance", "pvariance", "stdev", "pstdev",
           "quantiles", "StatisticsError",
           "harmonic_mean", "geometric_mean"]
