"""pystro stub for _testcapi (CPython internal C-API tests)."""
import sys
INT_MAX = (1 << 31) - 1
LONG_MAX = (1 << 63) - 1
LLONG_MAX = (1 << 63) - 1
SIZE_MAX = (1 << 63) - 1
PYTHON_VERSION_HEX = sys.hexversion if hasattr(sys, "hexversion") else 0x030C0000
class error(Exception): pass

def get_recursion_depth(): return 0
def set_recursion_limit(n): return None
def py_buildvalue(*a, **kw): return None
def py_buildvalue_ints(*a, **kw): return None

class HeapCType: pass
class HeapCTypeSubclass(HeapCType): pass
class HeapCTypeWithDict(HeapCType): pass
class HeapCTypeSetattr(HeapCType): pass
class HeapDocCType: pass
class HeapGcCType: pass

__all__ = ["error", "INT_MAX", "LONG_MAX", "LLONG_MAX", "SIZE_MAX",
           "get_recursion_depth", "set_recursion_limit"]

