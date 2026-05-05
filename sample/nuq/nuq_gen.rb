require 'astrogen'

# nuq's NODE_DEF dispatchers return EMIT (= a slice of c->pool), not
# VALUE.  This avoids allocating a fresh nuq_array per filter call —
# emits are appended to a per-process growable pool, and the result is
# (start_ptr, count).  Callers reset c->pool_top after consuming.
class NuqNodeDef < ASTroGen::NodeDef
  class Node < ASTroGen::NodeDef::Node
    def result_type = "EMIT"
  end
end
