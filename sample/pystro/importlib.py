# pystro stdlib `importlib` (minimal).

def import_module(name):
    return __import__(name)


__all__ = ["import_module"]
