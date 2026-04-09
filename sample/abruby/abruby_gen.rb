require 'astrogen'

class AbRubyNodeDef < ASTroGen::NodeDef
  register_gen_task :mark,
    func_typedef: "typedef void (*node_marker_func_t)(struct Node *n);",
    func_prefix: "MARKER_",
    kind_field: "node_marker_func_t marker"

  class Node < ASTroGen::NodeDef::Node
    class Operand < ASTroGen::NodeDef::Node::Operand
      def hash_call(val)
        return "0" if ref?
        case @type
        when 'ID'
          "hash_cstr(rb_id2name(#{val}))"
        else
          super
        end
      end

      def build_dumper(name)
        return nil if ref?
        case @type
        when 'ID'
          "        fprintf(fp, \"%s\", rb_id2name(n->u.#{name}.#{self.name}));"
        else
          super
        end
      end

      def build_specializer(name)
        if ref?
          # pass runtime address (not specialized)
          arg = "    fprintf(fp, \"        &n->u.#{name}.#{self.name}\");"
          return nil, arg
        end
        case @type
        when 'ID'
          arg = "    fprintf(fp, \"        n->u.#{name}.#{self.name}\");"
          return nil, arg
        else
          super
        end
      end
    end

    def result_type = "RESULT"

    def alloc_dispatcher_expr
      if no_inline?
        "DISPATCH_#{@name}"
      else
        "(OPTION.compiled_only ? NULL : DISPATCH_#{@name})"
      end
    end

    def build_marker
      node_ops = @operands.reject(&:ref?).select(&:node?)
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
