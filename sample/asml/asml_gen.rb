require 'astrogen'

# `@ref` operands whose type is a host-defined struct (gref_cache,
# app_cache) need the same custom handling as ascheme/astocaml: they're
# excluded from the structural hash and emitted as `&n->u.kind.name`
# in specialised code.
class AsmlNodeDef < ASTroGen::NodeDef
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
