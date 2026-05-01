require 'astrogen'

# Per-language ASTroGen extension for astocaml.  Same pattern as ascheme:
# `@ref` operands of host-defined struct types (the `gref_cache` inline
# cache) need explicit handling because astrogen.rb only knows primitive
# types.
class AstocamlNodeDef < ASTroGen::NodeDef
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
