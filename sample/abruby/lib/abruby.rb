require 'prism'
require_relative '../abruby'

class AbRuby
  class Parser
    attr_reader :entries  # [(name, body_node)] for @noinline nodes (def, class, module)

    def initialize
      @frames = []
      @source_file = nil
      @entries = []
    end

    def parse(code, source_file = nil)
      @source_file = source_file
      @entries = []
      result = Prism.parse(code)
      unless result.success?
        raise SyntaxError, "parse error: #{result.errors.map(&:message).join(', ')}"
      end
      transduce(result.value)
    end

    private

    # method_body: the AST to pre-walk for block-local slot accounting.
    # Any BlockNode discovered in this subtree (before hitting another
    # method/class/module boundary) contributes params + block-new-locals
    # to the outer frame's slot count.  The block slots sit between the
    # outer method's named locals and its expression-temp region, so
    # arg_index starts past them.
    def push_frame(locals, params_cnt: 0, method_body: nil)
      locals_arr = locals.map(&:to_s)
      max_block_slots = method_body ? count_block_slots(method_body) : 0
      frame_size = locals_arr.size + max_block_slots
      @frames.push({
        locals: locals_arr,
        arg_index: frame_size,
        max: frame_size,
        params_cnt: params_cnt,
        block_slot_next: locals_arr.size,   # cursor for assigning block literal slots
        block_scope_stack: [],              # stack of {name => slot} per active block scope
      })
    end

    # Walk a subtree and sum the block local slot requirement.  Stops at
    # DefNode / ClassNode / ModuleNode boundaries (those introduce their
    # own frames).  A BlockNode contributes (params + block-new-locals)
    # slots and then recurses into its body (nested blocks share the
    # same outer frame).
    def count_block_slots(node)
      return 0 if node.nil?
      case node
      when Prism::BlockNode
        params = node.parameters
        requireds = (params && params.parameters) ? params.parameters.requireds : []
        param_names = requireds.map { |p| p.name.to_s }
        other_locals = node.locals.map(&:to_s) - param_names
        slots = param_names.size + other_locals.size
        slots + (node.body ? count_block_slots(node.body) : 0)
      when Prism::DefNode, Prism::ClassNode, Prism::ModuleNode
        0
      else
        if node.respond_to?(:child_nodes)
          node.child_nodes.compact.sum { |c| count_block_slots(c) }
        else
          0
        end
      end
    end

    def pop_frame
      @frames.pop
    end

    def current_frame
      @frames.last
    end

    def lvar_index(name)
      frame = current_frame
      # Innermost block scope wins (shadows outer method locals).
      frame[:block_scope_stack].reverse_each do |scope|
        return scope[name.to_s] if scope.key?(name.to_s)
      end
      idx = frame[:locals].index(name.to_s)
      raise "unknown local variable: #{name}" unless idx
      idx
    end

    def arg_index
      current_frame[:arg_index]
    end

    def inc_arg_index
      idx = current_frame[:arg_index]
      current_frame[:arg_index] += 1
      if current_frame[:arg_index] > current_frame[:max]
        current_frame[:max] = current_frame[:arg_index]
      end
      idx
    end

    def rewind_arg_index(idx)
      current_frame[:arg_index] = idx
    end

    def transduce(node)
      case node
      when Prism::ProgramNode
        push_frame(node.locals, method_body: node.statements)
        body = transduce(node.statements)
        frame = pop_frame
        AbRuby.alloc_node_scope(frame[:max], body)

      when Prism::StatementsNode
        stmts = node.body.map { |n| transduce(n) }
        build_seq(stmts)

      when Prism::FloatNode
        AbRuby.alloc_node_float_new(node.value)

      when Prism::IntegerNode
        v = node.value
        # Fixnum literals (anything that fits in a CRuby Fixnum,
        # ~63 bits on a 64-bit VM) are pre-encoded into a node_literal
        # at parse time and returned directly from EVAL as the stored
        # VALUE.  Larger literals stay on the bignum parse path.
        if v.is_a?(Integer) && v.bit_length < 63
          AbRuby.alloc_node_literal(v)
        else
          AbRuby.alloc_node_bignum_new(v.to_s)
        end

      when Prism::StringNode
        AbRuby.alloc_node_str_new(node.unescaped)

      when Prism::InterpolatedStringNode
        # "hello #{expr} world" → str_concat(["hello", expr.to_s, " world"])
        # Evaluate all parts, then collect into contiguous slots for str_concat
        base_idx = arg_index
        # Reserve contiguous result slots first
        result_slots = node.parts.map { inc_arg_index }
        # Reserve extra work slots for to_s calls
        work_idx = arg_index

        seq_nodes = node.parts.each_with_index.map do |part, i|
          slot = result_slots[i]
          case part
          when Prism::StringNode
            AbRuby.alloc_node_lvar_set(slot, AbRuby.alloc_node_str_new(part.unescaped))
          when Prism::EmbeddedStatementsNode
            inner = part.statements ? transduce(part.statements) : AbRuby.alloc_node_str_new("")
            # store expr result, call to_s, store back
            store = AbRuby.alloc_node_lvar_set(slot, inner)
            recv_ref = AbRuby.alloc_node_lvar_get(slot)
            to_s_call = set_line(AbRuby.alloc_node_method_call(recv_ref, "to_s", 0, work_idx), node)
            AbRuby.alloc_node_seq(store, AbRuby.alloc_node_lvar_set(slot, to_s_call))
          else
            raise "unsupported interpolation part: #{part.class}"
          end
        end

        rewind_arg_index(base_idx)
        concat_node = AbRuby.alloc_node_str_concat(node.parts.size, base_idx)
        build_seq(seq_nodes + [concat_node])

      when Prism::EmbeddedStatementsNode
        node.statements ? transduce(node.statements) : AbRuby.alloc_node_nil

      when Prism::RationalNode
        # 3r → Rational(3, 1)
        num = node.numerator
        den = node.denominator
        recv_idx = inc_arg_index
        recv_store = AbRuby.alloc_node_lvar_set(recv_idx, AbRuby.alloc_node_self)
        call_arg_idx = arg_index
        idx_num = inc_arg_index
        idx_den = inc_arg_index
        store_num = AbRuby.alloc_node_lvar_set(idx_num, AbRuby.alloc_node_num(num))
        store_den = AbRuby.alloc_node_lvar_set(idx_den, AbRuby.alloc_node_num(den))
        rewind_arg_index(recv_idx)
        recv_ref = AbRuby.alloc_node_lvar_get(recv_idx)
        call_node = set_line(AbRuby.alloc_node_method_call(recv_ref, "Rational", 2, call_arg_idx), node)
        build_seq([recv_store, store_num, store_den, call_node])

      when Prism::ImaginaryNode
        # 2i → Complex(0, 2)
        inner = node.numeric
        recv_idx = inc_arg_index
        recv_store = AbRuby.alloc_node_lvar_set(recv_idx, AbRuby.alloc_node_self)
        call_arg_idx = arg_index
        idx_real = inc_arg_index
        idx_imag = inc_arg_index
        store_real = AbRuby.alloc_node_lvar_set(idx_real, AbRuby.alloc_node_num(0))
        store_imag = AbRuby.alloc_node_lvar_set(idx_imag, transduce(inner))
        rewind_arg_index(recv_idx)
        recv_ref = AbRuby.alloc_node_lvar_get(recv_idx)
        call_node = set_line(AbRuby.alloc_node_method_call(recv_ref, "Complex", 2, call_arg_idx), node)
        build_seq([recv_store, store_real, store_imag, call_node])

      when Prism::TrueNode
        AbRuby.alloc_node_true

      when Prism::FalseNode
        AbRuby.alloc_node_false

      when Prism::NilNode
        AbRuby.alloc_node_nil

      when Prism::ParenthesesNode
        if node.body
          transduce(node.body)
        else
          AbRuby.alloc_node_nil
        end

      when Prism::GlobalVariableReadNode
        AbRuby.alloc_node_gvar_get(node.name.to_s)

      when Prism::GlobalVariableWriteNode
        AbRuby.alloc_node_gvar_set(node.name.to_s, transduce(node.value))

      when Prism::GlobalVariableOperatorWriteNode
        op = node.binary_operator.to_s
        recv = AbRuby.alloc_node_gvar_get(node.name.to_s)
        rhs = transduce(node.value)
        if BINOP_MAP.key?(op)
          call_node = set_line(AbRuby.send("alloc_node_#{BINOP_MAP[op]}", recv, rhs, inc_arg_index.tap { rewind_arg_index(_1) }), node)
        else
          call_arg_idx = arg_index
          idx = inc_arg_index
          store_rhs = AbRuby.alloc_node_lvar_set(idx, rhs)
          rewind_arg_index(call_arg_idx)
          call_node = AbRuby.alloc_node_seq(store_rhs,
            set_line(AbRuby.alloc_node_method_call(recv, op, 1, call_arg_idx), node))
        end
        AbRuby.alloc_node_gvar_set(node.name.to_s, call_node)

      when Prism::InstanceVariableReadNode
        AbRuby.alloc_node_ivar_get(node.name.to_s)

      when Prism::InstanceVariableWriteNode
        AbRuby.alloc_node_ivar_set(node.name.to_s, transduce(node.value))

      when Prism::InstanceVariableOperatorWriteNode
        op = node.binary_operator.to_s
        recv = AbRuby.alloc_node_ivar_get(node.name.to_s)
        rhs = transduce(node.value)
        if BINOP_MAP.key?(op)
          call_node = set_line(AbRuby.send("alloc_node_#{BINOP_MAP[op]}", recv, rhs, inc_arg_index.tap { rewind_arg_index(_1) }), node)
        else
          call_arg_idx = arg_index
          idx = inc_arg_index
          store_rhs = AbRuby.alloc_node_lvar_set(idx, rhs)
          rewind_arg_index(call_arg_idx)
          call_node = AbRuby.alloc_node_seq(store_rhs,
            set_line(AbRuby.alloc_node_method_call(recv, op, 1, call_arg_idx), node))
        end
        AbRuby.alloc_node_ivar_set(node.name.to_s, call_node)

      when Prism::LocalVariableReadNode
        AbRuby.alloc_node_lvar_get(lvar_index(node.name))

      when Prism::LocalVariableWriteNode
        AbRuby.alloc_node_lvar_set(lvar_index(node.name), transduce(node.value))

      when Prism::LocalVariableOperatorWriteNode
        # a += 1 => a = a.+(1)
        op = node.binary_operator.to_s
        recv = AbRuby.alloc_node_lvar_get(lvar_index(node.name))
        rhs = transduce(node.value)
        if BINOP_MAP.key?(op)
          call_node = set_line(AbRuby.send("alloc_node_#{BINOP_MAP[op]}", recv, rhs, inc_arg_index.tap { rewind_arg_index(_1) }), node)
        else
          call_arg_idx = arg_index
          idx = inc_arg_index
          store_rhs = AbRuby.alloc_node_lvar_set(idx, rhs)
          rewind_arg_index(call_arg_idx)
          call_node = AbRuby.alloc_node_seq(store_rhs,
            set_line(AbRuby.alloc_node_method_call(recv, op, 1, call_arg_idx), node))
        end
        AbRuby.alloc_node_lvar_set(lvar_index(node.name), call_node)

      when Prism::IfNode
        cond = transduce(node.predicate)
        then_n = node.statements ? transduce(node.statements) : AbRuby.alloc_node_nil
        else_n = node.subsequent ? transduce(node.subsequent) : AbRuby.alloc_node_nil
        AbRuby.alloc_node_if(cond, then_n, else_n)

      when Prism::ElseNode
        if node.statements
          transduce(node.statements)
        else
          AbRuby.alloc_node_nil
        end

      when Prism::UnlessNode
        cond = transduce(node.predicate)
        then_n = node.statements ? transduce(node.statements) : AbRuby.alloc_node_nil
        else_n = node.else_clause ? transduce(node.else_clause) : AbRuby.alloc_node_nil
        AbRuby.alloc_node_if(cond, else_n, then_n)

      when Prism::WhileNode
        AbRuby.alloc_node_while(transduce(node.predicate), transduce(node.statements))

      when Prism::UntilNode
        # until cond → while !cond
        cond = transduce(node.predicate)
        body = transduce(node.statements)
        # !cond = if(cond, false, true)
        not_cond = AbRuby.alloc_node_if(cond, AbRuby.alloc_node_false, AbRuby.alloc_node_true)
        AbRuby.alloc_node_while(not_cond, body)

      when Prism::AndNode
        # a && b → tmp = a; if(tmp, b, tmp)
        idx = inc_arg_index
        store = AbRuby.alloc_node_lvar_set(idx, transduce(node.left))
        ref = AbRuby.alloc_node_lvar_get(idx)
        ref2 = AbRuby.alloc_node_lvar_get(idx)
        rewind_arg_index(idx)
        AbRuby.alloc_node_seq(store, AbRuby.alloc_node_if(ref, transduce(node.right), ref2))

      when Prism::OrNode
        # a || b → tmp = a; if(tmp, tmp, b)
        idx = inc_arg_index
        store = AbRuby.alloc_node_lvar_set(idx, transduce(node.left))
        ref = AbRuby.alloc_node_lvar_get(idx)
        ref2 = AbRuby.alloc_node_lvar_get(idx)
        rewind_arg_index(idx)
        AbRuby.alloc_node_seq(store, AbRuby.alloc_node_if(ref, ref2, transduce(node.right)))

      when Prism::BreakNode
        value = if node.arguments
                  args = node.arguments.arguments
                  if args.size == 1
                    transduce(args[0])
                  else
                    raise "break with multiple values not supported"
                  end
                else
                  AbRuby.alloc_node_nil
                end
        AbRuby.alloc_node_break(value)

      when Prism::NextNode
        value = if node.arguments
                  args = node.arguments.arguments
                  if args.size == 1
                    transduce(args[0])
                  else
                    raise "next with multiple values not supported"
                  end
                else
                  AbRuby.alloc_node_nil
                end
        AbRuby.alloc_node_next(value)

      when Prism::ReturnNode
        # `return a, b, c` desugars to `return [a, b, c]` — Ruby semantics.
        # `return *ary` isn't supported (rare in practice).
        value = if node.arguments
                  args = node.arguments.arguments
                  if args.size == 1 && !args[0].is_a?(Prism::SplatNode)
                    transduce(args[0])
                  else
                    build_ary_literal(args)
                  end
                else
                  AbRuby.alloc_node_nil
                end
        AbRuby.alloc_node_return(value)

      when Prism::YieldNode
        args = node.arguments&.arguments || []
        call_arg_idx = arg_index
        if args.any?
          seq_nodes = args.map do |arg|
            idx = inc_arg_index
            AbRuby.alloc_node_lvar_set(idx, transduce(arg))
          end
          rewind_arg_index(call_arg_idx)
          yield_node = set_line(AbRuby.alloc_node_yield(args.size, call_arg_idx), node)
          build_seq(seq_nodes + [yield_node])
        else
          set_line(AbRuby.alloc_node_yield(0, call_arg_idx), node)
        end

      when Prism::DefNode
        name = node.name.to_s
        params = node.parameters
        params_cnt = params ? params.requireds.size : 0

        push_frame(node.locals, params_cnt: params_cnt, method_body: node.body)
        body = node.body ? transduce(node.body) : AbRuby.alloc_node_nil
        frame = pop_frame

        @entries << [name, body]
        AbRuby.alloc_node_def(name, body, params_cnt, frame[:max])

      when Prism::ArrayNode
        elements = node.elements
        base_idx = arg_index
        seq_nodes = elements.map do |elem|
          idx = inc_arg_index
          AbRuby.alloc_node_lvar_set(idx, transduce(elem))
        end
        rewind_arg_index(base_idx)
        ary_node = AbRuby.alloc_node_ary_new(elements.size, base_idx)
        seq_nodes.empty? ? ary_node : build_seq(seq_nodes + [ary_node])

      when Prism::HashNode, Prism::KeywordHashNode
        # KeywordHashNode appears when a call is written as `foo(a: 1, b: 2)`;
        # Ruby semantics for our purposes is "pass a Hash as the last arg".
        # Treat it exactly like a HashNode.
        pairs = node.elements
        base_idx = arg_index
        seq_nodes = []
        pairs.each do |assoc|
          k_idx = inc_arg_index
          v_idx = inc_arg_index
          seq_nodes << AbRuby.alloc_node_lvar_set(k_idx, transduce(assoc.key))
          seq_nodes << AbRuby.alloc_node_lvar_set(v_idx, transduce(assoc.value))
        end
        rewind_arg_index(base_idx)
        hash_node = AbRuby.alloc_node_hash_new(pairs.size * 2, base_idx)
        seq_nodes.empty? ? hash_node : build_seq(seq_nodes + [hash_node])

      when Prism::SymbolNode
        AbRuby.alloc_node_sym(node.value.to_s)

      when Prism::RangeNode
        b = node.left ? transduce(node.left) : AbRuby.alloc_node_nil
        e = node.right ? transduce(node.right) : AbRuby.alloc_node_nil
        exclude = (node.operator == "...") ? 1 : 0
        AbRuby.alloc_node_range_new(b, e, exclude)

      when Prism::RegularExpressionNode
        flags = extract_regexp_flags(node)
        AbRuby.alloc_node_regexp_new(node.unescaped, flags)

      when Prism::SelfNode
        AbRuby.alloc_node_self

      when Prism::ConstantWriteNode
        AbRuby.alloc_node_const_set(node.name.to_s, transduce(node.value))

      when Prism::ConstantReadNode
        AbRuby.alloc_node_const_get(node.name.to_s)

      when Prism::ConstantPathNode
        if node.parent.is_a?(Prism::ConstantReadNode)
          AbRuby.alloc_node_const_path_get(node.parent.name.to_s, node.name.to_s)
        else
          raise "unsupported constant path: #{node.inspect}"
        end

      when Prism::ModuleNode
        name = node.constant_path.name.to_s
        body = node.body ? transduce(node.body) : AbRuby.alloc_node_nil
        @entries << ["module:#{name}", body]
        AbRuby.alloc_node_module_def(name, body)

      when Prism::ClassNode
        name = node.constant_path.name.to_s
        super_expr = node.superclass ? transduce(node.superclass) : AbRuby.alloc_node_nil
        body = node.body ? transduce(node.body) : AbRuby.alloc_node_nil
        @entries << ["class:#{name}", body]
        @entries << ["class:#{name}:super", super_expr]
        AbRuby.alloc_node_class_def(name, super_expr, body)

      when Prism::MultiWriteNode
        # a, b = 1, 2 — right side is always ArrayNode from parser
        lefts = node.lefts
        values = node.value

        # Evaluate right-hand values into temp slots
        if values.is_a?(Prism::ArrayNode) &&
           values.elements.none? { |e| e.is_a?(Prism::SplatNode) }
          rhs_nodes = values.elements.map { |e| transduce(e) }
        elsif values.is_a?(Prism::ArrayNode) &&
              values.elements.size == 1 && values.elements[0].is_a?(Prism::SplatNode)
          # a, b = *expr → treat expr as the array to decompose
          values = values.elements[0].expression
        end

        # RHS is a single expression yielding an array: decompose via [0], [1], ...
        unless rhs_nodes
          # RHS is a single expression (e.g., method call returning array)
          # Evaluate into temp, then access via [0], [1], ...
          ary_idx = inc_arg_index
          ary_store = AbRuby.alloc_node_lvar_set(ary_idx, transduce(values))
          rhs_nodes = lefts.each_index.map do |i|
            ary_ref = AbRuby.alloc_node_lvar_get(ary_idx)
            idx_node = AbRuby.alloc_node_num(i)
            # ary_ref[i] via method call
            recv_idx = inc_arg_index
            recv_store = AbRuby.alloc_node_lvar_set(recv_idx, ary_ref)
            arg_idx = inc_arg_index
            arg_store = AbRuby.alloc_node_lvar_set(arg_idx, idx_node)
            rewind_arg_index(recv_idx)
            recv_ref = AbRuby.alloc_node_lvar_get(recv_idx)
            call_node = AbRuby.alloc_node_method_call(recv_ref, "[]", 1, arg_idx)
            build_seq([ary_store, recv_store, arg_store, call_node])
          end
          rewind_arg_index(ary_idx)
        end

        assigns = []
        lefts.each_with_index do |target, i|
          rhs = i < rhs_nodes.size ? rhs_nodes[i] : AbRuby.alloc_node_nil
          case target
          when Prism::LocalVariableTargetNode
            assigns << AbRuby.alloc_node_lvar_set(lvar_index(target.name), rhs)
          when Prism::InstanceVariableTargetNode
            assigns << AbRuby.alloc_node_ivar_set(target.name.to_s, rhs)
          when Prism::GlobalVariableTargetNode
            assigns << AbRuby.alloc_node_gvar_set(target.name.to_s, rhs)
          else
            raise "unsupported multi-assign target: #{target.class}"
          end
        end

        build_seq(assigns)

      when Prism::BeginNode
        body = node.statements ? transduce(node.statements) : AbRuby.alloc_node_nil

        rescue_clause = node.rescue_clause
        if rescue_clause
          rescue_body = rescue_clause.statements ? transduce(rescue_clause.statements) : AbRuby.alloc_node_nil

          exception_lvar_index = if rescue_clause.reference.is_a?(Prism::LocalVariableTargetNode)
            lvar_index(rescue_clause.reference.name)
          else
            0xFFFFFFFF
          end
        else
          rescue_body = AbRuby.alloc_node_nil
          exception_lvar_index = 0xFFFFFFFF
        end

        ensure_body = if node.ensure_clause&.statements
          transduce(node.ensure_clause.statements)
        else
          AbRuby.alloc_node_nil
        end

        AbRuby.alloc_node_rescue(body, rescue_body, ensure_body, exception_lvar_index)

      when Prism::CaseNode
        transduce_case(node)

      when Prism::CallNode
        if !node.receiver && %w[attr_reader attr_writer attr_accessor].include?(node.name.to_s)
          transduce_attr(node)
        elsif node.receiver &&
           %w[+ - * /].include?(node.name.to_s) &&
           node.arguments&.arguments&.size == 1
          transduce_binop(node)
        else
          transduce_call(node)
        end

      when Prism::ForwardingSuperNode
        # super (bare) — forward all args by expanding to super(param0, param1, ...)
        params_cnt = current_frame[:params_cnt]
        if params_cnt == 0
          AbRuby.alloc_node_super(0, arg_index)
        else
          base_idx = arg_index
          seq_nodes = (0...params_cnt).map do |i|
            idx = inc_arg_index
            AbRuby.alloc_node_lvar_set(idx, AbRuby.alloc_node_lvar_get(i))
          end
          rewind_arg_index(base_idx)
          call_node = AbRuby.alloc_node_super(params_cnt, base_idx)
          build_seq(seq_nodes + [call_node])
        end

      when Prism::SuperNode
        # super() or super(args), optionally with an explicit { ... } block.
        # If no explicit block, the current method's received block is
        # implicitly forwarded by node_super at runtime.
        args = node.arguments&.arguments || []
        has_block = node.respond_to?(:block) && node.block
        block_literal = has_block ? transduce_block_literal(node.block) : nil

        if args.empty?
          if block_literal
            set_line(AbRuby.alloc_node_super_with_block(0, arg_index, block_literal), node)
          else
            AbRuby.alloc_node_super(0, arg_index)
          end
        else
          base_idx = arg_index
          seq_nodes = args.map do |arg|
            idx = inc_arg_index
            AbRuby.alloc_node_lvar_set(idx, transduce(arg))
          end
          rewind_arg_index(base_idx)
          call_node =
            if block_literal
              set_line(AbRuby.alloc_node_super_with_block(args.size, base_idx, block_literal), node)
            else
              AbRuby.alloc_node_super(args.size, base_idx)
            end
          build_seq(seq_nodes + [call_node])
        end

      else
        loc = node.location
        file = @source_file || "(unknown)"
        raise "unsupported node: #{node.class} at #{file}:#{loc.start_line}"
      end
    end

    # Desugar case/when into if/elsif chain with === calls
    def transduce_case(node)
      pred = node.predicate

      # Store predicate in a temp variable
      tmp_idx = inc_arg_index
      tmp_store = AbRuby.alloc_node_lvar_set(tmp_idx, transduce(pred))

      # Build from inside out: start with else
      else_body = if node.else_clause&.statements
                    transduce(node.else_clause.statements)
                  else
                    AbRuby.alloc_node_nil
                  end

      # Process when clauses in reverse to build nested if/elsif
      result = node.conditions.reverse.reduce(else_body) do |else_node, when_node|
        body = when_node.statements ? transduce(when_node.statements) : AbRuby.alloc_node_nil

        # Build condition: cond1 === tmp || cond2 === tmp || ...
        cond = when_node.conditions.map do |c|
          # c === tmp  →  c.===(tmp)
          cond_val = transduce(c)
          tmp_ref = AbRuby.alloc_node_lvar_get(tmp_idx)
          recv_idx = inc_arg_index
          recv_store = AbRuby.alloc_node_lvar_set(recv_idx, cond_val)
          arg_idx = inc_arg_index
          arg_store = AbRuby.alloc_node_lvar_set(arg_idx, tmp_ref)
          rewind_arg_index(recv_idx)
          recv_ref = AbRuby.alloc_node_lvar_get(recv_idx)
          call_node = AbRuby.alloc_node_method_call(recv_ref, "===", 1, arg_idx)
          build_seq([recv_store, arg_store, call_node])
        end

        # OR conditions together: c1 || c2 || ...
        combined = cond.reduce do |left, right|
          # left || right → tmp2 = left; if(tmp2, tmp2, right)
          or_idx = inc_arg_index
          or_store = AbRuby.alloc_node_lvar_set(or_idx, left)
          or_ref1 = AbRuby.alloc_node_lvar_get(or_idx)
          or_ref2 = AbRuby.alloc_node_lvar_get(or_idx)
          rewind_arg_index(or_idx)
          AbRuby.alloc_node_seq(or_store, AbRuby.alloc_node_if(or_ref1, or_ref2, right))
        end

        AbRuby.alloc_node_if(combined, body, else_node)
      end

      rewind_arg_index(tmp_idx)
      AbRuby.alloc_node_seq(tmp_store, result)
    end

    def transduce_attr(node)
      kind = node.name.to_s
      syms = node.arguments.arguments.map { |a| a.value.to_s }
      defs = []

      syms.each do |attr|
        ivar = "@#{attr}"
        if kind == "attr_reader" || kind == "attr_accessor"
          # def attr; @attr; end
          body = AbRuby.alloc_node_ivar_get(ivar)
          defs << AbRuby.alloc_node_def(attr, body, 0, 0)
        end
        if kind == "attr_writer" || kind == "attr_accessor"
          # def attr=(value); @attr = value; end
          push_frame([:"#{attr}_value"])
          param_ref = AbRuby.alloc_node_lvar_get(0)
          body = AbRuby.alloc_node_ivar_set(ivar, param_ref)
          pop_frame
          defs << AbRuby.alloc_node_def("#{attr}=", body, 1, 1)
        end
      end

      defs.size == 1 ? defs[0] : build_seq(defs)
    end

    BINOP_MAP = {
      "+" => "fixnum_plus", "-" => "fixnum_minus",
      "*" => "fixnum_mul", "/" => "fixnum_div",
      "<" => "fixnum_lt", "<=" => "fixnum_le",
      ">" => "fixnum_gt", ">=" => "fixnum_ge",
      "==" => "fixnum_eq", "!=" => "fixnum_neq",
      "%" => "fixnum_mod",
    }.freeze

    def transduce_binop(node)
      left = transduce(node.receiver)
      right = transduce(node.arguments.arguments[0])
      # Reserve a fallback slot for arith method dispatch (e.g., Float + Float)
      fallback_idx = inc_arg_index
      rewind_arg_index(fallback_idx)
      alloc_method = "alloc_node_#{BINOP_MAP[node.name.to_s]}"
      set_line(AbRuby.send(alloc_method, left, right, fallback_idx), node)
    end

    # Parse a Prism::BlockNode into a node_block_literal.
    #
    # Slot allocation strategy:
    #  - The outer method's frame size was pre-extended by push_frame with
    #    count_block_slots(method_body), so there is a guaranteed-free
    #    range [locals.size .. locals.size + total_block_slots).
    #  - Each block literal encountered in transduce order consumes a
    #    fresh chunk from the cursor frame[:block_slot_next], regardless
    #    of whether names clash with other blocks (sibling blocks get
    #    disjoint slot ranges; nested blocks too).
    #  - Block param and block-new-local names are registered in a
    #    per-block scope hash that lvar_index consults before falling
    #    through to the outer method locals — this is how shadowing of
    #    an outer-method variable by a same-named block param is
    #    implemented.
    #  - captured_fp (= outer fp at call time) + param_base gives the
    #    absolute slot address; yield writes args there before running
    #    the body.
    #  - arg_index stays pointing at the expression-temp region above
    #    block locals; the outer call's args/recv go there and the
    #    callee's fp starts beyond block locals, so captured writes
    #    never collide with callee frame memory.
    def transduce_block_literal(block_node)
      params = block_node.parameters
      requireds = []
      if params
        inner = params.parameters
        requireds = inner ? inner.requireds : []
        unsupported = [
          inner&.optionals, inner&.rest, inner&.posts,
          inner&.keywords, inner&.keyword_rest, inner&.block
        ].compact.flat_map { |x| x.respond_to?(:each) ? x.to_a : [x] }.compact
        unless unsupported.empty?
          raise "block parameters other than required are not supported (Phase 2): #{unsupported.inspect}"
        end
        if params.respond_to?(:locals) && params.locals && !params.locals.empty?
          raise "block-local variables |..;x,y| not supported (Phase 2)"
        end
      end
      param_names = requireds.map { |p| p.name.to_s }
      other_locals = block_node.locals.map(&:to_s) - param_names

      frame = current_frame
      param_base = frame[:block_slot_next]
      slot_cnt = param_names.size + other_locals.size
      frame[:block_slot_next] = param_base + slot_cnt

      scope = {}
      param_names.each_with_index { |n, i| scope[n] = param_base + i }
      other_locals.each_with_index { |n, i| scope[n] = param_base + param_names.size + i }
      frame[:block_scope_stack].push(scope)

      body = block_node.body ? transduce(block_node.body) : AbRuby.alloc_node_nil

      frame[:block_scope_stack].pop
      # block_slot_next stays advanced — sibling/later blocks in the
      # same outer method get a fresh range, matching the pre-walk in
      # count_block_slots so max is respected.

      set_line(AbRuby.alloc_node_block_literal(body, param_names.size, param_base), block_node)
    end

    # Maximum dynamic args for a splat call; must match
    # ABRUBY_APPLY_MAX_ARGS in node.def.
    APPLY_MAX_ARGS = 32

    # Does this argument list contain a splat?
    def args_have_splat?(args)
      args.any? { |a| a.is_a?(Prism::SplatNode) }
    end

    # Build an AST that evaluates to an Array of one element: transduce(arg)
    def build_single_ary(arg)
      base_idx = arg_index
      slot = inc_arg_index
      rewind_arg_index(base_idx)
      build_seq([
        AbRuby.alloc_node_lvar_set(slot, transduce(arg)),
        AbRuby.alloc_node_ary_new(1, base_idx),
      ])
    end

    # Build an AST that evaluates to an Array literal containing the given args.
    def build_ary_literal(args)
      return AbRuby.alloc_node_ary_new(0, arg_index) if args.empty?
      base_idx = arg_index
      slots = args.map { inc_arg_index }
      seq = args.each_with_index.map do |a, i|
        AbRuby.alloc_node_lvar_set(slots[i], transduce(a))
      end
      rewind_arg_index(base_idx)
      build_seq(seq + [AbRuby.alloc_node_ary_new(args.size, base_idx)])
    end

    # Lower a mixed splat/non-splat arg list to a single Array expression.
    # `foo(a, *b, c, *d)` -> `[a] + b + [c] + d`
    # Splat argument expressions must already evaluate to an Array.
    def build_splat_args_array(args)
      parts = []   # list of array-valued AST expressions
      buf = []     # buffered non-splat args

      flush = -> {
        unless buf.empty?
          parts << build_ary_literal(buf)
          buf = []
        end
      }

      args.each do |a|
        if a.is_a?(Prism::SplatNode)
          flush.()
          # *expr: expect an Array
          parts << transduce(a.expression)
        else
          buf << a
        end
      end
      flush.()

      # Fold with Array#+.
      parts.reduce do |acc, x|
        fallback_idx = inc_arg_index
        rewind_arg_index(fallback_idx)
        AbRuby.alloc_node_plus(acc, x, fallback_idx)
      end
    end

    # Emit an apply-call node when a call has splat arguments.
    # recv_node is nil for implicit-self calls (func_call_apply).
    def transduce_splat_call(node, recv_node, name, args)
      # Reserve 1 + APPLY_MAX_ARGS slots at call_arg_idx.  The runtime pins
      # the args array at fp[call_arg_idx] and unpacks into
      # fp[call_arg_idx+1 .. call_arg_idx+1+argc].  Any intermediate
      # compile-time temps used by recv/args subexpressions must sit PAST
      # the reservation, so we leave arg_index advanced and only rewind at
      # the end.
      call_arg_idx = arg_index
      (1 + APPLY_MAX_ARGS).times { inc_arg_index }

      recv_ast = recv_node ? transduce(recv_node) : nil
      args_expr = build_splat_args_array(args)

      rewind_arg_index(call_arg_idx)

      if recv_ast
        set_line(AbRuby.alloc_node_method_call_apply(recv_ast, name.to_s, args_expr, call_arg_idx), node)
      else
        set_line(AbRuby.alloc_node_func_call_apply(name.to_s, args_expr, call_arg_idx), node)
      end
    end

    def transduce_call(node)
      name = node.name
      args = node.arguments&.arguments || []
      line = node.location.start_line

      # Splat in call arguments → apply-call form.
      if args_have_splat?(args)
        if node.respond_to?(:block) && node.block
          raise "splat call with block is not supported yet"
        end
        recv = node.receiver
        if recv.is_a?(Prism::SelfNode)
          return transduce_splat_call(node, nil, name, args)
        elsif recv
          return transduce_splat_call(node, recv, name, args)
        else
          return transduce_splat_call(node, nil, name, args)
        end
      end

      # method call with receiver: obj.method(args)
      if node.receiver
        # Binary operators → specialized nodes
        op = name.to_s
        if BINOP_MAP.key?(op) && args.size == 1
          if node.respond_to?(:block) && node.block
            raise "binary operator with block is not supported"
          end
          return transduce_binop(node)
        end

        # Explicit self.method() → node_func_call (skip recv slot)
        if node.receiver.is_a?(Prism::SelfNode)
          call_arg_idx = arg_index
          if args.any?
            seq_nodes = args.map do |arg|
              idx = inc_arg_index
              AbRuby.alloc_node_lvar_set(idx, transduce(arg))
            end
            rewind_arg_index(call_arg_idx)
            if node.respond_to?(:block) && node.block
              block_literal = transduce_block_literal(node.block)
              call_node = set_line(AbRuby.alloc_node_func_call_with_block(name.to_s, args.size, call_arg_idx, block_literal), node)
            else
              call_node = set_line(AbRuby.alloc_node_func_call(name.to_s, args.size, call_arg_idx), node)
            end
            return build_seq(seq_nodes + [call_node])
          else
            if node.respond_to?(:block) && node.block
              block_literal = transduce_block_literal(node.block)
              return set_line(AbRuby.alloc_node_func_call_with_block(name.to_s, 0, call_arg_idx, block_literal), node)
            else
              return set_line(AbRuby.alloc_node_func_call(name.to_s, 0, call_arg_idx), node)
            end
          end
        end

        # Reserve a slot for the receiver result to avoid slot collision
        # with args that may contain nested calls
        recv_idx = inc_arg_index
        recv_store = AbRuby.alloc_node_lvar_set(recv_idx, transduce(node.receiver))

        call_arg_idx = arg_index
        if args.any?
          seq_nodes = args.map do |arg|
            idx = inc_arg_index
            AbRuby.alloc_node_lvar_set(idx, transduce(arg))
          end
          rewind_arg_index(recv_idx)

          recv_ref = AbRuby.alloc_node_lvar_get(recv_idx)
          if node.respond_to?(:block) && node.block
            block_literal = transduce_block_literal(node.block)
            call_node = set_line(AbRuby.alloc_node_method_call_with_block(recv_ref, name.to_s, args.size, call_arg_idx, block_literal), node)
          else
            call_node = set_line(AbRuby.alloc_node_method_call(recv_ref, name.to_s, args.size, call_arg_idx), node)
          end
          return build_seq([recv_store] + seq_nodes + [call_node])
        else
          rewind_arg_index(recv_idx)
          recv_ref = AbRuby.alloc_node_lvar_get(recv_idx)
          if node.respond_to?(:block) && node.block
            block_literal = transduce_block_literal(node.block)
            call_node = set_line(AbRuby.alloc_node_method_call_with_block(recv_ref, name.to_s, 0, call_arg_idx, block_literal), node)
          else
            call_node = set_line(AbRuby.alloc_node_method_call(recv_ref, name.to_s, 0, call_arg_idx), node)
          end
          return AbRuby.alloc_node_seq(recv_store, call_node)
        end
      end

      # function call (no receiver) → node_func_call (optimized self-call)
      call_arg_idx = arg_index

      if args.any?
        seq_nodes = args.map do |arg|
          idx = inc_arg_index
          AbRuby.alloc_node_lvar_set(idx, transduce(arg))
        end
        rewind_arg_index(call_arg_idx)

        if node.respond_to?(:block) && node.block
          block_literal = transduce_block_literal(node.block)
          call_node = set_line(AbRuby.alloc_node_func_call_with_block(name.to_s, args.size, call_arg_idx, block_literal), node)
        else
          call_node = set_line(AbRuby.alloc_node_func_call(name.to_s, args.size, call_arg_idx), node)
        end
        build_seq(seq_nodes + [call_node])
      else
        if node.respond_to?(:block) && node.block
          block_literal = transduce_block_literal(node.block)
          set_line(AbRuby.alloc_node_func_call_with_block(name.to_s, 0, call_arg_idx, block_literal), node)
        else
          set_line(AbRuby.alloc_node_func_call(name.to_s, 0, call_arg_idx), node)
        end
      end
    end

    def extract_regexp_flags(node)
      flags = ""
      # Check Prism flag constants
      if defined?(Prism::RegularExpressionFlags::IGNORE_CASE) &&
         node.respond_to?(:options)
        opts = node.options rescue 0
        flags += "i" if (opts & 1) != 0
        flags += "x" if (opts & 2) != 0
        flags += "m" if (opts & 4) != 0
      end
      # Fallback: parse the closing delimiter from source if available
      if flags.empty? && node.respond_to?(:closing) && node.closing
        closing = node.closing.delete("/")
        flags = closing
      end
      flags
    end

    # Set source line number on a node from a Prism AST node
    def set_line(ab_node, prism_node)
      if prism_node.respond_to?(:location) && prism_node.location
        AbRuby.set_node_line(ab_node, prism_node.location.start_line)
      end
      ab_node
    end

    def build_seq(nodes)
      return AbRuby.alloc_node_nil if nodes.empty?
      return nodes.first if nodes.size == 1

      nodes[0..-2].reverse.reduce(nodes.last) do |tail, head|
        AbRuby.alloc_node_seq(head, tail)
      end
    end
  end

  # Instance methods
  attr_reader :last_entries  # [(name, body_node)] from last parse

  def parse(code, source_file = nil)
    parser = Parser.new
    ast = parser.parse(code, source_file || current_file)
    @last_entries = parser.entries
    ast
  end

  def eval(code)
    ast = parse(code)
    eval_ast(ast)
  end

  def dump(code, pretty: false)
    ast = parse(code)
    s = dump_ast(ast)
    pretty ? pretty_print_sexp(s) : s
  end

  # Class convenience methods (create temporary instance)
  def self.eval(code)
    new.eval(code)
  end

  def self.dump(code, pretty: false)
    new.dump(code, pretty: pretty)
  end

  def self.pretty_print_sexp(s)
    out = +""
    indent = 0
    i = 0
    while i < s.size
      ch = s[i]
      case ch
      when '('
        out << "\n" << ("  " * indent) unless out.empty?
        out << '('
        indent += 1
      when ')'
        indent -= 1
        out << ')'
      else
        out << ch
      end
      i += 1
    end
    out
  end
end
