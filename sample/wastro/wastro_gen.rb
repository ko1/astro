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
  end
end
