require 'astrogen'

# Castro-specific override of the framework specializer for
# `uint64_t` operands.  The default emits `(VALUE)<lit>ULL`, which
# assumes VALUE is a scalar typedef (e.g. wastro's
# `typedef uint64_t VALUE`).  In castro VALUE is a tagged union, so
# the cast is invalid; the EVAL parameter is `uint64_t` anyway, so a
# plain numeric literal is the right thing.
class CastroNodeDef < ASTroGen::NodeDef
  class Node < ASTroGen::NodeDef::Node
    class Operand < ASTroGen::NodeDef::Node::Operand
      def build_specializer(name)
        case @type
        when 'uint64_t'
          arg = "    fprintf(fp, \"        %lluULL\", (unsigned long long)n->u.#{name}.#{self.name});"
          return nil, arg
        else
          super
        end
      end
    end
  end
end
