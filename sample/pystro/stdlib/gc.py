# pystro stub for `gc` (CPython's cyclic garbage collector).  pystro
# uses Boehm GC, not CPython's refcount + cycle collector — most APIs
# are no-ops or return reasonable defaults.

def enable(): pass
def disable(): pass
def isenabled(): return True
def collect(generation=2):
    try:
        __pystro_gc_collect__()
    except NameError:
        pass
    return 0
def get_count(): return (0, 0, 0)
def get_threshold(): return (700, 10, 10)
def set_threshold(*args): pass
def get_objects(generation=None): return []
def get_referrers(*args): return []
def get_referents(*args): return []
def is_tracked(obj): return False
def is_finalized(obj): return False
def freeze(): pass
def unfreeze(): pass
def get_freeze_count(): return 0
def set_debug(flags): pass
def get_debug(): return 0
def get_stats(): return [{"collections": 0, "collected": 0, "uncollectable": 0}]
def callbacks(): return []

DEBUG_STATS         = 1
DEBUG_COLLECTABLE   = 2
DEBUG_UNCOLLECTABLE = 4
DEBUG_SAVEALL       = 32
DEBUG_LEAK          = DEBUG_COLLECTABLE | DEBUG_UNCOLLECTABLE | DEBUG_SAVEALL

garbage = []

__all__ = ["enable", "disable", "isenabled", "collect",
           "get_count", "get_threshold", "set_threshold",
           "get_objects", "get_referrers", "get_referents",
           "is_tracked", "is_finalized", "freeze", "unfreeze",
           "get_freeze_count", "set_debug", "get_debug",
           "get_stats", "callbacks", "garbage",
           "DEBUG_STATS", "DEBUG_COLLECTABLE", "DEBUG_UNCOLLECTABLE",
           "DEBUG_SAVEALL", "DEBUG_LEAK"]
