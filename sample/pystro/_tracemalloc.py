"""pystro stub for `_tracemalloc`."""


def start(nframe=1): pass
def stop(): pass
def is_tracing(): return False
def get_traceback_limit(): return 1
def get_tracemalloc_memory(): return 0
def get_traced_memory(): return (0, 0)
def reset_peak(): pass
def clear_traces(): pass
def _get_traces(): return []
def _get_object_traceback(obj): return None


__all__ = ["start", "stop", "is_tracing", "get_traceback_limit",
           "get_tracemalloc_memory", "get_traced_memory", "reset_peak",
           "clear_traces"]
