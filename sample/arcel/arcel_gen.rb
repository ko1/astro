require 'astrogen'

# arcel uses a tagged-union struct for VALUE (16 bytes, see context.h).
# ASTroGen's default SD specializer for `uint64_t` operands wraps the
# literal in a `(VALUE)` cast — fine when VALUE is `int64_t`/`intptr_t`
# (calc, koruby, abruby, ...), but a compile error here because you
# can't cast an integer to a struct type:
#
#   c/SD_xxx.c:14:5: error: conversion to non-scalar type requested
#       return EVAL_node_uint_lit(c, n,
#           (VALUE)25ULL);
#
# Drop the cast — the `EVAL_node_xxx` signature already declares the
# operand as `uint64_t`, so the implicit conversion at the call site
# does the right thing.
#
# (Same fix would apply to `double` operands stored in struct-VALUE
# samples; arcel's `node_double_lit` doesn't trip ASTroGen's default
# because the default for `double` is `(VALUE)%g` — wait no, looking
# again, double's emitter prints `        %.17g` without a cast, so
# only uint64_t is affected.)
class ASTroGen::NodeDef::Node::Operand
  alias_method :build_specializer_orig_arcel, :build_specializer
  def build_specializer(name)
    case @type
    when 'uint64_t'
      # ASTroGen's default wraps with `(VALUE)` cast — fine when VALUE
      # is intptr_t-like (calc, koruby, ...), broken when VALUE is a
      # struct (arcel uses a 16-byte tagged union).  Drop the cast.
      arg = "    fprintf(fp, \"        %lluULL\", (unsigned long long)n->u.#{name}.#{self.name});"
      return nil, arg
    when 'const char *'
      # Convention: each `const char *` operand named `<x>` is paired
      # with a `uint32_t` operand named `<x>_len`.  Emit the blob as a
      # length-aware, `\xHH`-escaped C literal via arcel_fprint_blob_lit
      # (defined in node.c).  This keeps binary-safe content (CEL
      # `'\000\xff'` etc.) intact while still letting gcc constant-fold
      # the resulting memcmp / hash compare in the inlined SD body.
      arg = "    arcel_fprint_blob_lit(fp, n->u.#{name}.#{self.name}, n->u.#{name}.#{self.name}_len);"
      return nil, arg
    end
    build_specializer_orig_arcel(name)
  end
end
