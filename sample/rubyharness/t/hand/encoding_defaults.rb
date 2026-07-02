# Encoding.default_external/internal getters + setters. vs ruby.
p Encoding.default_external.class
old_i = Encoding.default_internal
p old_i
Encoding.default_internal = Encoding::UTF_8
p Encoding.default_internal.name
Encoding.default_internal = old_i
p Encoding.default_internal
oe = Encoding.default_external
Encoding.default_external = Encoding::US_ASCII
p Encoding.default_external.name
Encoding.default_external = oe
