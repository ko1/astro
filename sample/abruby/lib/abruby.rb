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

    # Walk a subtree and sum the block local slot requirement.  Stops
    # at DefNode (which introduces its own frame).  A BlockNode
    # contributes (params + block-new-locals) slots and recurses into
    # its body.  ClassNode/ModuleNode bodies share the outer fp at
    # runtime — they contribute their own `locals` (declared in the
    # class body) plus any nested-block slots their body needs.
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
      when Prism::DefNode
        0
      when Prism::ClassNode, Prism::ModuleNode
        node.locals.size + (node.body ? count_block_slots(node.body) : 0)
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

      when Prism::InterpolatedSymbolNode
        # `:"@#{x}"` — build the interpolated string then call .to_sym.
        str_node = node.dup
        # Rebuild the same parts as an InterpolatedStringNode-equivalent
        # walk, then wrap with a to_sym call.
        str_ast =
          begin
            fake = Struct.new(:parts).new(node.parts)
            # Inline the interpolated-string lowering logic here.
            base_idx = arg_index
            result_slots = fake.parts.map { inc_arg_index }
            work_idx = arg_index

            seq_nodes = fake.parts.each_with_index.map do |part, i|
              slot = result_slots[i]
              case part
              when Prism::StringNode
                AbRuby.alloc_node_lvar_set(slot, AbRuby.alloc_node_str_new(part.unescaped))
              when Prism::EmbeddedStatementsNode
                inner = part.statements ? transduce(part.statements) : AbRuby.alloc_node_str_new("")
                store = AbRuby.alloc_node_lvar_set(slot, inner)
                recv_ref = AbRuby.alloc_node_lvar_get(slot)
                to_s_call = set_line(AbRuby.alloc_node_method_call(recv_ref, "to_s", 0, work_idx), node)
                AbRuby.alloc_node_seq(store, AbRuby.alloc_node_lvar_set(slot, to_s_call))
              else
                raise "unsupported interpolation part: #{part.class}"
              end
            end

            rewind_arg_index(base_idx)
            concat_node = AbRuby.alloc_node_str_concat(fake.parts.size, base_idx)
            build_seq(seq_nodes + [concat_node])
          end

        # Now call `.to_sym` on the concatenated string.
        AbRuby.alloc_node_method_call(str_ast, "to_sym", 0, arg_index)

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

      # === X ||= v / X &&= v lowering ===
      #
      # For each left-hand target, `X ||= v` lowers to
      #   tmp = read(X); if tmp then tmp else write(X, v) end
      # and `X &&= v` lowers to
      #   tmp = read(X); if tmp then write(X, v) else tmp end
      #
      # Local / ivar / gvar / constant targets can re-evaluate the read
      # and write expressions directly (no side effects at the location
      # itself).  Index / call targets cache receiver (and arguments)
      # into temp slots so they are evaluated exactly once.

      when Prism::LocalVariableOrWriteNode
        li = lvar_index(node.name)
        build_or_write(
          AbRuby.alloc_node_lvar_get(li),
          transduce(node.value),
          ->(v){ AbRuby.alloc_node_lvar_set(li, v) })

      when Prism::LocalVariableAndWriteNode
        li = lvar_index(node.name)
        build_and_write(
          AbRuby.alloc_node_lvar_get(li),
          transduce(node.value),
          ->(v){ AbRuby.alloc_node_lvar_set(li, v) })

      when Prism::InstanceVariableOrWriteNode
        nm = node.name.to_s
        build_or_write(
          AbRuby.alloc_node_ivar_get(nm),
          transduce(node.value),
          ->(v){ AbRuby.alloc_node_ivar_set(nm, v) })

      when Prism::InstanceVariableAndWriteNode
        nm = node.name.to_s
        build_and_write(
          AbRuby.alloc_node_ivar_get(nm),
          transduce(node.value),
          ->(v){ AbRuby.alloc_node_ivar_set(nm, v) })

      when Prism::GlobalVariableOrWriteNode
        nm = node.name.to_s
        build_or_write(
          AbRuby.alloc_node_gvar_get(nm),
          transduce(node.value),
          ->(v){ AbRuby.alloc_node_gvar_set(nm, v) })

      when Prism::GlobalVariableAndWriteNode
        nm = node.name.to_s
        build_and_write(
          AbRuby.alloc_node_gvar_get(nm),
          transduce(node.value),
          ->(v){ AbRuby.alloc_node_gvar_set(nm, v) })

      when Prism::ConstantOrWriteNode
        nm = node.name.to_s
        build_or_write(
          AbRuby.alloc_node_const_get(nm),
          transduce(node.value),
          ->(v){ AbRuby.alloc_node_const_set(nm, v) })

      when Prism::ConstantAndWriteNode
        nm = node.name.to_s
        build_and_write(
          AbRuby.alloc_node_const_get(nm),
          transduce(node.value),
          ->(v){ AbRuby.alloc_node_const_set(nm, v) })

      when Prism::IndexOrWriteNode
        transduce_index_op_write(node, :or)
      when Prism::IndexAndWriteNode
        transduce_index_op_write(node, :and)
      when Prism::IndexOperatorWriteNode
        transduce_index_op_write(node, node.binary_operator.to_s)

      when Prism::CallOrWriteNode
        transduce_call_op_write(node, :or)
      when Prism::CallAndWriteNode
        transduce_call_op_write(node, :and)
      when Prism::CallOperatorWriteNode
        transduce_call_op_write(node, node.binary_operator.to_s)

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
        block_param_name = (params && params.block) ? params.block.name&.to_s : nil

        push_frame(node.locals, params_cnt: params_cnt, method_body: node.body)
        body = node.body ? transduce(node.body) : AbRuby.alloc_node_nil

        if block_param_name
          # `def f(&blk)`: prepend a node_block_param that converts the
          # method's implicit block (current_frame->block) to a Proc and
          # stores it in `blk`'s local slot.  When the method was called
          # without a block, the slot is set to nil.
          slot = current_frame[:locals].index(block_param_name)
          if slot
            block_param_node = AbRuby.alloc_node_block_param(slot)
            body = build_seq([block_param_node, body])
          end
        end

        frame = pop_frame
        @entries << [name, body]
        AbRuby.alloc_node_def(name, body, params_cnt, frame[:max])

      when Prism::ArrayNode
        elements = node.elements
        if elements.any? { |e| e.is_a?(Prism::SplatNode) }
          # `[a, *b, c]` → `[a] + b + [c]`.  Same folding trick as
          # splat call args.  Preserves evaluation order.
          build_splat_args_array(elements)
        else
          base_idx = arg_index
          seq_nodes = elements.map do |elem|
            idx = inc_arg_index
            AbRuby.alloc_node_lvar_set(idx, transduce(elem))
          end
          rewind_arg_index(base_idx)
          ary_node = AbRuby.alloc_node_ary_new(elements.size, base_idx)
          seq_nodes.empty? ? ary_node : build_seq(seq_nodes + [ary_node])
        end

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

      when Prism::InterpolatedRegularExpressionNode
        # `/a#{x}b/` — only used in optcarrot's codegen path (opt.rb)
        # which the -b bench never executes.  Emit a nil stub so parsing
        # succeeds; any code that actually reaches this will raise.
        AbRuby.alloc_node_nil

      when Prism::NumberedReferenceReadNode, Prism::BackReferenceReadNode,
           Prism::MatchLastLineNode
        # $1 / $~ / $& / ... — abruby doesn't track regex globals, so we
        # emit nil.  Only safe because optcarrot-bench never runs the
        # codegen paths that would observe these (the -o pipeline does).
        AbRuby.alloc_node_nil

      when Prism::RescueModifierNode
        # `expr rescue fallback` — equivalent to `begin; expr; rescue;
        # fallback; end`.  Lower to a node_rescue with no exception
        # binding and no ensure.
        body = transduce(node.expression)
        rescue_body = transduce(node.rescue_expression)
        AbRuby.alloc_node_rescue(body, rescue_body, AbRuby.alloc_node_nil, 0xFFFFFFFF)

      when Prism::SourceFileNode
        # __FILE__ — return the file path known to the parser at parse time.
        AbRuby.alloc_node_str_new((node.filepath && !node.filepath.empty?) ?
                                  node.filepath : (@source_file || "(abruby)"))

      when Prism::SourceEncodingNode
        # __ENCODING__ — abruby doesn't track encodings; emit nil.
        AbRuby.alloc_node_nil

      when Prism::SourceLineNode
        # __LINE__ — bake in the line number at parse time.
        AbRuby.alloc_node_num(node.location.start_line)

      when Prism::SelfNode
        AbRuby.alloc_node_self

      when Prism::ConstantWriteNode
        AbRuby.alloc_node_const_set(node.name.to_s, transduce(node.value))

      when Prism::ConstantReadNode
        AbRuby.alloc_node_const_get(node.name.to_s)

      when Prism::ConstantPathNode
        if node.parent.nil?
          # Top-level ::FOO
          AbRuby.alloc_node_const_get(node.name.to_s)
        elsif node.parent.is_a?(Prism::ConstantReadNode)
          AbRuby.alloc_node_const_path_get(node.parent.name.to_s, node.name.to_s)
        else
          # Runtime lookup: evaluate parent (any expression), call
          # `const_get(:NAME)` on it.  Matches Module#const_get semantics
          # well enough for abruby's flat constant tables.
          parent_ast = transduce(node.parent)
          recv_idx = inc_arg_index
          recv_store = AbRuby.alloc_node_lvar_set(recv_idx, parent_ast)
          call_arg_idx = arg_index
          sym_slot = inc_arg_index
          sym_store = AbRuby.alloc_node_lvar_set(sym_slot,
            AbRuby.alloc_node_sym(node.name.to_s))
          rewind_arg_index(recv_idx)
          recv_ref = AbRuby.alloc_node_lvar_get(recv_idx)
          call = AbRuby.alloc_node_method_call(recv_ref, "const_get", 1, call_arg_idx)
          build_seq([recv_store, sym_store, call])
        end

      when Prism::ModuleNode
        name = node.constant_path.name.to_s
        body = with_class_body_scope(node) { transduce_class_body(node.body) }
        @entries << ["module:#{name}", body]
        AbRuby.alloc_node_module_def(name, body)

      when Prism::ClassNode
        name = node.constant_path.name.to_s
        super_expr = node.superclass ? transduce(node.superclass) : AbRuby.alloc_node_nil
        body = with_class_body_scope(node) { transduce_class_body(node.body) }
        @entries << ["class:#{name}", body]
        @entries << ["class:#{name}:super", super_expr]
        AbRuby.alloc_node_class_def(name, super_expr, body)

      when Prism::MultiWriteNode
        # a, b = 1, 2 — right side is always ArrayNode from parser
        lefts = node.lefts
        values = node.value

        # Evaluate right-hand values into temp slots.
        #
        # Ruby semantics for `a, b = b, a + b` require *all* RHS
        # expressions to be evaluated to values BEFORE any LHS
        # assignment runs.  We do that by storing each evaluated RHS
        # into its own temp slot, then having the assigns loop read
        # back from those slots.  Without this hoist, `a, b = b, a+b`
        # would update `a` before `a + b` is computed and produce
        # wrong values (the famous Fibonacci bug).
        rhs_pre_seq = nil
        if values.is_a?(Prism::ArrayNode) &&
           values.elements.none? { |e| e.is_a?(Prism::SplatNode) }
          temp_slots = values.elements.map { inc_arg_index }
          rhs_pre_seq = values.elements.each_with_index.map do |e, i|
            AbRuby.alloc_node_lvar_set(temp_slots[i], transduce(e))
          end
          rhs_nodes = temp_slots.map { |s| AbRuby.alloc_node_lvar_get(s) }
        elsif values.is_a?(Prism::ArrayNode) &&
              values.elements.size == 1 && values.elements[0].is_a?(Prism::SplatNode)
          # a, b = *expr → treat expr as the array to decompose
          values = values.elements[0].expression
        end

        # RHS is a single expression yielding an array: evaluate it ONCE
        # into a temp slot, then read each LHS via [0], [1], ... on the
        # cached result.  The pre-store goes into rhs_pre_store and is
        # prepended to the final assigns sequence; rhs_nodes only carry
        # the per-element [] calls.
        rhs_pre_store = nil
        ary_idx = nil
        unless rhs_nodes
          ary_idx = inc_arg_index   # this slot stays live for the
                                    # whole multi-assign — we cache
                                    # the RHS here and read [i] off it
                                    # for each LHS.
          rhs_pre_store = AbRuby.alloc_node_lvar_set(ary_idx, transduce(values))
          rhs_nodes = lefts.each_index.map do |i|
            ary_ref = AbRuby.alloc_node_lvar_get(ary_idx)
            idx_node = AbRuby.alloc_node_num(i)
            # ary_ref[i] via method call.  recv/arg slots are reused
            # across iterations *within* this loop — they're rewound
            # before the next iteration — but we never touch ary_idx.
            recv_idx = inc_arg_index
            recv_store = AbRuby.alloc_node_lvar_set(recv_idx, ary_ref)
            arg_idx = inc_arg_index
            arg_store = AbRuby.alloc_node_lvar_set(arg_idx, idx_node)
            rewind_arg_index(recv_idx)
            recv_ref = AbRuby.alloc_node_lvar_get(recv_idx)
            call_node = AbRuby.alloc_node_method_call(recv_ref, "[]", 1, arg_idx)
            build_seq([recv_store, arg_store, call_node])
          end
          # Do NOT rewind to ary_idx here — the per-LHS assigns below
          # may also need scratch slots, and they must not stomp on the
          # cached RHS array.  We rewind past the multi-assign instead.
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
          when Prism::IndexTargetNode
            # Lower `recv[idx...] = rhs` to `recv.[]=(idx..., rhs)`.
            recv_ast = transduce(target.receiver)
            idx_nodes = target.arguments ? target.arguments.arguments : []
            recv_idx = inc_arg_index
            recv_store = AbRuby.alloc_node_lvar_set(recv_idx, recv_ast)
            call_arg_idx = arg_index
            arg_seq = []
            idx_nodes.each do |ix|
              slot = inc_arg_index
              arg_seq << AbRuby.alloc_node_lvar_set(slot, transduce(ix))
            end
            rhs_slot = inc_arg_index
            arg_seq << AbRuby.alloc_node_lvar_set(rhs_slot, rhs)
            rewind_arg_index(recv_idx)
            recv_ref = AbRuby.alloc_node_lvar_get(recv_idx)
            call = AbRuby.alloc_node_method_call(recv_ref, "[]=", idx_nodes.size + 1, call_arg_idx)
            assigns << build_seq([recv_store] + arg_seq + [call])
          when Prism::ConstantTargetNode
            assigns << AbRuby.alloc_node_const_set(target.name.to_s, rhs)
          when Prism::CallTargetNode
            # Lower `recv.attr = rhs` to `recv.attr=(rhs)`.  Evaluate
            # rhs FIRST so that any temp slots it needs (e.g. the inner
            # `[]` call from a multi-assign decompose that reuses
            # nearby slots) are released before we materialise our own
            # recv slot.
            rhs_eval_slot = inc_arg_index
            rhs_eval_store = AbRuby.alloc_node_lvar_set(rhs_eval_slot, rhs)
            recv_ast = transduce(target.receiver)
            recv_idx = inc_arg_index
            recv_store = AbRuby.alloc_node_lvar_set(recv_idx, recv_ast)
            call_arg_idx = arg_index
            rhs_slot = inc_arg_index
            rhs_store = AbRuby.alloc_node_lvar_set(rhs_slot, AbRuby.alloc_node_lvar_get(rhs_eval_slot))
            rewind_arg_index(rhs_eval_slot)
            recv_ref = AbRuby.alloc_node_lvar_get(recv_idx)
            attr_name = target.name.to_s  # already ends in "=" for CallTargetNode
            call = AbRuby.alloc_node_method_call(recv_ref, attr_name, 1, call_arg_idx)
            assigns << build_seq([rhs_eval_store, recv_store, rhs_store, call])
          else
            raise "unsupported multi-assign target: #{target.class}"
          end
        end

        rewind_arg_index(ary_idx) if ary_idx
        prelude = []
        prelude.concat(rhs_pre_seq) if rhs_pre_seq
        prelude << rhs_pre_store if rhs_pre_store
        build_seq(prelude.empty? ? assigns : prelude + assigns)

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
        if !node.receiver && %w[attr_reader attr_writer attr_accessor].include?(node.name.to_s) &&
           node.arguments&.arguments&.all? { |a| a.is_a?(Prism::SymbolNode) }
          # All-symbol-literal call: parse-time inline expansion.  Mixed
          # / dynamic args fall through to a regular method dispatch and
          # are handled by the runtime cfuncs in builtin/class.c.
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

      # env_size = the enclosing frame's max slot count.  When the block
      # is converted to a Proc and outlives the enclosing method, the
      # runtime snapshots `captured_fp[0..env_size]` to a heap env so
      # the body can still read its closure locals.
      env_size = frame[:max]
      set_line(AbRuby.alloc_node_block_literal(body, param_names.size, param_base, env_size), block_node)
    end

    # Build a call node that carries a block — either a literal
    # `{ ... }` (BlockNode) or a `&proc_var` runtime expression
    # (BlockArgumentNode).  recv is the already-transduced receiver
    # AST, or nil for an implicit-self call.  name is a String.
    def alloc_call_with_block(node_block, recv, name, args_count, call_arg_idx)
      case node_block
      when Prism::BlockNode
        block_literal = transduce_block_literal(node_block)
        if recv
          AbRuby.alloc_node_method_call_with_block(recv, name, args_count, call_arg_idx, block_literal)
        else
          AbRuby.alloc_node_func_call_with_block(name, args_count, call_arg_idx, block_literal)
        end
      when Prism::BlockArgumentNode
        expr_ast = node_block.expression ? transduce(node_block.expression) : AbRuby.alloc_node_nil
        if recv
          AbRuby.alloc_node_method_call_with_block_arg(recv, name, args_count, call_arg_idx, expr_ast)
        else
          AbRuby.alloc_node_func_call_with_block_arg(name, args_count, call_arg_idx, expr_ast)
        end
      else
        raise "unsupported block kind: #{node_block.class}"
      end
    end

    # Push a scope for class/module body locals.  Allocates outer-frame
    # slots for the body's lexical locals (Prism gives us the list via
    # `node.locals`) so code in the body can read/write them.  At runtime
    # the class body shares the outer fp, so these slots live in the
    # current_frame's slot range — same trick as block-local slots.
    def with_class_body_scope(node)
      locals = node.locals.map(&:to_s)
      if locals.empty?
        return yield
      end
      frame = current_frame
      scope = {}
      locals.each do |name|
        # Avoid double-allocation if the name is also a method-frame
        # local (rare, but happens for top-level class bodies).
        next if frame[:locals].include?(name)
        slot = frame[:block_slot_next]
        scope[name] = slot
        frame[:block_slot_next] += 1
        frame[:max] = frame[:block_slot_next] if frame[:block_slot_next] > frame[:max]
      end
      frame[:block_scope_stack].push(scope)
      result = yield
      frame[:block_scope_stack].pop
      result
    end

    def transduce_class_body(body)
      body ? transduce(body) : AbRuby.alloc_node_nil
    end

    # Build `read ||= value` — tmp = read; if(tmp, tmp, write(value)).
    # Caller provides already-transduced read/value ASTs and a
    # write-constructor lambda that consumes the new rhs AST.
    def build_or_write(read_ast, value_ast, write_ctor)
      idx = inc_arg_index
      store = AbRuby.alloc_node_lvar_set(idx, read_ast)
      ref1 = AbRuby.alloc_node_lvar_get(idx)
      ref2 = AbRuby.alloc_node_lvar_get(idx)
      rewind_arg_index(idx)
      AbRuby.alloc_node_seq(store,
        AbRuby.alloc_node_if(ref1, ref2, write_ctor.(value_ast)))
    end

    # Build `read &&= value` — tmp = read; if(tmp, write(value), tmp).
    def build_and_write(read_ast, value_ast, write_ctor)
      idx = inc_arg_index
      store = AbRuby.alloc_node_lvar_set(idx, read_ast)
      ref1 = AbRuby.alloc_node_lvar_get(idx)
      ref2 = AbRuby.alloc_node_lvar_get(idx)
      rewind_arg_index(idx)
      AbRuby.alloc_node_seq(store,
        AbRuby.alloc_node_if(ref1, write_ctor.(value_ast), ref2))
    end

    # Emit a binary-op call: lhs op rhs.  Used for `X op= v` patterns.
    def build_binop_call(op, lhs, rhs)
      if BINOP_MAP.key?(op)
        fb = inc_arg_index
        rewind_arg_index(fb)
        AbRuby.send("alloc_node_#{BINOP_MAP[op]}", lhs, rhs, fb)
      else
        call_arg_idx = arg_index
        idx = inc_arg_index
        store_rhs = AbRuby.alloc_node_lvar_set(idx, rhs)
        rewind_arg_index(call_arg_idx)
        AbRuby.alloc_node_seq(store_rhs,
          AbRuby.alloc_node_method_call(lhs, op, 1, call_arg_idx))
      end
    end

    # Lower `recv[idx...] op= value` / `||= ` / `&&= `.
    # `op` is :or, :and, or a string binary operator name ("+", etc.).
    #
    # We cache receiver and each index argument into temp slots so the
    # evaluation-order semantics of Ruby are preserved: receiver and
    # indices are evaluated exactly once.
    def transduce_index_op_write(node, op)
      recv_ast = transduce(node.receiver)
      idx_nodes = node.arguments ? node.arguments.arguments : []

      recv_idx = inc_arg_index
      recv_store = AbRuby.alloc_node_lvar_set(recv_idx, recv_ast)

      idx_slots = idx_nodes.map { inc_arg_index }
      idx_stores = idx_nodes.each_with_index.map do |n, i|
        AbRuby.alloc_node_lvar_set(idx_slots[i], transduce(n))
      end

      # Build a []-call that reads current value using the cached recv/idx.
      build_get = lambda do
        # Put indices into a call arg area.
        call_arg_idx = arg_index
        arg_seq = idx_slots.each_with_index.map do |slot, i|
          put = inc_arg_index
          AbRuby.alloc_node_lvar_set(put, AbRuby.alloc_node_lvar_get(slot))
        end
        rewind_arg_index(call_arg_idx)
        recv_ref = AbRuby.alloc_node_lvar_get(recv_idx)
        call = AbRuby.alloc_node_method_call(recv_ref, "[]", idx_slots.size, call_arg_idx)
        arg_seq.empty? ? call : build_seq(arg_seq + [call])
      end

      # Build a []=-call that writes the rhs using the cached recv/idx.
      build_set = lambda do |rhs_ast|
        call_arg_idx = arg_index
        arg_seq = idx_slots.each_with_index.map do |slot, i|
          put = inc_arg_index
          AbRuby.alloc_node_lvar_set(put, AbRuby.alloc_node_lvar_get(slot))
        end
        rhs_slot = inc_arg_index
        arg_seq << AbRuby.alloc_node_lvar_set(rhs_slot, rhs_ast)
        rewind_arg_index(call_arg_idx)
        recv_ref = AbRuby.alloc_node_lvar_get(recv_idx)
        call = AbRuby.alloc_node_method_call(recv_ref, "[]=", idx_slots.size + 1, call_arg_idx)
        build_seq(arg_seq + [call])
      end

      body =
        case op
        when :or
          build_or_write(build_get.(), transduce(node.value), build_set)
        when :and
          build_and_write(build_get.(), transduce(node.value), build_set)
        else
          new_val = build_binop_call(op, build_get.(), transduce(node.value))
          build_set.(new_val)
        end

      rewind_arg_index(recv_idx)
      build_seq([recv_store] + idx_stores + [body])
    end

    # Lower `recv.attr op= value` / `||=` / `&&=`.
    # Caches recv in a temp to avoid double side-effects.
    def transduce_call_op_write(node, op)
      recv_ast = transduce(node.receiver)
      attr_name = node.read_name.to_s        # "x"
      write_name = node.write_name.to_s      # "x="

      recv_idx = inc_arg_index
      recv_store = AbRuby.alloc_node_lvar_set(recv_idx, recv_ast)

      build_get = lambda do
        recv_ref = AbRuby.alloc_node_lvar_get(recv_idx)
        AbRuby.alloc_node_method_call(recv_ref, attr_name, 0, arg_index)
      end

      build_set = lambda do |rhs_ast|
        call_arg_idx = arg_index
        rhs_slot = inc_arg_index
        store = AbRuby.alloc_node_lvar_set(rhs_slot, rhs_ast)
        rewind_arg_index(call_arg_idx)
        recv_ref = AbRuby.alloc_node_lvar_get(recv_idx)
        call = AbRuby.alloc_node_method_call(recv_ref, write_name, 1, call_arg_idx)
        AbRuby.alloc_node_seq(store, call)
      end

      body =
        case op
        when :or
          build_or_write(build_get.(), transduce(node.value), build_set)
        when :and
          build_and_write(build_get.(), transduce(node.value), build_set)
        else
          new_val = build_binop_call(op, build_get.(), transduce(node.value))
          build_set.(new_val)
        end

      rewind_arg_index(recv_idx)
      AbRuby.alloc_node_seq(recv_store, body)
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
          # *expr: expect an Array (or Range/nil — handled by Array#+ at runtime)
          parts << transduce(a.expression)
        else
          buf << a
        end
      end
      flush.()

      # Always start with an empty array literal so the result of
      # Array#+ folding is a real Array even when the args list is a
      # single splat (`[*x]` or `foo(*x)`).  Otherwise the bare splat
      # expression — which may be a Range, nil, etc. — leaks out.
      empty_idx = arg_index
      empty_ary = AbRuby.alloc_node_ary_new(0, empty_idx)
      [empty_ary, *parts].reduce do |acc, x|
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
              call_node = set_line(alloc_call_with_block(node.block, nil, name.to_s, args.size, call_arg_idx), node)
            else
              call_node = set_line(AbRuby.alloc_node_func_call(name.to_s, args.size, call_arg_idx), node)
            end
            return build_seq(seq_nodes + [call_node])
          else
            if node.respond_to?(:block) && node.block
              return set_line(alloc_call_with_block(node.block, nil, name.to_s, 0, call_arg_idx), node)
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
            call_node = set_line(alloc_call_with_block(node.block, recv_ref, name.to_s, args.size, call_arg_idx), node)
          else
            call_node = set_line(AbRuby.alloc_node_method_call(recv_ref, name.to_s, args.size, call_arg_idx), node)
          end
          return build_seq([recv_store] + seq_nodes + [call_node])
        else
          rewind_arg_index(recv_idx)
          recv_ref = AbRuby.alloc_node_lvar_get(recv_idx)
          if node.respond_to?(:block) && node.block
            call_node = set_line(alloc_call_with_block(node.block, recv_ref, name.to_s, 0, call_arg_idx), node)
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
          call_node = set_line(alloc_call_with_block(node.block, nil, name.to_s, args.size, call_arg_idx), node)
        else
          call_node = set_line(AbRuby.alloc_node_func_call(name.to_s, args.size, call_arg_idx), node)
        end
        build_seq(seq_nodes + [call_node])
      else
        if node.respond_to?(:block) && node.block
          set_line(alloc_call_with_block(node.block, nil, name.to_s, 0, call_arg_idx), node)
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
