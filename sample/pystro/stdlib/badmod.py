# Used by test/30_import_raise.py — module init raises.
def helper():
    return 42
raise ValueError("badmod init failed")
