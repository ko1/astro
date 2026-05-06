"""pystro stub for `_opcode` (CPython bytecode introspection)."""

stack_effect = lambda *a, **k: 0
get_executor = lambda *a, **k: None
get_specialization_stats = lambda: {}

# A few commonly-referenced opcode names.
EXTENDED_ARG = 144
NOP = 9
RESUME = 151

def get_opmap():
    return {}

def get_metadata():
    return []
