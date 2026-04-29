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
        when 'struct callcache *', 'struct func_addr_cache *'
          # Inline caches are runtime memoisation slots — they don't
          # contribute to the structural identity of the node, so the
          # hash sees them as constant.
          '0'
        else
          super
        end
      end

      def build_dumper(name)
        case @type
        when 'struct callcache *'
          "        fprintf(fp, \"<cc>\");"
        when 'struct func_addr_cache *'
          "        fprintf(fp, \"<fac>\");"
        else
          super
        end
      end

      def build_specializer(name)
        case @type
        when 'struct callcache *', 'struct func_addr_cache *'
          # When @ref, the cache lives inline in the node struct; pass
          # its address so the EVAL signature (which still wants a
          # pointer) gets a usable value.  Without @ref, the field is
          # already a pointer.
          ref = ref? ? '&' : ''
          arg = "    fprintf(fp, \"        #{ref}n->u.#{name}.#{self.name}\");"
          return nil, arg
        when 'uint64_t'
          # The framework default emits `(VALUE)NN` which assumes
          # VALUE is a scalar typedef (e.g. wastro's `typedef uint64_t
          # VALUE`).  In castro VALUE is a tagged union, so the cast
          # is invalid; the EVAL parameter is `uint64_t` anyway, so a
          # plain numeric literal is the right thing.
          arg = "    fprintf(fp, \"        %lluULL\", (unsigned long long)n->u.#{name}.#{self.name});"
          return nil, arg
        else
          super
        end
      end
    end
  end
end
