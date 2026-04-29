#!/usr/bin/env ruby
# parse.rb: tree-sitter-c -> typed S-expression IR for castro.
#
# Output IR (S-expression):
#   (program
#     (func NAME PARAMS_CNT LOCALS_CNT RET_TY EXPR)
#     ...)
# Types: i = int (int64-backed), d = double, v = void.

require 'tree_sitter'

LANG = TreeSitter.lang('c')
PARSER = TreeSitter::Parser.new
PARSER.language = LANG

class CompileError < StandardError; end

GLOBAL_FUNCS = {}    # name -> [ret_ty, [param_tys]]

# ---------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------

def text(node, src)
  src.byteslice(node.start_byte, node.end_byte - node.start_byte).force_encoding('UTF-8')
end

def parse_type(node, src)
  return 'i' if node.nil?
  t = text(node, src).strip
  %w[const\  volatile\  register\  static\  unsigned\  signed\ ].each do |q|
    while t.start_with?(q)
      t = t[q.length..]
    end
  end
  return 'd' if %w[double float].include?(t) || t == 'long double'
  return 'v' if t == 'void'
  'i'
end

def cast(expr, frm, to)
  return expr if frm == to
  return [:cast_id, expr] if frm == 'i' && to == 'd'
  return [:cast_di, expr] if frm == 'd' && to == 'i'
  return [:drop, expr]    if to == 'v'
  raise CompileError, "can't cast from void" if frm == 'v'
  expr
end

def to_bool(expr, ty)
  return expr if ty == 'i'
  return [:neq_d, expr, [:lit_d, 0.0]] if ty == 'd'
  raise CompileError, "can't booleanize void"
end

def parse_int_literal(t)
  tx = t.downcase.gsub(/[ul]+$/, '')
  if tx.start_with?('0x') then tx[2..].to_i(16)
  elsif tx.start_with?('0b') then tx[2..].to_i(2)
  elsif tx.start_with?('0') && tx.length > 1 then tx.to_i(8)
  else tx.to_i
  end
end

CHAR_ESC = {
  'n' => 10, 't' => 9, 'r' => 13, '0' => 0, '\\' => 92,
  "'" => 39, '"' => 34, 'a' => 7, 'b' => 8, 'f' => 12, 'v' => 11
}

def parse_char_literal(t)
  s = t[1..-2]
  if s.start_with?('\\') then CHAR_ESC[s[1]] || s[1].ord
  else s.ord
  end
end

# Drill through pointer_declarator/parenthesized_declarator/function_declarator
# to the function_declarator level, returning [name, parameter_list_node].
def unwrap_decl(decl, src)
  while decl
    case decl.type.to_s
    when 'function_declarator'
      inner = decl.child_by_field_name('declarator')
      params = decl.child_by_field_name('parameters')
      return nil if inner.nil?
      while inner && inner.type.to_s != 'identifier'
        inner = inner.child_by_field_name('declarator')
      end
      return nil if inner.nil?
      return [text(inner, src), params]
    end
    nxt = decl.child_by_field_name('declarator')
    return nil if nxt.nil? || nxt.equal?(decl)
    decl = nxt
  end
  nil
end

# ---------------------------------------------------------------------
# Func: per-function symbol table
# ---------------------------------------------------------------------

class Func
  attr_reader :name, :ret_ty, :params, :locals_map
  attr_accessor :next_local
  def initialize(name, ret_ty)
    @name = name
    @ret_ty = ret_ty
    @params = []
    @locals_map = {}
    @next_local = 0
  end

  def add_param(name, ty)
    idx = @next_local
    @locals_map[name] = [idx, ty]
    @params << [name, ty]
    @next_local += 1
    idx
  end

  def add_local(name, ty)
    idx = @next_local
    @locals_map[name] = [idx, ty]
    @next_local += 1
    idx
  end
end

# ---------------------------------------------------------------------
# emitter
# ---------------------------------------------------------------------

def emit_sx(node, out)
  case node
  when Array
    out << '('
    node.each_with_index do |x, i|
      out << ' ' if i > 0
      emit_sx(x, out)
    end
    out << ')'
  when Symbol
    out << node.to_s
  when String
    if node.empty? || node.match?(/[\s()"]/)
      out << '"' << node.gsub('\\', '\\\\').gsub('"', '\\"') << '"'
    else
      out << node
    end
  when true  then out << '1'
  when false then out << '0'
  when Integer then out << node.to_s
  when Float
    # repr that strtod can parse back
    out << format('%.17g', node)
  else
    raise CompileError, "emit: unsupported #{node.inspect}"
  end
end

# ---------------------------------------------------------------------
# expression compiler
# ---------------------------------------------------------------------

# Returns [sexpr, type].
def compile_expr(node, fn, src)
  k = node.type.to_s

  case k
  when 'number_literal'
    t = text(node, src)
    if t.include?('.') || t.downcase.include?('e') || t.match?(/[fF]$/)
      return [[:lit_d, t.gsub(/[fFlL]$/, '').to_f], 'd']
    end
    v = parse_int_literal(t)
    if (-2**31...2**31).cover?(v)
      [[:lit_i, v], 'i']
    else
      [[:lit_i64, v & ((1 << 64) - 1)], 'i']
    end

  when 'char_literal'
    [[:lit_i, parse_char_literal(text(node, src))], 'i']

  when 'true'  then [[:lit_i, 1], 'i']
  when 'false' then [[:lit_i, 0], 'i']
  when 'null'  then [[:lit_i, 0], 'i']

  when 'identifier'
    name = text(node, src)
    if fn.locals_map.key?(name)
      idx, ty = fn.locals_map[name]
      [[:lget, idx], ty]
    else
      raise CompileError, "undefined identifier: #{name}"
    end

  when 'parenthesized_expression'
    node.each_named { |c| return compile_expr(c, fn, src) }
    raise CompileError, 'empty parenthesized'

  when 'binary_expression'
    op = text(node.child_by_field_name('operator'), src)
    ls, lt = compile_expr(node.child_by_field_name('left'), fn, src)
    rs, rt = compile_expr(node.child_by_field_name('right'), fn, src)
    if %w[&& ||].include?(op)
      tag = op == '&&' ? :land : :lor
      return [[tag, to_bool(ls, lt), to_bool(rs, rt)], 'i']
    end
    if %w[& | ^ << >> %].include?(op)
      ls = cast(ls, lt, 'i'); rs = cast(rs, rt, 'i')
      omap = { '&' => :band, '|' => :bor, '^' => :bxor, '<<' => :shl, '>>' => :shr, '%' => :mod_i }
      return [[omap[op], ls, rs], 'i']
    end
    promoted = (lt == 'd' || rt == 'd') ? 'd' : 'i'
    ls = cast(ls, lt, promoted); rs = cast(rs, rt, promoted)
    suf = '_' + promoted
    if %w[+ - * /].include?(op)
      omap = { '+' => 'add', '-' => 'sub', '*' => 'mul', '/' => 'div' }
      return [[(omap[op] + suf).to_sym, ls, rs], promoted]
    end
    if %w[< <= > >= == !=].include?(op)
      omap = { '<' => 'lt', '<=' => 'le', '>' => 'gt', '>=' => 'ge', '==' => 'eq', '!=' => 'neq' }
      return [[(omap[op] + suf).to_sym, ls, rs], 'i']
    end
    raise CompileError, "binop #{op} not supported"

  when 'unary_expression'
    op = text(node.child_by_field_name('operator'), src)
    es, et = compile_expr(node.child_by_field_name('argument'), fn, src)
    case op
    when '-'
      return [[:neg_d, es], 'd'] if et == 'd'
      [[:neg_i, cast(es, et, 'i')], 'i']
    when '+'
      [es, et]
    when '!'
      [[:lnot, to_bool(es, et)], 'i']
    when '~'
      [[:bnot, cast(es, et, 'i')], 'i']
    else
      raise CompileError, "unary #{op}"
    end

  when 'update_expression'
    # ++x / x++ / --x / x--
    op_node = nil
    arg_node = nil
    prefix = false
    node.each do |c|
      if %w[++ --].include?(c.type.to_s)
        op_node = c
      elsif c.named?
        arg_node = c
      end
    end
    if op_node && arg_node && op_node.start_byte < arg_node.start_byte
      prefix = true
    end
    raise CompileError, 'update_expression: only simple identifiers' unless arg_node && arg_node.type.to_s.to_s == 'identifier'
    name = text(arg_node, src)
    raise CompileError, "undefined: #{name}" unless fn.locals_map.key?(name)
    idx, ty = fn.locals_map[name]
    delta = op_node.type.to_s.to_s == '++' ? 1 : -1
    if ty == 'd'
      new_val = [:add_d, [:lget, idx], [:lit_d, delta.to_f]]
      old_recover = [:sub_d, [:lget, idx], [:lit_d, delta.to_f]]
    else
      new_val = [:add_i, [:lget, idx], [:lit_i, delta]]
      old_recover = [:sub_i, [:lget, idx], [:lit_i, delta]]
    end
    if prefix
      [[:lset, idx, new_val], ty]
    else
      [[:seq, [:lset, idx, new_val], old_recover], ty]
    end

  when 'assignment_expression'
    op = text(node.child_by_field_name('operator'), src)
    l_node = node.child_by_field_name('left')
    r_node = node.child_by_field_name('right')
    while l_node.type.to_s.to_s == 'parenthesized_expression'
      ll = nil
      l_node.each_named { |c| ll = c; break }
      l_node = ll
    end
    raise CompileError, 'lhs must be a simple identifier (Phase 1)' unless l_node.type.to_s.to_s == 'identifier'
    name = text(l_node, src)
    raise CompileError, "undefined: #{name}" unless fn.locals_map.key?(name)
    idx, lt = fn.locals_map[name]
    rs, rt = compile_expr(r_node, fn, src)
    if op == '='
      return [[:lset, idx, cast(rs, rt, lt)], lt]
    end
    comp = { '+=' => '+', '-=' => '-', '*=' => '*', '/=' => '/', '%=' => '%',
             '&=' => '&', '|=' => '|', '^=' => '^', '<<=' => '<<', '>>=' => '>>' }
    if comp.key?(op)
      base = comp[op]
      if %w[+ - * /].include?(base)
        promoted = (lt == 'd' || rt == 'd') ? 'd' : 'i'
        ls2 = cast([:lget, idx], lt, promoted)
        rs2 = cast(rs, rt, promoted)
        omap = { '+' => 'add', '-' => 'sub', '*' => 'mul', '/' => 'div' }
        e = [(omap[base] + ('_' + promoted)).to_sym, ls2, rs2]
        return [[:lset, idx, cast(e, promoted, lt)], lt]
      else
        ls2 = cast([:lget, idx], lt, 'i')
        rs2 = cast(rs, rt, 'i')
        omap = { '%' => :mod_i, '&' => :band, '|' => :bor, '^' => :bxor, '<<' => :shl, '>>' => :shr }
        e = [omap[base], ls2, rs2]
        return [[:lset, idx, cast(e, 'i', lt)], lt]
      end
    end
    raise CompileError, "unsupported assign op #{op}"

  when 'call_expression'
    callee = node.child_by_field_name('function')
    raise CompileError, 'call: only direct calls supported' unless callee.type.to_s == 'identifier'
    fname = text(callee, src)
    args_node = node.child_by_field_name('arguments')
    args = []
    args_node.each_named { |a| args << a }
    raise CompileError, "call to undefined function: #{fname}" unless GLOBAL_FUNCS.key?(fname)
    ret_ty, param_tys = GLOBAL_FUNCS[fname]
    raise CompileError, "arity mismatch: #{fname}" if args.length != param_tys.length
    # Reserve arg_index..arg_index+nargs-1 during arg compilation so any
    # nested call's scratch lands above this range and can't trample
    # our partial results.
    arg_index = fn.next_local
    fn.next_local += args.length
    seq_ops = []
    args.each_with_index do |a, i|
      asx, ats = compile_expr(a, fn, src)
      seq_ops << [:lset, arg_index + i, cast(asx, ats, param_tys[i])]
    end
    fn.next_local -= args.length
    call_expr = [:call, fname, args.length, arg_index]
    chain = call_expr
    seq_ops.reverse_each { |s| chain = [:seq, s, chain] }
    [chain, ret_ty]

  when 'conditional_expression'
    children = []
    node.each_named { |c| children << c }
    cs_, ct_ = compile_expr(children[0], fn, src)
    ts_, tt_ = compile_expr(children[1], fn, src)
    es_, et_ = compile_expr(children[2], fn, src)
    promoted = (tt_ == 'd' || et_ == 'd') ? 'd' : 'i'
    [[:ternary, to_bool(cs_, ct_), cast(ts_, tt_, promoted), cast(es_, et_, promoted)], promoted]

  when 'cast_expression'
    ty = parse_type(node.child_by_field_name('type'), src)
    es, et = compile_expr(node.child_by_field_name('value'), fn, src)
    [cast(es, et, ty), ty]

  when 'comma_expression'
    ls, _ = compile_expr(node.child_by_field_name('left'), fn, src)
    rs, rt = compile_expr(node.child_by_field_name('right'), fn, src)
    [[:seq, ls, rs], rt]

  when 'sizeof_expression'
    [[:lit_i, 8], 'i']

  else
    raise CompileError, "unhandled expr: #{k}"
  end
end

# ---------------------------------------------------------------------
# statement compiler
# ---------------------------------------------------------------------

def compile_stmt(node, fn, src)
  k = node.type.to_s
  case k
  when 'compound_statement'
    stmts = []
    node.each_named { |c| stmts << compile_stmt(c, fn, src) }
    return [:nop] if stmts.empty?
    out = stmts.last
    stmts[0...-1].reverse_each { |s| out = [:seq, s, out] }
    out

  when 'declaration'
    ty = parse_type(node.child_by_field_name('type'), src)
    seq_ops = []
    node.each_named do |c|
      case c.type.to_s
      when 'init_declarator'
        d = c.child_by_field_name('declarator')
        v = c.child_by_field_name('value')
        raise CompileError, 'declaration: only simple declarators in Phase 1' unless d.type.to_s == 'identifier'
        idx = fn.add_local(text(d, src), ty)
        vs, vt = compile_expr(v, fn, src)
        seq_ops << [:lset, idx, cast(vs, vt, ty)]
      when 'identifier'
        idx = fn.add_local(text(c, src), ty)
        if ty == 'd'
          seq_ops << [:lset, idx, [:lit_d, 0.0]]
        else
          seq_ops << [:lset, idx, [:lit_i, 0]]
        end
      end
    end
    return [:nop] if seq_ops.empty?
    out = seq_ops.last
    seq_ops[0...-1].reverse_each { |s| out = [:seq, s, out] }
    [:drop, out]

  when 'expression_statement'
    node.each_named do |c|
      es, _ = compile_expr(c, fn, src)
      return [:drop, es]
    end
    [:nop]

  when 'return_statement'
    cnt = 0
    node.each_named { cnt += 1 }
    return [:return_void] if cnt.zero?
    e = nil
    node.each_named { |c| e = c; break }
    es, et = compile_expr(e, fn, src)
    [:return, cast(es, et, fn.ret_ty)]

  when 'if_statement'
    cs, ct = compile_expr(node.child_by_field_name('condition'), fn, src)
    ts = compile_stmt(node.child_by_field_name('consequence'), fn, src)
    else_node = node.child_by_field_name('alternative')
    es = else_node ? compile_stmt(else_node, fn, src) : [:nop]
    [:if, to_bool(cs, ct), ts, es]

  when 'while_statement'
    cs, ct = compile_expr(node.child_by_field_name('condition'), fn, src)
    body = compile_stmt(node.child_by_field_name('body'), fn, src)
    [:while, to_bool(cs, ct), body]

  when 'do_statement'
    body = compile_stmt(node.child_by_field_name('body'), fn, src)
    cs, ct = compile_expr(node.child_by_field_name('condition'), fn, src)
    [:do_while, body, to_bool(cs, ct)]

  when 'for_statement'
    init = node.child_by_field_name('initializer')
    cond = node.child_by_field_name('condition')
    upd  = node.child_by_field_name('update')
    body = node.child_by_field_name('body')
    init_sx =
      if init.nil? then [:nop]
      elsif init.type.to_s == 'declaration' then compile_stmt(init, fn, src)
      else
        es, _ = compile_expr(init, fn, src)
        [:drop, es]
      end
    cs =
      if cond.nil? then [:lit_i, 1]
      else
        cs_, ct_ = compile_expr(cond, fn, src)
        to_bool(cs_, ct_)
      end
    upd_sx =
      if upd.nil? then [:nop]
      else
        es, _ = compile_expr(upd, fn, src)
        [:drop, es]
      end
    body_sx = compile_stmt(body, fn, src)
    [:for, init_sx, cs, upd_sx, body_sx]

  when 'else_clause'
    inner = nil
    node.each_named { |c| inner = c; break }
    return [:nop] if inner.nil?
    compile_stmt(inner, fn, src)

  when 'comment', ';'
    [:nop]

  else
    raise CompileError, "unhandled stmt: #{k}"
  end
end

# ---------------------------------------------------------------------
# top-level
# ---------------------------------------------------------------------

def gather_signatures(root, src)
  root.each_named do |fdef|
    next unless fdef.type.to_s == 'function_definition'
    ret_ty = parse_type(fdef.child_by_field_name('type'), src)
    nm_pa = unwrap_decl(fdef.child_by_field_name('declarator'), src)
    next if nm_pa.nil?
    name, params = nm_pa
    param_tys = []
    if params
      params.each_named do |p|
        param_tys << parse_type(p.child_by_field_name('type'), src) if p.type.to_s == 'parameter_declaration'
      end
    end
    GLOBAL_FUNCS[name] = [ret_ty, param_tys]
  end
end

def compile_function(fdef, src)
  ret_ty = parse_type(fdef.child_by_field_name('type'), src)
  nm_pa = unwrap_decl(fdef.child_by_field_name('declarator'), src)
  raise CompileError, 'function with no name' if nm_pa.nil?
  name, params = nm_pa
  fn = Func.new(name, ret_ty)
  if params
    params.each_named do |p|
      next unless p.type.to_s == 'parameter_declaration'
      pty = parse_type(p.child_by_field_name('type'), src)
      pname_node = p.child_by_field_name('declarator')
      next if pname_node.nil?
      while pname_node && pname_node.type.to_s.to_s != 'identifier'
        pname_node = pname_node.child_by_field_name('declarator')
      end
      next if pname_node.nil?
      fn.add_param(text(pname_node, src), pty)
    end
  end
  body = fdef.child_by_field_name('body')
  body_sx = compile_stmt(body, fn, src)
  body_sx = [:seq, body_sx, [:return_void]]
  [name, fn, body_sx, ret_ty]
end

def main
  if ARGV.empty?
    warn 'usage: parse.rb <file.c>'
    exit 1
  end
  src = File.binread(ARGV[0])
  tree = PARSER.parse_string(nil, src)
  root = tree.root_node
  warn "warning: parse errors in #{ARGV[0]}" if root.has_error?
  gather_signatures(root, src)

  out = +''
  out << "(program\n"
  root.each_named do |fdef|
    next unless fdef.type.to_s == 'function_definition'
    begin
      name, fn, body_sx, ret_ty = compile_function(fdef, src)
    rescue CompileError => e
      warn "error in #{ARGV[0]}: #{e.message}"
      exit 2
    end
    out << "  (func #{name} #{fn.params.length} #{fn.next_local} #{ret_ty}\n    "
    emit_sx(body_sx, out)
    out << ")\n"
  end
  out << ")\n"
  $stdout.write(out)
end

main if $0 == __FILE__
