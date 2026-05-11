"""pystro stub for the `_multiprocessing` C extension."""

class SemLock:
    SEM_VALUE_MAX = 32768
    def __init__(self, *args, **kwargs): pass


def sem_unlink(name): pass


__all__ = ["SemLock", "sem_unlink"]
