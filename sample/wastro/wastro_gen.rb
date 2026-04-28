require 'astrogen'

# Wastro's per-language ASTroGen extension.  Two changes from the
# default:
#
#   1. Return type is RESULT (defined in context.h) — a 12-byte
#      `{ VALUE value; uint32_t br_depth; }` that fits in rax+rdx so
#      branch state never hits memory.
#
#   2. Common parameter count is 3 — every NODE_DEF takes
#      `(CTX *c, NODE *n, union wastro_slot *frame, ...)`.  The third
#      parameter is the per-function-call wasm-locals slot array,
#      caller-allocated by node_call_N (and by wastro_invoke at the
#      module boundary).  Lifted local ops read/write `frame[i].<type>`
#      directly, so gcc gets typed-field information at the access site
#      and can SROA each slot into a register when the SD chain inlines.
class WastroNodeDef < ASTroGen::NodeDef
  class Node < ASTroGen::NodeDef::Node
    def result_type = "RESULT"

    # Add `union wastro_slot *frame` to the common parameter list.
    # See node.def — every NODE_DEF body writes the parameter visibly
    # (no hidden injection); the framework just knows to thread three
    # named params through the dispatch chain instead of two.
    def common_param_count
      3
    end
  end
end
