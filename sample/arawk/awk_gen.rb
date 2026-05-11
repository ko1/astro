require 'astrogen'

# arawk-specific operand overrides — minimal; we don't yet have node-
# embedded callcaches or other custom operand types.  Inherits the
# framework defaults for int32_t / uint32_t / NODE * / const char *.
class AwkNodeDef < ASTroGen::NodeDef
  class Node < ASTroGen::NodeDef::Node
    # Three-arg dispatcher (CTX *, NODE *, VALUE *fp), matching astr.
    def common_param_count
      3
    end

    # All dispatchers return RESULT (VALUE + state) so `next` / `exit`
    # propagate via non-NORMAL state instead of needing setjmp.
    def result_type
      "RESULT"
    end

    class Operand < ASTroGen::NodeDef::Node::Operand
      def build_dumper(name)
        case @type
        when 'uint64_t'
          # node_float stores a double's bit pattern — render the
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
    end
  end
end
