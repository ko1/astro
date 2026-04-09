require 'astrogen'

class AbRubyNodeDef < ASTroGen::NodeDef
  register_gen_task :mark,
    func_typedef: "typedef void (*node_marker_func_t)(struct Node *n);",
    func_prefix: "MARKER_",
    kind_field: "node_marker_func_t marker"

  class Node < ASTroGen::NodeDef::Node
    def result_type = "RESULT"

    def alloc_dispatcher_expr
      if no_inline?
        "DISPATCH_#{@name}"
      else
        "(OPTION.compiled_only ? NULL : DISPATCH_#{@name})"
      end
    end

    def build_marker
      node_ops = @operands.select(&:node?)
      marks = node_ops.map { |op| "    MARK(n->u.#{@name}.#{op.name});" }
      <<~C
      static void
      MARKER_#{@name}(NODE *n)
      {
      #{marks.join("\n")}
      }
      C
    end
  end

  def build_mark
    <<~C
    // This file is auto-generated from #{@file}.
    // GC mark functions

    #{@nodes.map{|name, n| n.build_marker}.join("\n")}
    C
  end
end
