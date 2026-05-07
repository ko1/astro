"""pystro stub for `_lsprof` (cProfile accelerator)."""


class Profiler:
    def __init__(self, *args, **kwargs):
        pass
    def enable(self, *a, **k): pass
    def disable(self): pass
    def clear(self): pass
    def getstats(self): return []


__all__ = ["Profiler"]
