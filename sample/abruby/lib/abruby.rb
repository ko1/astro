require 'prism'
require_relative '../abruby'

class AbRuby
  class Parser
    def initialize
      @frames = []
    end

    def parse(code)
      result = Prism.parse(code)
      unless result.success?
        raise SyntaxError, "parse error: #{result.errors.map(&:message).join(', ')}"
      end
      transduce(result.value)
    end

    private

    def push_frame(locals)
      @frames.push({ locals: locals.map(&:to_s), arg_index: locals.size, max: locals.size })
    end

    def pop_frame
      @frames.pop
    end

    def current_frame
      @frames.last
    end

    def lvar_index(name)
      idx = current_frame[:locals].index(name.to_s)
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
        push_frame(node.locals)
        body = transduce(node.statements)
        frame = pop_frame
        AbRuby.alloc_node_scope(frame[:max], body)

      when Prism::StatementsNode
        stmts = node.body.map { |n| transduce(n) }
        build_seq(stmts)

      when Prism::FloatNode
        AbRuby.alloc_node_float(node.value.to_s)

      when Prism::IntegerNode
        v = node.value
        if v >= -(2**30) && v < 2**30
          AbRuby.alloc_node_num(v)
        else
          AbRuby.alloc_node_bignum(v.to_s)
        end

      when Prism::StringNode
        AbRuby.alloc_node_str(node.unescaped)

      when Prism::InterpolatedStringNode
        # "hello #{expr} world" → "hello " + expr.to_s + " world"
        # Reserve dedicated temp slots for the whole interpolation
        base_idx = arg_index
        tmp_recv = inc_arg_index  # slot for left/recv of +
        tmp_arg  = inc_arg_index  # slot for right/arg of +
        tmp_to_s = inc_arg_index  # slot for to_s receiver
        _tmp_pad = inc_arg_index  # padding slot for to_s arg_index

        parts = node.parts.map do |part|
          case part
          when Prism::StringNode
            AbRuby.alloc_node_str(part.unescaped)
          when Prism::EmbeddedStatementsNode
            inner = part.statements ? transduce(part.statements) : AbRuby.alloc_node_str("")
            # call to_s on the result
            recv_store = AbRuby.alloc_node_lset(tmp_to_s, inner)
            recv_ref = AbRuby.alloc_node_lget(tmp_to_s)
            to_s_call = AbRuby.alloc_node_method_call(recv_ref, "to_s", 0, tmp_to_s + 1)
            AbRuby.alloc_node_seq(recv_store, to_s_call)
          else
            raise "unsupported interpolation part: #{part.class}"
          end
        end

        rewind_arg_index(base_idx)

        # concatenate with +
        parts.reduce do |left, right|
          recv_store = AbRuby.alloc_node_lset(tmp_recv, left)
          arg_store = AbRuby.alloc_node_lset(tmp_arg, right)
          recv_ref = AbRuby.alloc_node_lget(tmp_recv)
          concat = AbRuby.alloc_node_method_call(recv_ref, "+", 1, tmp_arg)
          build_seq([recv_store, arg_store, concat])
        end

      when Prism::EmbeddedStatementsNode
        node.statements ? transduce(node.statements) : AbRuby.alloc_node_nil

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
        AbRuby.alloc_node_gget(node.name.to_s)

      when Prism::GlobalVariableWriteNode
        AbRuby.alloc_node_gset(node.name.to_s, transduce(node.value))

      when Prism::GlobalVariableOperatorWriteNode
        op = node.binary_operator.to_s
        call_arg_idx = arg_index
        idx = inc_arg_index
        store_rhs = AbRuby.alloc_node_lset(idx, transduce(node.value))
        rewind_arg_index(call_arg_idx)

        recv = AbRuby.alloc_node_gget(node.name.to_s)
        call_node = AbRuby.alloc_node_method_call(recv, op, 1, call_arg_idx)
        AbRuby.alloc_node_gset(node.name.to_s,
          AbRuby.alloc_node_seq(store_rhs, call_node))

      when Prism::InstanceVariableReadNode
        AbRuby.alloc_node_ivar_get(node.name.to_s)

      when Prism::InstanceVariableWriteNode
        AbRuby.alloc_node_ivar_set(node.name.to_s, transduce(node.value))

      when Prism::InstanceVariableOperatorWriteNode
        op = node.binary_operator.to_s
        call_arg_idx = arg_index
        idx = inc_arg_index
        store_rhs = AbRuby.alloc_node_lset(idx, transduce(node.value))
        rewind_arg_index(call_arg_idx)

        recv = AbRuby.alloc_node_ivar_get(node.name.to_s)
        call_node = AbRuby.alloc_node_method_call(recv, op, 1, call_arg_idx)
        AbRuby.alloc_node_ivar_set(node.name.to_s,
          AbRuby.alloc_node_seq(store_rhs, call_node))

      when Prism::LocalVariableReadNode
        AbRuby.alloc_node_lget(lvar_index(node.name))

      when Prism::LocalVariableWriteNode
        AbRuby.alloc_node_lset(lvar_index(node.name), transduce(node.value))

      when Prism::LocalVariableOperatorWriteNode
        # a += 1 => a = a.+(1)
        op = node.binary_operator.to_s
        call_arg_idx = arg_index
        idx = inc_arg_index
        store_rhs = AbRuby.alloc_node_lset(idx, transduce(node.value))
        rewind_arg_index(call_arg_idx)

        recv = AbRuby.alloc_node_lget(lvar_index(node.name))
        call_node = AbRuby.alloc_node_method_call(recv, op, 1, call_arg_idx)
        AbRuby.alloc_node_lset(lvar_index(node.name),
          AbRuby.alloc_node_seq(store_rhs, call_node))

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
        store = AbRuby.alloc_node_lset(idx, transduce(node.left))
        ref = AbRuby.alloc_node_lget(idx)
        ref2 = AbRuby.alloc_node_lget(idx)
        rewind_arg_index(idx)
        AbRuby.alloc_node_seq(store, AbRuby.alloc_node_if(ref, transduce(node.right), ref2))

      when Prism::OrNode
        # a || b → tmp = a; if(tmp, tmp, b)
        idx = inc_arg_index
        store = AbRuby.alloc_node_lset(idx, transduce(node.left))
        ref = AbRuby.alloc_node_lget(idx)
        ref2 = AbRuby.alloc_node_lget(idx)
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

      when Prism::ReturnNode
        value = if node.arguments
                  args = node.arguments.arguments
                  if args.size == 1
                    transduce(args[0])
                  else
                    raise "return with multiple values not supported"
                  end
                else
                  AbRuby.alloc_node_nil
                end
        AbRuby.alloc_node_return(value)

      when Prism::DefNode
        name = node.name.to_s
        params = node.parameters
        params_cnt = params ? params.requireds.size : 0

        push_frame(node.locals)
        body = node.body ? transduce(node.body) : AbRuby.alloc_node_nil
        frame = pop_frame

        AbRuby.alloc_node_def(name, body, params_cnt, frame[:max])

      when Prism::ArrayNode
        elements = node.elements
        base_idx = arg_index
        seq_nodes = elements.map do |elem|
          idx = inc_arg_index
          AbRuby.alloc_node_lset(idx, transduce(elem))
        end
        rewind_arg_index(base_idx)
        ary_node = AbRuby.alloc_node_array_new(elements.size, base_idx)
        seq_nodes.empty? ? ary_node : build_seq(seq_nodes + [ary_node])

      when Prism::HashNode
        pairs = node.elements
        base_idx = arg_index
        seq_nodes = []
        pairs.each do |assoc|
          k_idx = inc_arg_index
          v_idx = inc_arg_index
          seq_nodes << AbRuby.alloc_node_lset(k_idx, transduce(assoc.key))
          seq_nodes << AbRuby.alloc_node_lset(v_idx, transduce(assoc.value))
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
        AbRuby.alloc_node_range(b, e, exclude)

      when Prism::RegularExpressionNode
        flags = extract_regexp_flags(node)
        AbRuby.alloc_node_regexp(node.unescaped, flags)

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
        AbRuby.alloc_node_module_def(name, body)

      when Prism::ClassNode
        name = node.constant_path.name.to_s
        super_name = node.superclass.is_a?(Prism::ConstantReadNode) ? node.superclass.name.to_s : ""
        body = node.body ? transduce(node.body) : AbRuby.alloc_node_nil
        AbRuby.alloc_node_class_def(name, super_name, body)

      when Prism::MultiWriteNode
        # a, b = 1, 2 — right side is always ArrayNode from parser
        lefts = node.lefts
        values = node.value

        # Evaluate right-hand values into temp slots
        if values.is_a?(Prism::ArrayNode)
          rhs_nodes = values.elements.map { |e| transduce(e) }
        else
          raise "unsupported multi-assign RHS: #{values.class}"
        end

        assigns = []
        lefts.each_with_index do |target, i|
          rhs = i < rhs_nodes.size ? rhs_nodes[i] : AbRuby.alloc_node_nil
          case target
          when Prism::LocalVariableTargetNode
            assigns << AbRuby.alloc_node_lset(lvar_index(target.name), rhs)
          when Prism::InstanceVariableTargetNode
            assigns << AbRuby.alloc_node_ivar_set(target.name.to_s, rhs)
          when Prism::GlobalVariableTargetNode
            assigns << AbRuby.alloc_node_gset(target.name.to_s, rhs)
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

      when Prism::CallNode
        transduce_call(node)

      else
        raise "unsupported node: #{node.class} (#{node.type})"
      end
    end

    def transduce_call(node)
      name = node.name
      args = node.arguments&.arguments || []

      # method call with receiver: obj.method(args)
      if node.receiver
        # Reserve a slot for the receiver result to avoid slot collision
        # with args that may contain nested calls
        recv_idx = inc_arg_index
        recv_store = AbRuby.alloc_node_lset(recv_idx, transduce(node.receiver))

        call_arg_idx = arg_index
        if args.any?
          seq_nodes = args.map do |arg|
            idx = inc_arg_index
            AbRuby.alloc_node_lset(idx, transduce(arg))
          end
          rewind_arg_index(recv_idx)

          recv_ref = AbRuby.alloc_node_lget(recv_idx)
          call_node = AbRuby.alloc_node_method_call(recv_ref, name.to_s, args.size, call_arg_idx)
          return build_seq([recv_store] + seq_nodes + [call_node])
        else
          rewind_arg_index(recv_idx)
          recv_ref = AbRuby.alloc_node_lget(recv_idx)
          call_node = AbRuby.alloc_node_method_call(recv_ref, name.to_s, 0, call_arg_idx)
          return AbRuby.alloc_node_seq(recv_store, call_node)
        end
      end

      # function call (no receiver) → self.method(args)
      self_node = AbRuby.alloc_node_self
      recv_idx = inc_arg_index
      recv_store = AbRuby.alloc_node_lset(recv_idx, self_node)
      call_arg_idx = arg_index

      if args.any?
        seq_nodes = args.map do |arg|
          idx = inc_arg_index
          AbRuby.alloc_node_lset(idx, transduce(arg))
        end
        rewind_arg_index(recv_idx)

        recv_ref = AbRuby.alloc_node_lget(recv_idx)
        call_node = AbRuby.alloc_node_method_call(recv_ref, name.to_s, args.size, call_arg_idx)
        build_seq([recv_store] + seq_nodes + [call_node])
      else
        rewind_arg_index(recv_idx)
        recv_ref = AbRuby.alloc_node_lget(recv_idx)
        call_node = AbRuby.alloc_node_method_call(recv_ref, name.to_s, 0, call_arg_idx)
        AbRuby.alloc_node_seq(recv_store, call_node)
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

    def build_seq(nodes)
      return AbRuby.alloc_node_nil if nodes.empty?
      return nodes.first if nodes.size == 1

      nodes[0..-2].reverse.reduce(nodes.last) do |tail, head|
        AbRuby.alloc_node_seq(head, tail)
      end
    end
  end

  # Instance methods
  def eval(code)
    ast = Parser.new.parse(code)
    eval_ast(ast)
  end

  def dump(code, pretty: false)
    ast = Parser.new.parse(code)
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
