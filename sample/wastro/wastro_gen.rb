require 'astrogen'

# Wastro overrides ASTroGen's default `VALUE` return type for node
# evaluators to a small `{ VALUE value; uint32_t br_depth; }` struct
# that fits in two registers (rax+rdx on SysV x86_64).  Branch state
# (`br` / `return` / `unreachable`) is carried in the return register
# pair instead of a CTX field, eliminating per-iteration memory traffic
# in tight loops.  See context.h for the RESULT struct definition and
# the UNWRAP macro that propagates a non-normal br_depth.
class WastroNodeDef < ASTroGen::NodeDef
  class Node < ASTroGen::NodeDef::Node
    def result_type = "RESULT"

    # Wastro lifts wasm locals out of the operand stack into a caller-
    # allocated `VALUE F[local_cnt]` (or per-function typed struct) and
    # passes its address through the dispatcher chain so gcc can SROA
    # the locals into registers in the inner loop.  Every NODE_DEF body
    # gets `void *frame` in scope as a hidden parameter; lifted local
    # ops cast it to a slot array.  See node.def for the local-op nodes
    # and node_call_N where the frame is allocated.
    def extra_prefix_args
      ["void *frame"]
    end
  end
end
