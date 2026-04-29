require 'astrogen'

# Adds support for `struct callcache *` operands (used by node_call)
# to ASTroGen's hash/dump/specialize hooks.  callcache is a runtime
# inline cache slot — not part of the structural identity, so its
# hash contribution is 0 and SPECIALIZE rebuilds it as-is.
class CastroNodeDef < ASTroGen::NodeDef
  class Node < ASTroGen::NodeDef::Node
    class Operand < ASTroGen::NodeDef::Node::Operand
      def hash_call(val, kind: :horg)
        case @type
        when 'struct callcache *'
          '0'
        else
          super
        end
      end

      def build_dumper(name)
        case @type
        when 'struct callcache *'
          "        fprintf(fp, \"<cc>\");"
        else
          super
        end
      end

      def build_specializer(name)
        case @type
        when 'struct callcache *'
          arg = "    fprintf(fp, \"        n->u.#{name}.#{self.name}\");"
          return nil, arg
        else
          super
        end
      end
    end
  end
end
