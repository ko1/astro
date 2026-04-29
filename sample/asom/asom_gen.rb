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
        when 'VALUE'
          # `VALUE cached` is a parse-time cache (e.g., the
          # asom_string built once for a string literal). Its bits
          # are a heap pointer that varies per-process so it must be
          # excluded from the structural hash; surrogate content
          # fields (bytes / len) carry the deterministic hash.
          '0'
        else
          super
        end
      end

      def build_dumper(name)
        case @type
        when 'struct asom_callcache *'
          "        fprintf(fp, \"<cc>\");"
        when 'VALUE'
          "        fprintf(fp, \"<value>\");"
        else
          super
        end
      end

      def build_specializer(name)
        case @type
        when 'struct asom_callcache *'
          arg = "    fprintf(fp, \"        n->u.#{name}.#{self.name}\");"
          return nil, arg
        when 'VALUE'
          # SD-baked code reads the cached VALUE back via the struct
          # field — a fresh process re-parses the source, populates
          # the cache, and the SD just reads it.
          arg = "    fprintf(fp, \"        n->u.#{name}.#{self.name}\");"
          return nil, arg
        else
          super
        end
      end
    end
  end
end
