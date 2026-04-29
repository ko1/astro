require 'astrogen'

# Per-language ASTroGen extension for ascheme.  All we customize is
# how `@ref` operands of host-defined struct types are treated by the
# default code generators — astrogen.rb only knows about primitive
# types (uint32_t, NODE*, const char*, double), so the inline-cache
# fields like `struct gref_cache *cache @ref` need explicit handling.
#
#   - hash:        @ref fields are runtime caches, not part of structural
#                  identity; we contribute "0" to the Merkle hash so two
#                  nodes that differ only in cache contents collapse.
#   - dump:        skip — caches are noise in pretty-printed AST.
#   - specialize:  emit `&n->u.<kind>.<field>` so the SD function gets
#                  a stable address into the original node's cache slot
#                  (the cache state survives across SD invocations).
class AschemeNodeDef < ASTroGen::NodeDef
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
        super
      end
    end
  end
end
