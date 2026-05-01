require 'astrogen'

# Per-language ASTroGen extension for pascalast.  Tells the default
# generators how to deal with `@ref` cache operands (struct types
# the framework can't introspect).  See ascheme_gen.rb for the same
# pattern used in the Scheme sample.
class PascalastNodeDef < ASTroGen::NodeDef
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
