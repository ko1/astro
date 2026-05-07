require 'astrogen'

# pystro's `@ref` operands are inline cache structs (struct gref_cache *).
# Same handling as ascheme: contribute "0" to structural hash, skip the
# dump line, and emit `&n->u.<kind>.<field>` in the specializer so the
# SD function gets a stable address into the original NODE's cache slot.
class PystroNodeDef < ASTroGen::NodeDef
  class Node < ASTroGen::NodeDef::Node
    class Operand < ASTroGen::NodeDef::Node::Operand
      def hash_call(val, kind: :horg)
        return "0" if ref?
        super
      end

      def build_dumper(name)
        return nil if ref?
        super
      end

      def build_specializer(name)
        if ref?
          arg = "    fprintf(fp, \"        &n->u.#{name}.#{self.name}\");"
          return nil, arg
        end
        # Override `const char *` to reference NODE's stored field instead
        # of emitting a C string literal.  The literal would land in the
        # SD's .rodata and be a different pointer from the parser's
        # `intern_name` pool — pointer-compare-first lookups
        # (py_class_lookup_method) would all miss.  Sharing the NODE
        # pointer means SD passes the interned pointer and lookups can
        # short-circuit strcmp.
        if @type == 'const char *'
          arg = "    fprintf(fp, \"        n->u.#{name}.#{self.name}\");"
          return nil, arg
        end
        super
      end
    end
  end
end
