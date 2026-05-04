require 'astrogen'

# astr-specific operand handling.  The base framework supports
# int32_t / uint32_t / uint64_t / NODE * / const char * / double / void *.
# We add `struct function_entry *` for the per-callsite function-table
# slot pointer (used as @ref so it's stored inline in the NODE union).
class AstrNodeDef < ASTroGen::NodeDef
  class Node < ASTroGen::NodeDef::Node
    # Three-arg dispatcher (CTX *c, NODE *n, VALUE *fp), matching naruby /
    # castro: explicit fp keeps the callee frame in a register through
    # the recursive SD chain.
    def common_param_count
      3
    end

    # All dispatchers return RESULT (= VALUE + state) so `return`
    # propagates as non-NORMAL state instead of needing setjmp.
    def result_type
      "RESULT"
    end

    class Operand < ASTroGen::NodeDef::Node::Operand
      def hash_call(val, kind: :horg)
        case @type
        when 'struct astr_callcache *'
          # The cache is per-process state determined by call-site
          # location, not by structure.  Hash to a constant so two
          # structurally-equivalent call sites hash identically.
          '0ULL'
        else
          super
        end
      end

      def build_dumper(name)
        case @type
        when 'struct astr_callcache *'
          "        fputs(\"<cc>\", fp);"
        when 'uint64_t'
          # node_num stores a double's bit pattern — render the actual
          # numeric value rather than the opaque uint64 so AST dumps
          # are readable.
          if self.name == 'bits'
            "        { union { uint64_t u; double d; } _pun = { .u = n->u.#{name}.#{self.name} }; fprintf(fp, \"%g\", _pun.d); }"
          else
            super
          end
        else
          super
        end
      end

      def build_specializer(name)
        case @type
        when 'struct astr_callcache *'
          # @ref: the framework stores the struct inline; the SD must
          # pass `&n->u.X.cc` so cache writes hit the inline slot.
          arg = if ref?
                  "    fprintf(fp, \"        &n->u.#{name}.#{self.name}\");"
                else
                  "    fprintf(fp, \"        n->u.#{name}.#{self.name}\");"
                end
          return nil, arg
        else
          super
        end
      end
    end
  end
end
