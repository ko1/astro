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
    def children_op = @operands.find(&:children?)

    # Same as the base, but the operand-name suffix set also accepts @children
    # (placed before @child so it wins the alternation on "@children").
    def parse_operands(str)
      suffix_re = /(?:@ref|@children|@child|@sym)?/
      owner = self
      @operands = str.split(',').tap do
        @prefix_args = it.shift(common_param_count)
      end.map do
        case it.strip
        when /(.+)\s+([a-zA-Z_][a-zA-Z0-9_]*#{suffix_re.source})$/
          op = self.class::Operand.new $1, $2
        when /(.+\*)([a-zA-Z_][a-zA-Z0-9_]*#{suffix_re.source})$/
          op = self.class::Operand.new $1, $2
        else
          raise "ill-formed field: #{it}"
        end
        op.owner = owner
        op
      end
    end

    def compute_slot_count
      if children_op
        raise "#{@name}: @children must be the only staged operand" unless @operands.none?(&:child?)
        has_tmp = false
        scan_body(@body || "") { |m| has_tmp ||= m.start_with?('$') }
        raise "#{@name}: @children node may not use $tmp slots" if has_tmp
        return 0   # the staged count is dynamic; the prologue advances by it
      end
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
    # DISPATCH for a @children node: dynamic cursor advance + a staging loop
    # (one EVAL/SD per node-type; the SD unrolls with a baked count).
    def build_children_dispatch(kids)
      f = "n->u.#{@name}.#{kids.name}"
      body_args = comma_operands(@operands.map do |op|
        if op.children?       then "_cnt"
        elsif op.ref?         then "&n->u.#{@name}.#{op.name}"
        elsif op.node?        then "n->u.#{@name}.#{op.name}, n->u.#{@name}.#{op.name}->head.dispatcher"
        else                       "n->u.#{@name}.#{op.name}"
        end
      end)
      # @framehdr: reserve KORB_FRAME_HDR meta cells BELOW the staged children
      # (the callee frame's EP + magic header at base[-2]/base[-3]).  The cursor
      # advances by cnt+HDR but only cnt children are dispatched; the header cells
      # are filled by korb_invoke.
      framehdr = @option.include?('@framehdr')
      hdr = framehdr ? ' + KORB_FRAME_HDR' : ''
      # The reserved header cells (base[-2..]) sit in the GC-scanned slot range,
      # so they MUST be zeroed before any arg eval (which can GC) — a stale value
      # would be misread as a heap pointer.  korb_invoke fills EP/magic later.
      zero = framehdr ? "\n          for (uint32_t _h = 0; _h < KORB_FRAME_HDR; _h++) slots[-(intptr_t)_cnt - 1 - (intptr_t)_h] = 0;" : ''
      <<~C
      static __attribute__((no_stack_protector)) #{result_type}
      DISPATCH_#{@name}(#{@prefix_args.join(', ')})
      {
          const uint32_t _cnt = #{f}_cnt;
          NODE *const *const _av = #{f};
          slots += _cnt#{hdr};#{zero}
          for (uint32_t _i = 0; _i < _cnt; _i++)
              slots[(intptr_t)_i - (intptr_t)_cnt] = UNWRAP((*_av[_i]->head.dispatcher)(c, _av[_i], slots));
          return EVAL_#{@name}(#{prefix_call_args.join(', ')}#{body_args});
      }
      C
    end

    def build_eval_dispatch
      return build_children_dispatch(children_op) if children_op
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

    # Allocator: identical to the base, but @children operands need two field
    # assignments (array ptr + count) — done via Operand#alloc_assignment.
    def build_allocator
      return super unless children_op
      alloc_ops = @operands.reject { |o| o.ref? || o.storageless? }
      ref_ops = @operands.select(&:ref?)
      sname = "#{@name}_struct"
      <<~C
      NODE *
      ALLOC_#{name}(#{alloc_ops.empty? ? 'void' : alloc_ops.map { it.join }.join(', ')}) {
          NODE *_n = node_allocate(sizeof(struct NodeHead) + sizeof(struct #{sname}));
          _n->head.dispatcher = #{alloc_dispatcher_expr};
          _n->head.dispatcher_name = "DISPATCH_#{@name}";
          _n->head.kind = &kind_#{@name};
      #ifdef ASTRO_NODEHEAD_SLOT_COUNT
          _n->head.slot_count = #{slot_count};
      #endif
      #ifdef ASTRO_NODEHEAD_PARENT
          _n->head.parent = NULL;
      #endif
      #ifdef ASTRO_NODEHEAD_JIT_STATUS
          _n->head.jit_status = JIT_STATUS_Unknown;
      #endif
      #ifdef ASTRO_NODEHEAD_DISPATCH_CNT
          _n->head.dispatch_cnt = 0;
      #endif
          _n->head.flags.has_hash_value = false;
          _n->head.flags.is_specialized = false;
          _n->head.flags.is_specializing = false;
          _n->head.flags.is_dumping = false;
          _n->head.flags.no_inline = #{no_inline? ? true : false};
      #{alloc_ops.map { it.alloc_assignment(name) }.join("\n")}
      #{ref_ops.map { "    memset(&_n->u.#{name}.#{it.name}, 0, sizeof(_n->u.#{name}.#{it.name}));" }.join("\n")}
          OPTIMIZE(_n);
          if (OPTION.record_all) code_repo_add(NULL, _n, false);
          return _n;
      }
      C
    end

    # Structural hash: @children hashes each child + the count via a C loop
    # (the base emits one hash_merge per operand, which can't express a loop).
    def build_hash_func
      return super unless children_op
      lines = @operands.reject(&:storageless?).map do |op|
        if op.children?
          fld = "n->u.#{@name}.#{op.name}"
          "    for (uint32_t _i = 0; _i < #{fld}_cnt; _i++) h = hash_merge(h, hash_node(#{fld}[_i]));\n" \
          "    h = hash_merge(h, hash_uint32(#{fld}_cnt));"
        else
          "    h = hash_merge(h, #{op.hash_call("n->u.#{@name}.#{op.name}", kind: :horg)});"
        end
      end
      <<~C
      static node_hash_t
      HASH_#{name}(NODE *n)
      {
          node_hash_t h = hash_cstr(#{canonical_name.dump});
      #{lines.join("\n")}
          return h;
      }
      C
    end

    # SD for a @children node: recursively specialize each child, then emit a
    # dispatcher that unrolls the staging with the baked count.
    def build_children_specializer(kids)
      if @option.include? '@noinline'
        return "static void\nSPECIALIZE_#{@name}(FILE *fp, NODE *n, bool is_public)\n{\n    /* do nothing */\n}\n"
      end
      f = "n->u.#{@name}.#{kids.name}"
      child_nodes = []
      args = @operands.map do |op|
        if op.children?
          'fprintf(fp, "        %u", _cnt);'
        else
          cn, arg = op.build_specializer(@name)
          child_nodes << cn if cn   # e.g. SPECIALIZE(block) for a lazy NODE* operand
          arg
        end
      end
      # cursor-advance line(s) emitted into the SD body.  @framehdr nodes must
      # reserve + zero KORB_FRAME_HDR meta cells below the staged children, exactly
      # like the interpreted dispatcher (build_children_dispatch) — otherwise the
      # AOT path leaves EP/magic unreserved and corrupts the callee frame.
      adv = if @option.include?('@framehdr')
              'fprintf(fp, "    slots += %u + KORB_FRAME_HDR;\\n", _cnt);' + "\n" +
              '          fprintf(fp, "    for (uint32_t _h = 0; _h < KORB_FRAME_HDR; _h++) slots[-(intptr_t)%u - 1 - (intptr_t)_h] = 0;\\n", _cnt);'
            else
              'fprintf(fp, "    slots += %u;\\n", _cnt);'
            end
      <<~C
      static void
      SPECIALIZE_#{@name}(FILE *fp, NODE *n, bool is_public)
      {
          const uint32_t _cnt = #{f}_cnt;
          for (uint32_t _i = 0; _i < _cnt; _i++) SPECIALIZE(fp, #{f}[_i]);
      #{child_nodes.join("\n")}
          const char *dispatcher_name = alloc_dispatcher_name(n);
          n->head.dispatcher_name = dispatcher_name;

          if (astro_emit_sd_comments_p()) {
              fprintf(fp, "// ");
              DUMP(fp, n, true);
              fprintf(fp, "\\n");
          }

          for (uint32_t _i = 0; _i < _cnt; _i++)
              if (!#{f}[_i]->head.flags.no_inline)
                  fprintf(fp, "static inline #{result_type} %s(#{@prefix_args.join(', ')});\\n", #{f}[_i]->head.dispatcher_name);

          if (!is_public) fprintf(fp, "static inline #{@option.include?('@always_inline') ? '__attribute__((always_inline)) ' : ''}");
          fprintf(fp, "__attribute__((no_stack_protector)) #{result_type}\\n");
          fprintf(fp, "%s(#{@prefix_args.join(', ')})\\n", dispatcher_name);
          fprintf(fp, "{\\n");
          #{adv}
          for (uint32_t _i = 0; _i < _cnt; _i++) {
              /* no_inline child → indirect call via its stored dispatcher (index
               * baked); inlinable child → direct call to its baked SD name. */
              if (#{f}[_i]->head.flags.no_inline)
                  fprintf(fp, "    slots[%d] = UNWRAP(#{f}[%u]->head.dispatcher(c, #{f}[%u], slots));\\n",
                          (int)_i - (int)_cnt, _i, _i);
              else
                  fprintf(fp, "    slots[%d] = UNWRAP(%s(c, #{f}[%u], slots));\\n",
                          (int)_i - (int)_cnt, #{f}[_i]->head.dispatcher_name, _i);
          }
          fprintf(fp, "    return EVAL_#{@name}(#{prefix_call_args.join(', ')}, \\n");
      #{ args.map { |a| "    " + a }.join("\n    fprintf(fp, \",\\n\");\n") }
          fprintf(fp, "\\n    );\\n");
          fprintf(fp, "}\\n\\n");
      }
      C
    end

    # ---- SD emission: same shape, children direct-called by SD name ----
    def build_specializer
      return build_children_specializer(children_op) if children_op
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

      # `@children` — a variadic run of child nodes staged into consecutive
      # slots (slots ABI only).  Storage is `NODE **<name>` + `uint32_t
      # <name>_cnt`; the body receives only the count (`uint32_t <name>_cnt`),
      # reading the staged values from `slots`.  Lets one node cover any arity
      # (replaces the send0..3 family).  Must be the sole staged operand.
      def initialize(type, name)
        @children = name.end_with?('@children')
        name = name.sub(/@children$/, '') if @children
        # `@sym` — a uint32_t operand holding an interned symbol ID.  The ID is
        # per-process (intern-order dependent), so it must NOT enter the
        # structural hash and must NOT be baked as a literal into an SD; the SD
        # reads it from the node at runtime (like `line`).  This keeps the code
        # store symbol-ID-independent so structurally-identical nodes share SDs
        # and a store baked in one process binds in another.  (A future loader
        # will instead hash by name + remap IDs at load; until then, exclude.)
        @sym = name.end_with?('@sym')
        name = name.sub(/@sym$/, '') if @sym
        super(type, name)
      end

      def children? = @children
      def sym? = @sym

      def storage_type = children? ? 'NODE **' : super

      def struct_field_join
        return "NODE **#{name}; uint32_t #{name}_cnt" if children?
        super
      end

      # allocator parameter list contribution (array ptr + count).
      def join
        return "NODE **#{name}, uint32_t #{name}_cnt" if children?
        super
      end

      # body sees only the count; the values live in `slots`.
      def eval_param
        return "uint32_t #{name}_cnt" if children?
        super
      end

      # not a single NODE* — structural passes handle it via the count loop.
      def node? = children? ? false : super

      # allocator assignments (two fields for @children).
      def alloc_assignment(node_name)
        if children?
          "    _n->u.#{node_name}.#{name} = #{name};\n" \
          "    _n->u.#{node_name}.#{name}_cnt = #{name}_cnt;"
        else
          "    _n->u.#{node_name}.#{name} = #{name};"
        end
      end

      def hash_call(val, kind: :horg)
        # @sym: interned symbol ID — per-process, so excluded from the hash
        # (the SD reads it at runtime; see build_specializer below).
        return '0' if sym?
        case @type
        when 'struct korb_callcache *', 'struct korb_ivcache *', 'struct korb_constcache *', 'struct korb_inlcache *', 'struct korb_oncecell *', 'struct korb_oncecell *'
          '0'   # mutable runtime cache — not part of structure
        when 'uint32_t'
          # `line` is diagnostic metadata (raise-site line number); the SD
          # references it at runtime (below), so excluding it from the hash
          # lets structurally identical nodes on different lines share SDs.
          self.name == 'line' ? '0' : super
        when 'VALUE'
          # Symbol literals are runtime-ref'd by the SD (build_specializer), so
          # exclude the per-process ID from the hash: structurally identical
          # sym lits share one SD and the store binds across intern orders.
          return super if child?
          "(SYMBOL_P(#{val}) ? hash_uint64(0xCu) : hash_uint64((uint64_t)(uintptr_t)(#{val})))"
        else
          super
        end
      end

      def build_dumper(node_name)
        if children?
          f = "n->u.#{node_name}.#{name}"
          return "        fprintf(fp, \"[\");\n" \
                 "        for (uint32_t _i = 0; _i < #{f}_cnt; _i++) { if (_i) fprintf(fp, \", \"); DUMP(fp, #{f}[_i], oneline); }\n" \
                 "        fprintf(fp, \"]\");"
        end
        case @type
        when 'struct korb_callcache *', 'struct korb_ivcache *', 'struct korb_constcache *', 'struct korb_inlcache *', 'struct korb_oncecell *'
          "        fprintf(fp, \"<cc>\");"
        else
          super
        end
      end

      def build_specializer(name)
        # Bare VALUE operand (node_lit): Symbol literals are per-process
        # interned IDs, so baking the raw bits breaks any consumer with a
        # different intern order (--build exes rebuild the AST without
        # parsing).  Runtime-ref symbols; other immediates bake as constants.
        if !child? && @type == 'VALUE'
          return nil,
            "    if (SYMBOL_P(n->u.#{name}.#{self.name}))\n" \
            "        fprintf(fp, \"        n->u.#{name}.#{self.name}\");\n" \
            "    else\n" \
            "        fprintf(fp, \"        (VALUE)0x%lxL\", (long)n->u.#{name}.#{self.name});"
        end
        # Staged (VALUE_REF) @child: the EVAL arg is a VALUE_REF to the
        # staging cell, mirroring the DISPATCH glue (the base default
        # would pass the bare cell expression).
        if child? && @type == 'VALUE_REF'
          cn = "    SPECIALIZE(fp, n->u.#{name}.#{self.name});"
          arg = "    fprintf(fp, \"        VALUE_REF_AT(&#{owner.child_storage_expr(sp_slot)})\");"
          return cn, arg
        end
        case @type
        when 'struct korb_callcache *', 'struct korb_ivcache *', 'struct korb_constcache *', 'struct korb_inlcache *', 'struct korb_oncecell *'
          # @ref operand stored inline in the union; pass its address.
          return nil, "    fprintf(fp, \"        &n->u.#{name}.#{self.name}\");"
        when 'const char *'
          # String literal bytes may contain NULs; astro_fprint_cstr would
          # truncate (docs: feedback_astro_cstr_truncation).  Always emit a
          # runtime reference instead of baking the literal.
          return nil, "    fprintf(fp, \"        n->u.#{name}.#{self.name}\");"
        when 'uint32_t'
          if self.name == 'line' || sym?
            # hash-excluded (above) → must not be baked as a constant, or
            # hash-equal nodes (different lines, or different symbol IDs)
            # would share an SD carrying the wrong constant.  Reference the
            # node field at runtime instead.
            return nil, "    fprintf(fp, \"        n->u.#{name}.#{self.name}\");"
          end
          super
        else
          super
        end
      end

      # --build embedding (_embed.c).  The framework default covers scalars and
      # NODE* children; everything koruby adds on top — symbol IDs, NUL-bearing
      # byte arrays, @children, and the parse-built structures behind `void *`
      # operands — routes through the hand-written koruby_emit_* helpers
      # (node_embed.c), whose printed expressions re-intern via `_ectx` at exe
      # startup (the builder is emitted with a `CTX *_ectx` parameter).

      # const char* operands that are really uint32 symbol-ID arrays, with the
      # per-node count expression the matcher reads at runtime.
      SYM_ARRAY_CNT = {
        %w[node_call_kw kw_syms] =>
          '(n->u.node_call_kw.argv_cnt - 1U - n->u.node_call_kw.pos_argc)',
        %w[node_undef mids]        => 'n->u.node_undef.cnt',
        %w[node_nesting name_syms] => 'n->u.node_nesting.name_cnt',
        %w[node_binding name_syms] => 'n->u.node_binding.name_cnt',
      }.freeze

      # const char* byte buffers (may contain NULs) with their length field.
      BYTES_LEN = {
        %w[node_str bytes]        => 'len',
        %w[node_str_frozen bytes] => 'len',
        %w[node_str_enc bytes]    => 'len',
        %w[node_bignum digits]    => 'len',
        %w[node_regexp bytes]     => 'len',
        %w[node_rational_big num] => 'nlen',
        %w[node_rational_big den] => 'dlen',
      }.freeze

      # void* operands → emit helper + optional count expression (%{f} = field).
      VOIDP_EMIT = {
        %w[node_def opt_defaults] =>
          ['koruby_emit_opt_defaults', 'n->u.node_def.params_cnt - n->u.node_def.req_cnt'],
        %w[node_singleton_def opt_defaults] =>
          ['koruby_emit_opt_defaults', 'n->u.node_singleton_def.params_cnt - n->u.node_singleton_def.req_cnt'],
        %w[node_entry opt_defaults] =>
          ['koruby_emit_opt_defaults', 'n->u.node_entry.params_cnt - n->u.node_entry.req_cnt'],
        %w[node_def kw_info]              => ['koruby_emit_kw_info', nil],
        %w[node_singleton_def kw_info]    => ['koruby_emit_kw_info', nil],
        %w[node_entry kw_info]            => ['koruby_emit_kw_info', nil],
        %w[node_def param_info]           => ['koruby_emit_param_info', nil],
        %w[node_singleton_def param_info] => ['koruby_emit_param_info', nil],
        %w[node_entry param_info]         => ['koruby_emit_param_info', nil],
        %w[node_entry destructure_spec] =>
          ['koruby_emit_u8s', 'n->u.node_entry.params_cnt'],
        %w[node_entry cap_ns]     => ['koruby_emit_u16s', 'n->u.node_entry.cap_depth'],
        %w[node_massign offsets]  => ['koruby_emit_i32s', 'n->u.node_massign.ntargets'],
        %w[node_massign_splat offsets] =>
          ['koruby_emit_i32s', 'n->u.node_massign_splat.npre + 1U + n->u.node_massign_splat.npost'],
        %w[node_massign_het descs] => ['koruby_emit_het_descs', 'n->u.node_massign_het.nt'],
        %w[node_attr descs]        => ['koruby_emit_attr_descs', 'n->u.node_attr.count'],
        %w[node_match_pred desc]   => ['koruby_emit_pat', nil],
        %w[node_match_req desc]    => ['koruby_emit_pat', nil],
      }.freeze

      def build_emit_ast(node_name)
        return super if ref? || storageless?
        field = "n->u.#{node_name}.#{self.name}"
        if children?
          # Prints BOTH ALLOC args (array expr + count) — @children is the sole
          # operand contributing two parameters.
          return "    koruby_emit_children(fp, #{field}, #{field}_cnt);"
        end
        return "    koruby_emit_intern(fp, #{field});" if sym?
        case storage_type
        when 'VALUE'
          "    koruby_emit_value(fp, #{field});"
        when 'const char *'
          if (cnt = SYM_ARRAY_CNT[[node_name, self.name]])
            "    koruby_emit_syms(fp, #{field}, #{cnt});"
          elsif (len = BYTES_LEN[[node_name, self.name]])
            "    koruby_emit_cstr_len(fp, #{field}, n->u.#{node_name}.#{len});"
          else
            super   # NUL-terminated diagnostic strings (what / name)
          end
        when 'void *'
          helper, cnt = VOIDP_EMIT[[node_name, self.name]]
          unless helper
            raise ASTroGen::NodeDef::UnsupportedOperand,
                  "void * #{node_name}.#{self.name} has no embed emitter"
          end
          cnt ? "    #{helper}(fp, #{field}, #{cnt});" : "    #{helper}(fp, #{field});"
        else
          super
        end
      end
    end
  end
end
