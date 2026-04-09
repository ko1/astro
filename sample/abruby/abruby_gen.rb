require 'astrogen'

class AbRubyNodeDef < ASTroGen::NodeDef
  register_gen_task :mark,
    func_typedef: "typedef void (*node_marker_func_t)(struct Node *n);",
    func_prefix: "MARKER_",
    kind_field: "node_marker_func_t marker"

  class Node < ASTroGen::NodeDef::Node
    class Operand < ASTroGen::NodeDef::Node::Operand
      def hash_call(val)
        case @type
        when 'ID'
          "(#{val} ? hash_cstr(rb_id2name(#{val})) : hash_uint32(0))"
        else
          super
        end
      end

      def build_dumper(name)
        case @type
        when 'ID'
          "        fprintf(fp, \"%s\", #{id_to_name("n->u.#{name}.#{self.name}")});"
        else
          super
        end
      end

      def build_specializer(name)
        case @type
        when 'ID'
          field = "n->u.#{name}.#{self.name}"
          arg = "    if (#{field}) fprintf(fp, \"        rb_intern(\\\"%s\\\")\", rb_id2name(#{field}));\n" \
                "    else fprintf(fp, \"        (ID)0\");"
          return nil, arg
        else
          super
        end
      end

      private

      def id_to_name(expr)
        "(#{expr} ? rb_id2name(#{expr}) : \"\")"
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
