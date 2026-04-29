require 'astrogen'

# ASTroGen subclass that teaches the generator about asom-specific operand
# types (currently just `struct asom_callcache *`, which we want to store by
# pointer, leave out of the Merkle hash, and dump as "<cc>").

class ASOMNodeDef < ASTroGen::NodeDef
  class Node < ASTroGen::NodeDef::Node
    class Operand < ASTroGen::NodeDef::Node::Operand
      def hash_call(val, kind: :horg)
        case @type
        when 'struct asom_callcache *'
          '0'
        else
          super
        end
      end

      def build_dumper(name)
        case @type
        when 'struct asom_callcache *'
          "        fprintf(fp, \"<cc>\");"
        else
          super
        end
      end

      def build_specializer(name)
        case @type
        when 'struct asom_callcache *'
          arg = "    fprintf(fp, \"        n->u.#{name}.#{self.name}\");"
          return nil, arg
        else
          super
        end
      end
    end
  end
end
