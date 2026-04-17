require 'astrogen'

class AbRubyNodeDef < ASTroGen::NodeDef
  register_gen_task :mark,
    func_typedef: "typedef void (*node_marker_func_t)(struct Node *n);",
    func_prefix: "MARKER_",
    kind_field: "node_marker_func_t marker"

  class Node < ASTroGen::NodeDef::Node
    class Operand < ASTroGen::NodeDef::Node::Operand
      # method_prologue_t operand is storageless: no NODE struct field, no
      # alloc parameter.  DISPATCH passes NULL (interpreter path).  SPECIALIZE
      # queries the runtime resolver (abruby_pgo_prologue_name) for a baked
      # literal name — NULL if the resolver refuses to bake (POLY, unfilled,
      # etc.).  HASH contributes based on the baked literal so that PGO-baked
      # and unbaked specialized sites get distinct SD_ hashes.
      def storageless?
        @type == 'method_prologue_t' || super
      end

      def dispatch_default_expr
        return "NULL" if @type == 'method_prologue_t'
        super
      end

      def hash_call(val)
        return "0" if ref?
        case @type
        when 'ID'
          "hash_cstr(rb_id2name(#{val}))"
        when 'method_prologue_t'
          # `val` is the NODE* (the storageless hash path passes n).  The
          # resolver returns a stable C-string identifier for the baked
          # prologue or "none" if we're not baking here.
          "hash_cstr(abruby_pgo_prologue_name_for(#{val}))"
        else
          super
        end
      end

      def build_dumper(name)
        return nil if ref?
        return nil if storageless?
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
        when 'method_prologue_t'
          # The resolver returns either a C identifier like
          # "prologue_ast_simple_1" (bakeable) or "none" (don't bake).
          # We emit the identifier, or "NULL" for the non-baked path —
          # matching the interpreter DISPATCH behavior.
          arg = <<~ARG.chomp
                {
                    const char *_pgo = abruby_pgo_prologue_name_for(n);
                    fprintf(fp, "        %s", strcmp(_pgo, "none") == 0 ? "NULL" : _pgo);
                }
          ARG
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
