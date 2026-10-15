# Thread::Backtrace::Location#source_range support (Ruby 4.1), loaded on first use.
#
# CRuby keeps the node id of every backtrace position; koruby's backtrace is
# "path:line:in 'label'" only, so the node is found again in the file with Prism:
# the operation that can raise (a call, an operator assignment, a constant, a
# pattern, a class body, ...) that covers the line, inside the frame's own code —
# the innermost scope of the label's kind (a block, a method, a class body or the
# top level), not inside a nested one — the innermost such node.  When nothing
# there can raise, the frame's own scope (a method or block arity error).
# Ambiguous when one line of one scope holds nested calls (`foo(bar())`): the
# innermost is taken.
require "prism"

module Koruby
  module BacktraceNode
    SCOPES = [Prism::ProgramNode, Prism::DefNode, Prism::BlockNode, Prism::LambdaNode,
              Prism::ClassNode, Prism::ModuleNode, Prism::SingletonClassNode].freeze
    RAISERS = [Prism::CallNode, Prism::CallOperatorWriteNode, Prism::CallOrWriteNode, Prism::CallAndWriteNode,
               Prism::CallTargetNode, Prism::IndexOperatorWriteNode, Prism::IndexOrWriteNode, Prism::IndexAndWriteNode,
               Prism::IndexTargetNode, Prism::ConstantReadNode, Prism::ConstantPathNode, Prism::ConstantPathWriteNode,
               Prism::ConstantPathTargetNode, Prism::ConstantPathOperatorWriteNode, Prism::ConstantPathOrWriteNode,
               Prism::ConstantPathAndWriteNode, Prism::ConstantOperatorWriteNode, Prism::ConstantOrWriteNode,
               Prism::ConstantAndWriteNode, Prism::ClassVariableReadNode, Prism::ClassVariableOperatorWriteNode,
               Prism::ClassVariableOrWriteNode, Prism::ClassVariableAndWriteNode, Prism::LocalVariableOperatorWriteNode,
               Prism::InstanceVariableOperatorWriteNode, Prism::GlobalVariableOperatorWriteNode,
               Prism::HashPatternNode, Prism::ArrayPatternNode, Prism::FindPatternNode, Prism::KeywordHashNode, Prism::InterpolatedSymbolNode, Prism::InterpolatedStringNode,
               Prism::SuperNode, Prism::ForwardingSuperNode, Prism::YieldNode,
               Prism::ClassNode, Prism::ModuleNode, Prism::SingletonClassNode, Prism::DefNode,
               Prism::BlockNode, Prism::LambdaNode].freeze

    CONSTANTS = [Prism::ConstantReadNode, Prism::ConstantPathNode].freeze

    # an implicit conversion that failed (the exception's message, or the method the frame called) is the
    # node that converts, by kind in order of preference
    CONVERSIONS = {
      to_a: [Prism::ArrayNode, Prism::SplatNode], to_ary: [Prism::MultiWriteNode, Prism::MultiTargetNode],
      to_hash: [Prism::KeywordHashNode, Prism::AssocSplatNode, Prism::HashNode],
      hash: [Prism::KeywordHashNode, Prism::HashNode], deconstruct_keys: [Prism::HashPatternNode],
      deconstruct: [Prism::ArrayPatternNode, Prism::FindPatternNode], to_proc: [Prism::BlockArgumentNode],
      to_s: [Prism::InterpolatedStringNode, Prism::InterpolatedSymbolNode], to_sym: [Prism::InterpolatedSymbolNode],
    }.freeze

    # [source, first line] of the code at `path`: the copy kept for eval / -e, else the file read back, which
    # must be what was parsed (CRuby compares a digest).  nil: no source.
    def self.source_of(path, abs)
      kind, kept, first_line, = __source_record(path, nil)
      if kind == 1 || kind == 2
        raise ArgumentError, "cannot get source range for location in eval" unless kept
        return [kept, first_line]
      end
      return nil unless abs
      src = File.binread(abs)
      raise RuntimeError, "source has been modified" if kind == 0 && __source_record(path, src)[3] == false
      [src, 1]
    end

    def self.eval_path?(path)
      kind, = __source_record(path, nil)
      kind == 1 || kind == 2
    end

    # #syntax_tree of a Proc / Method: the node at its #source_range — a block's is the call it is passed to
    # (CRuby's iseq node), never an implicit-parameter node of the same extent
    def self.node_of_range(range)
      return nil unless range
      src, first_line = source_of(range.path, range.absolute_path)
      return nil unless src
      key = [range.start_line, range.start_column, range.end_line, range.end_column]
      stack = [[Prism.parse(src, line: first_line).value, nil]]
      until stack.empty?
        n, parent = stack.pop
        l = n.location
        if [l.start_line, l.start_column, l.end_line, l.end_column] == key
          return parent if n.is_a?(Prism::BlockNode) && parent.is_a?(Prism::CallNode)
          return n if n.is_a?(Prism::DefNode) || n.is_a?(Prism::LambdaNode) || n.is_a?(Prism::ForNode)
        end
        n.compact_child_nodes.each { |ch| stack << [ch, n] }
      end
      nil
    end

    def self.find(loc, source = nil, first_line = 1)
      line = loc.lineno
      result = source ? Prism.parse(source, line: first_line) : Prism.parse_file(loc.absolute_path)
      raise SyntaxError, result.errors.map(&:message).join("\n") unless result.success?
      root = result.value
      kind = scope_kind(loc.label)
      # the scope chain covering the line, outermost first
      chain = [root]
      loop do
        inner = nil
        stack = chain.last.compact_child_nodes.dup
        until stack.empty?
          n = stack.shift
          next unless covers?(n, line)
          if SCOPES.any? { |k| n.is_a?(k) } then inner = n; break end
          stack.concat(n.compact_child_nodes)
        end
        break unless inner
        chain << inner
      end
      hint = name_hint(loc)
      # a method label with no def of that name here (nor a define_method block) is a C method: its position is
      # the call in its caller's code
      if kind.include?(Prism::DefNode) && !cfunc_scope?(chain, loc.base_label)
        caller = loc.instance_variable_get(:@__caller)
        kind = scope_kind(caller ? caller.label : '')
        kind = [Prism::DefNode, Prism::BlockNode, Prism::LambdaNode, Prism::ProgramNode] if kind.include?(Prism::DefNode)
        hint = loc.base_label.to_sym unless (exc = loc.instance_variable_get(:@__exc)) && exc.is_a?(NameError) && exc.name
      end
      # the innermost scope of the label's kind, the kinds tried in order (a method label: its def, else the
      # block a define_method made it from)
      scope = nil
      if kind.include?(Prism::LambdaNode)                 # a block label: the innermost block or lambda
        scope = chain.reverse.find { |s| kind.any? { |k| s.is_a?(k) } }
      else
        kind.each { |k| break if (scope = chain.reverse.find { |s| s.is_a?(k) }) }
      end
      scope ||= chain.last
      # candidates: raisers covering the line in the scope's own code (nested scopes are
      # candidates themselves but are not entered)
      # a `when` value / an `in` pattern called with #===: that value
      if hint == :===
        stack = scope.compact_child_nodes.dup
        until stack.empty?
          n = stack.pop
          next unless covers?(n, line)
          vals = n.is_a?(Prism::WhenNode) ? n.conditions : n.is_a?(Prism::InNode) ? [n.pattern] : nil
          hit = vals&.find { |v| covers?(v, line) }
          return hit if hit
          stack.concat(n.compact_child_nodes) unless SCOPES.any? { |k| n.is_a?(k) }
        end
      end
      if (kinds = CONVERSIONS[hint])
        all = []
        stack = scope.compact_child_nodes.dup
        until stack.empty?
          n = stack.pop
          next unless covers?(n, line)
          all << n
          stack.concat(n.compact_child_nodes) unless SCOPES.any? { |k| n.is_a?(k) }
        end
        # `(a, b), c = x, 1`: the list is spread as it is, so the conversion is the nested target's
        kinds = kinds.reverse if hint == :to_ary && all.any? { |n| n.is_a?(Prism::MultiWriteNode) && n.value.is_a?(Prism::ArrayNode) }
        kinds.each do |k|
          hit = all.select { |n| n.is_a?(k) }.min_by { |n| n.location.end_offset - n.location.start_offset }
          return hit if hit
        end
      end
      cands = []
      stack = scope.compact_child_nodes.dup
      until stack.empty?
        n = stack.pop
        next unless covers?(n, line)
        nested = SCOPES.any? { |k| n.is_a?(k) }
        # a nested class / module body is an operation of this scope (its superclass check, its reopen);
        # a nested block / method / lambda is not
        cands << n if RAISERS.any? { |k| n.is_a?(k) } && !(nested && !n.is_a?(Prism::ClassNode) && !n.is_a?(Prism::ModuleNode) && !n.is_a?(Prism::SingletonClassNode))
        stack.concat(n.compact_child_nodes) unless nested
      end
      # a constant raises only NameError, for its own name
      cands.reject! { |n| CONSTANTS.any? { |k| n.is_a?(k) } && node_name(n) != hint }
      return scope unless scope.is_a?(Prism::ProgramNode) || !cands.empty?
      # the name that failed: the exception's (the raising position), else the method the frame called
      if hint
        named = cands.select { |n| node_name(n) == hint }
        # by name, the outermost: an operator assignment over its own target (`A::B += 1` is the whole
        # assignment, not `A::B`)
        return named.max_by { |n| n.location.end_offset - n.location.start_offset } unless named.empty?
      end
      cands.min_by { |n| n.location.end_offset - n.location.start_offset }
    end

    # the method / constant a node names: an assignment target's own name, an index target's []=
    def self.node_name(n)
      case n
      when Prism::IndexTargetNode, Prism::IndexOperatorWriteNode, Prism::IndexOrWriteNode, Prism::IndexAndWriteNode then :[]=
      when Prism::ConstantPathOperatorWriteNode, Prism::ConstantPathOrWriteNode, Prism::ConstantPathAndWriteNode,
           Prism::ConstantPathWriteNode
        n.target.name
      else n.respond_to?(:name) ? n.name : nil
      end
    end

    # false: no Ruby body for method `name` covers the line (a def of that name, or a block passed to
    # define_method / define_singleton_method)
    def self.cfunc_scope?(chain, name)
      return true if name.nil?
      sym = name.to_sym
      chain.any? { |s| s.is_a?(Prism::DefNode) && s.name == sym } ||
        chain.each_cons(2).any? { |o, s| s.is_a?(Prism::BlockNode) && dm_block?(o, s) }
    end

    def self.dm_block?(outer, blk)
      stack = [outer]
      until stack.empty?
        n = stack.pop
        return %i[define_method define_singleton_method].include?(n.name) if n.is_a?(Prism::CallNode) && n.block.equal?(blk)
        stack.concat(n.compact_child_nodes)
      end
      false
    end

    def self.name_hint(loc)
      exc = loc.instance_variable_get(:@__exc)
      return exc.name if exc.is_a?(NameError) && exc.name
      if exc
        m = exc.message.to_s
        return $1.to_sym if m =~ /\(\S+#(\w+[?!=]?) gives/ || m =~ /\A(\w+) must return/
      end
      callee = loc.instance_variable_get(:@__callee)
      return nil unless callee
      b = callee.base_label
      b && !b.start_with?('<') ? b.to_sym : nil
    end

    def self.covers?(n, line)
      l = n.location
      l.start_line <= line && line <= l.end_line
    end

    def self.scope_kind(label)
      case label.to_s
      when /\Ablock /                then [Prism::BlockNode, Prism::LambdaNode]
      when /\A<top/, /\A<main>/, ''  then [Prism::ProgramNode]
      when /\A<class:/               then [Prism::ClassNode]
      when /\A<module:/              then [Prism::ModuleNode]
      when /\Asingleton class/       then [Prism::SingletonClassNode]
      else                                [Prism::DefNode, Prism::BlockNode]   # define_method bodies are blocks
      end
    end
  end
end
