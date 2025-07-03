#
# This code is obsolete.
#

require 'prism'

class NaRubyCodeGen
  include Prism

  def initialize
    @statements = [] # [[name, expr]]

    # per frames
    @frames = [] # [{lvars: [...], arg_index: lvars.size}
  end

  def (ALLOC = Object.new).method_missing mid, *args
    raise "unsupported: #{mid}" unless /^node_/ =~ mid
    "ALLOC_#{mid}(#{args.join(', ')})"
  end

  BINOP2NAME = {
    :+ => :add,
    :- => :sub,
    :* => :mul,
    :/ => :div,
    :< => :lt,
    :<= => :le,
    :> => :gt,
    :>= => :gt,
  }

  def ALLOC.node_binop op, lhs, rhs
    __send__ "node_#{BINOP2NAME[op]}", lhs, rhs
  end

  def alloc
    ALLOC
  end

  def add_statement stmt
    name = "_stmt_#{@statements.size}_"
    @statements << [name, stmt]
    name
  end

  def scope lvars
    @frames << {lvars: lvars, arg_index: lvars.size, max: lvars.size}
    [yield, @frames.last[:max]]
  ensure
    pp @frames.pop
  end

  def lvars
    @frames.last[:lvars]
  end

  def inc_arg_index
    cnt = @frames.last[:arg_index]
    @frames.last[:arg_index] += 1
    @frames.last[:max] = [@frames.last[:arg_index], @frames.last[:max]].max
    cnt
  end

  def arg_index
    @frames.last[:arg_index]
  end

  def rewind_arg_index idx
    @frames.last[:arg_index] = idx
  end

  def push_arg node
    arg = transduce node
    alloc.node_lset(inc_arg_index, arg)
  end

  def lvar_index name
    lvars.find_index(name) or raise("Unknown lvar: #{name}")
  end

  def transduce node
    p node.type

    case node
############################################
    when ProgramNode
      body, max_cnt = scope node.locals do
        transduce node.statements
      end
      p max_cnt
      body
    when StatementsNode
      last_stmt = nil
      node.body.reverse.each do
        if last_stmt
          next_stmt = add_statement last_stmt
          last_stmt = alloc.node_seq(transduce(it), next_stmt)
        else
          last_stmt = transduce(it)
        end
      end
      add_statement last_stmt
    when ParenthesesNode
      transduce node.body
    when IfNode
      alloc.node_if transduce(node.predicate), transduce(node.statements), transduce(node.subsequent)
    when ElseNode
      transduce(node.statements)
    when WhileNode
      alloc.node_while transduce(node.predicate), transduce(node.statements)
############################################
    when LocalVariableReadNode
      alloc.node_lget lvar_index(node.name)
    when LocalVariableWriteNode
      alloc.node_lset lvar_index(node.name), transduce(node.value)
############################################
    when IntegerNode
      alloc.node_num(node.value)
############################################
    when DefNode
      p node.locals
      body, max_cnt = scope node.locals do
        transduce(node.body)
      end
      params_cnt = node.parameters ? node.parameters.requireds.size : 0
      alloc.node_def node.name.to_s.dump, body, params_cnt, max_cnt

    when CallNode
      case node.name
      when :+, :-, :*, :/, :%, :<, :<=, :>, :>=
        case node.arguments
        in [rhs]
          lhs = node.receiver
          alloc.node_binop node.name, transduce(lhs), transduce(rhs)
        end
      when :p
        alloc.node_p transduce(node.arguments.arguments[0])
      else
        if node.arguments
          call_arg_index = arg_index
          begin
            last_arg = nil
            node.arguments.arguments.each do
              if last_arg
                last_arg = alloc.node_seq last_arg, push_arg(it)
              else
                last_arg = push_arg it
              end
            end
            call_body = alloc.node_call(node.name.to_s.dump, node.arguments.arguments.size, call_arg_index)
            alloc.node_seq last_arg, call_body
          ensure
            rewind_arg_index call_arg_index
          end
        else
          alloc.node_call(node.name.to_s.dump, 0, arg_index)
        end
      end
    when LocalVariableOperatorWriteNode
      alloc.node_lset lvar_index(node.name), alloc.node_binop(node.binary_operator,
                                                              alloc.node_lget(lvar_index(node.name)),
                                                              transduce(node.value))
############################################
    else
      raise node.inspect
    end
  end

  def compile code
    compiled_code = transduce(Prism.parse(code).value)
    stmts = @statements.map{|(name, stmt)|
      "NODE *#{name} = #{stmt};"
    }.join("\n");

    File.write "naruby_code.c", <<~C
      #{stmts}
      NODE *ast = #{compiled_code};
    C
  end

  def self.compile code
    self.new.compile code
  end
end

if $0 == __FILE__
  NaRubyCodeGen.compile File.read('test.nota.rb')
end
