require 'astrogen'

# ASTroGen subclass for arjsv:
#   - Result type is `int` (1 = valid, 0 = invalid).
#   - Adds GC mark function generation (NODE children get rb_gc_mark'd via wrapper).
#   - Adds `double` operand type for numeric thresholds.
class ArjsvNodeDef < ASTroGen::NodeDef
  register_gen_task :mark,
    func_typedef: "typedef void (*node_marker_func_t)(struct Node *n);",
    func_prefix: "MARKER_",
    kind_field: "node_marker_func_t marker"

  class Node < ASTroGen::NodeDef::Node
    class Operand < ASTroGen::NodeDef::Node::Operand
      def hash_call(val, **kw)
        case @type
        when 'double'
          # Hash by bit-pattern via uint64.
          "hash_uint64(arjsv_double_bits(#{val}))"
        when 'int64_t'
          "hash_uint64((uint64_t)(#{val}))"
        else
          super
        end
      end

      def build_dumper(name)
        case @type
        when 'double'
          "        fprintf(fp, \"%g\", n->u.#{name}.#{self.name});"
        when 'int64_t'
          "        fprintf(fp, \"%lld\", (long long)n->u.#{name}.#{self.name});"
        else
          super
        end
      end

      def build_specializer(name)
        case @type
        when 'double'
          arg = "    fprintf(fp, \"        %.17g\", n->u.#{name}.#{self.name});"
          return nil, arg
        when 'int64_t'
          arg = "    fprintf(fp, \"        %lldLL\", (long long)n->u.#{name}.#{self.name});"
          return nil, arg
        else
          super
        end
      end
    end

    def result_type = "int"

    def build_marker
      node_ops = @operands.reject(&:ref?).select(&:node?)
      marks = node_ops.map { |op|
        "    if (n->u.#{@name}.#{op.name}) MARK(n->u.#{@name}.#{op.name});"
      }
      <<~C
      static void
      MARKER_#{@name}(NODE *n)
      {
      #{marks.empty? ? "    (void)n;" : marks.join("\n")}
      }
      C
    end
  end

  def build_mark
    <<~C
    // Auto-generated GC mark functions.

    #{@nodes.map{|name, n| n.build_marker}.join("\n")}
    C
  end
end
