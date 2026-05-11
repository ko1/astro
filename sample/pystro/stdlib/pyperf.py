"""pystro stub for `pyperf` (CPython micro-benchmark harness).

The full library is not implemented; just enough surface so that
`if __name__ == '__main__': import pyperf` inside CPython tests doesn't
explode at import time."""

from time import perf_counter


class Runner:
    def __init__(self, **kwargs): pass
    def parse_args(self, *args, **kwargs):
        class _Args: pass
        return _Args()
    def bench_func(self, name, fn, *args, **kwargs):
        return fn(*args)
    def bench_time_func(self, name, fn, *args, **kwargs):
        # Real pyperf passes a "loops" int; CPython tests use it as a
        # repeat count.  1 keeps tests cheap and avoids unbounded loops.
        return fn(1)
    def timeit(self, *args, **kwargs):
        return None


__all__ = ["Runner", "perf_counter"]
