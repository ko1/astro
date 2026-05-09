# __future__ stub: pystro already supports Python 3.12 syntax baseline.
# Each feature is a no-op object.
class _Feature:
    def __init__(self, name): self.name = name

annotations = _Feature("annotations")
division = _Feature("division")
print_function = _Feature("print_function")
absolute_import = _Feature("absolute_import")
unicode_literals = _Feature("unicode_literals")
generator_stop = _Feature("generator_stop")
nested_scopes = _Feature("nested_scopes")
generators = _Feature("generators")
with_statement = _Feature("with_statement")
braces = _Feature("braces")
all_feature_names = ["annotations", "division", "print_function",
                     "absolute_import", "unicode_literals", "generator_stop"]
