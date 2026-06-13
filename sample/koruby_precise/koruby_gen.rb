# koruby_precise v2 — ASTroGen extension (slots ABI; docs/v2_design.md §7).
#
# The v2 @child contract has TWO forms, selected by the declared type
# (v2_design §7.2):
#
#   VALUE x@child      — no staging: the glue passes the child's result in a
#                        register.  Allowed ONLY for the LAST @child operand
#                        (no later sibling evaluation can GC it away) and only
#                        when the body consumes it before its first GC point.
#   VALUE_REF x@child  — the glue stores the result below the cursor
#                        (staging = rooting) and passes a VALUE_REF to the
#                        slot.  Survives sibling-eval GCs; fixup-safe reads
#                        via VALUE_REF_GET.
#
# slot_count counts only VALUE_REF children + $tmp slots; register children
# occupy no slot.  Cursor identifier is `slots` (not the framework default
# `sp`) — set via the base `cursor_name` hook.

require 'astrogen'

class KorubyNodeDef < ASTroGen::NodeDef
  class Node < ASTroGen::NodeDef::Node
    def result_type = "RESULT"

    # Dispatcher signature: (CTX *c, NODE *n, VALUE *slots)
    def common_param_count = 3

    def cursor_name = "slots"

    def child_dispatch_args(slot, field)
      "c, #{field}, slots"
    end

    # Claim our slot area on entry (child-self-advance): `slots` from the
    # parent is the parent's top; advance by slot_count so our staged
    # children / $tmps live at negative offsets below our own cursor.
    def slot_area_prologue
      slot_count > 0 ? "slots += #{slot_count};" : ""
    end

    def ref_children = @operands.select { |op| op.child? && op.type == 'VALUE_REF' }
    def reg_children = @operands.select { |op| op.child? && op.type == 'VALUE' }

    # slot_count = staged (VALUE_REF) children + $tmp slots.  Register
    # children are excluded — they never touch the slot area.
    def compute_slot_count
      validate_children!
      tmp_names = []
      child_names = @operands.select(&:child?).map(&:name)
      scan_body(@body || "") do |match|
        next unless match.start_with?('$')
        name = match[1..]
        if child_names.include?(name)
          raise "#{@name}: $#{name} — @child operands are accessed via their parameter, not $slots"
        end
        tmp_names << name unless tmp_names.include?(name)
      end
      ref_children.size + tmp_names.size
    end

    # A register (VALUE) child must be the last @child: any later sibling
    # evaluation could GC while the value sits unrooted in a register.
    def validate_children!
      children = @operands.select(&:child?)
      children.each_with_index do |op, i|
        next unless op.type == 'VALUE'
        unless i == children.size - 1
          raise "#{@name}: `VALUE #{op.name}@child` must be the last @child " \
                "(later siblings may GC) — use VALUE_REF"
        end
      end
      bad = children.find { |op| !%w[VALUE VALUE_REF].include?(op.type) }
      raise "#{@name}: @child type must be VALUE or VALUE_REF (got #{bad.type})" if bad
    end

    # Staged-child cell expression (negative offset below the advanced
    # cursor).  `slot` is the index among REF children only.
    def child_storage_expr(slot)
      "slots[#{slot - slot_count}]"
    end

    def child_storage_decl(slot)
      ""
    end

    # $name substitution: REF children get slots 0..K-1 (declaration order),
    # $tmps K..; register children are rejected in compute_slot_count.
    def substitute_sp_slots(body)
      slot_map = {}
      ref_children.each_with_index { |op, i| slot_map[op.name] = i }
      tmp_idx = ref_children.size
      scan_body(body) do |match|
        next unless match.start_with?('$')
        name = match[1..]
        unless slot_map.key?(name)
          slot_map[name] = tmp_idx
          tmp_idx += 1
        end
      end

      total_slots = slot_count
      new_body = body.gsub(/
        (
          "(?:[^"\\]|\\.)*"
          | '(?:[^'\\]|\\.)*'
          | \/\/[^\n]*
          | \/\*.*?\*\/
          | \$\w+
        )
      /xm) do |m|
        if m.start_with?('$')
          name = m[1..]
          slot = slot_map[name]
          raise "unknown slot $#{name} in #{@name}" unless slot
          "#{cursor_name}[#{slot - total_slots}]"
        else
          m
        end
      end

      [new_body, total_slots]
    end

    # ---- DISPATCH glue (v2_design §7.3): staging = rooting -------------
    #
    # For each @child in declaration order:
    #   VALUE_REF — `slots[off] = UNWRAP(dispatch(child));`  (staged)
    #   VALUE     — `VALUE _c<i> = UNWRAP(dispatch(child));` (register)
    # Body args: VALUE_REF_AT(&slots[off]) / _c<i>.
    def build_eval_dispatch
      child_ops = @operands.select(&:child?)
      if child_ops.empty?
        return super
      end

      ref_slot = {}
      ref_children.each_with_index { |op, i| ref_slot[op.name] = i }

      stage_stmts = child_ops.map do |op|
        field = "n->u.#{@name}.#{op.name}"
        call = "UNWRAP((*#{field}->head.dispatcher)(#{child_dispatch_args(nil, field)}))"
        if op.type == 'VALUE_REF'
          "    #{child_storage_expr(ref_slot[op.name])} = #{call};"
        else
          "    VALUE _c_#{op.name} = #{call};"
        end
      end.join("\n")

      body_args = comma_operands(@operands.map do |op|
        if op.child?
          if op.type == 'VALUE_REF'
            "VALUE_REF_AT(&#{child_storage_expr(ref_slot[op.name])})"
          else
            "_c_#{op.name}"
          end
        elsif op.ref?
          "&n->u.#{@name}.#{op.name}"
        elsif op.node?
          "n->u.#{@name}.#{op.name}, n->u.#{@name}.#{op.name}->head.dispatcher"
        else
          "n->u.#{@name}.#{op.name}"
        end
      end)

      advance = slot_area_prologue.empty? ? "" : "    #{slot_area_prologue}\n"
      <<~C
      static __attribute__((no_stack_protector)) #{result_type}
      DISPATCH_#{@name}(#{@prefix_args.join(', ')})
      {
      #{advance}#{stage_stmts}
          return EVAL_#{@name}(#{prefix_call_args.join(', ')}#{body_args});
      }
      C
    end

    # ---- SD emission: same shape, children direct-called by SD name ----
    def build_specializer
      child_ops = @operands.select(&:child?)
      ref_slot = {}
      ref_children.each_with_index { |op, i| ref_slot[op.name] = i }
      # Base Operand#build_specializer (for @child) interpolates
      # `@owner.child_storage_expr(@sp_slot)` — feed it the REF slot index;
      # register children are handled inline below and never consult it.
      child_ops.each { |op| op.sp_slot = ref_slot[op.name] || 0 }

      child_nodes = []
      args = []
      @operands.each do |op|
        if op.child? && op.type == 'VALUE'
          child_nodes << "    SPECIALIZE(fp, n->u.#{@name}.#{op.name});"
          args << "    fprintf(fp, \"        _c_#{op.name}\");"
        else
          cn, arg = op.build_specializer(@name)
          child_nodes << cn if cn
          args << arg
        end
      end

      setup_emitters = child_ops.map do |op|
        field = "n->u.#{@name}.#{op.name}"
        if op.type == 'VALUE_REF'
          lhs = child_storage_expr(ref_slot[op.name])
        else
          lhs = "VALUE _c_#{op.name}"
        end
        "    fprintf(fp, \"    #{lhs} = UNWRAP(%s(#{child_dispatch_args(nil, field)}));\\n\", DISPATCHER_NAME(#{field}));"
      end
      unless slot_area_prologue.empty?
        setup_emitters.unshift("    fprintf(fp, \"    #{slot_area_prologue}\\n\");")
      end

      decls = @operands.find_all(&:node?).map do |op|
        field_name = "n->u.#{@name}.#{op.name}"
        "    if (#{field_name}) { fprintf(fp, \"static inline #{result_type} %s(#{@prefix_args.join(', ')});\\n\", #{field_name}->head.dispatcher_name); }"
      end

      if @option.include? '@noinline'
        return <<~C
        static void
        SPECIALIZE_#{@name}(FILE *fp, NODE *n, bool is_public)
        {
            /* do nothing */
        }
        C
      end

      <<~C
      static void
      SPECIALIZE_#{@name}(FILE *fp, NODE *n, bool is_public)
      {
      #{ child_nodes.join("\n") }
          const char *dispatcher_name = alloc_dispatcher_name(n);
          n->head.dispatcher_name = dispatcher_name;

          if (astro_emit_sd_comments_p()) {
              fprintf(fp, "// ");
              DUMP(fp, n, true);
              fprintf(fp, "\\n");
          }

      #{ decls.join("\n") }

          if (!is_public) fprintf(fp, "static inline #{@option.include?('@always_inline') ? '__attribute__((always_inline)) ' : ''}");
          fprintf(fp, "__attribute__((no_stack_protector)) #{result_type}\\n");
          fprintf(fp, "%s(#{@prefix_args.join(', ')})\\n", dispatcher_name);
          fprintf(fp, "{\\n");
      #{ setup_emitters.join("\n        ") }
      #{
        if args.empty?
          '          fprintf(fp, "    return EVAL_' + @name + '(' + prefix_call_args.join(', ') + ');\\n");'
        else
          <<~INNER.chomp
                  fprintf(fp, "    return EVAL_#{@name}(#{prefix_call_args.join(', ')}, \\n");
              #{ args.join("\n    fprintf(fp, \",\\n\");\n") }
                  fprintf(fp, "\\n    );\\n");
          INNER
        end
      }
          fprintf(fp, "}\\n\\n");
      }
      C
    rescue ASTroGen::NodeDef::UnsupportedOperand
      "#define SPECIALIZE_#{@name}  NULL\n"
    end

    class Operand < ASTroGen::NodeDef::Node::Operand
      attr_reader :type

      def hash_call(val, kind: :horg)
        case @type
        when 'struct korb_callcache *'
          '0'   # mutable runtime cache — not part of structure
        when 'uint32_t'
          # `line` is diagnostic metadata (raise-site line number); the SD
          # references it at runtime (below), so excluding it from the hash
          # lets structurally identical nodes on different lines share SDs.
          self.name == 'line' ? '0' : super
        else
          super
        end
      end

      def build_dumper(name)
        case @type
        when 'struct korb_callcache *'
          "        fprintf(fp, \"<cc>\");"
        else
          super
        end
      end

      def build_specializer(name)
        # Staged (VALUE_REF) @child: the EVAL arg is a VALUE_REF to the
        # staging cell, mirroring the DISPATCH glue (the base default
        # would pass the bare cell expression).
        if child? && @type == 'VALUE_REF'
          cn = "    SPECIALIZE(fp, n->u.#{name}.#{self.name});"
          arg = "    fprintf(fp, \"        VALUE_REF_AT(&#{owner.child_storage_expr(sp_slot)})\");"
          return cn, arg
        end
        case @type
        when 'struct korb_callcache *'
          # @ref operand stored inline in the union; pass its address.
          return nil, "    fprintf(fp, \"        &n->u.#{name}.#{self.name}\");"
        when 'const char *'
          # String literal bytes may contain NULs; astro_fprint_cstr would
          # truncate (docs: feedback_astro_cstr_truncation).  Always emit a
          # runtime reference instead of baking the literal.
          return nil, "    fprintf(fp, \"        n->u.#{name}.#{self.name}\");"
        when 'uint32_t'
          if self.name == 'line'
            # hash-excluded (above) → must not be baked as a constant, or
            # hash-equal nodes from different lines would share a wrong SD.
            return nil, "    fprintf(fp, \"        n->u.#{name}.#{self.name}\");"
          end
          super
        else
          super
        end
      end
    end
  end
end
