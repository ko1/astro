#!/usr/bin/env ruby
# parse.rb: tree-sitter-c -> typed S-expression IR for castro.
#
# Output IR (S-expression):
#   (program GLOBALS_SLOT_COUNT
#     INIT_EXPR             ; runs once, before main
#     (func NAME PARAMS_CNT LOCALS_CNT RET_TY HAS_RETURNS EXPR)
#     ...)
#
# Types are tracked as CType objects internally; the SX output uses
# single-letter slot kinds (i / d / p / v) only where the runtime
# needs to know which VALUE union slot to use.

require 'tree_sitter'
require 'open3'

LANG = TreeSitter.lang('c')
PARSER = TreeSitter::Parser.new
PARSER.language = LANG

class CompileError < StandardError; end

# =====================================================================
# CType: a small type system for C primitive / pointer / array / struct
# =====================================================================
class CType
  attr_reader :kind, :name, :inner, :size, :fields

  PRIM_INT = %w[
    char signed_char unsigned_char _Bool bool
    short unsigned_short
    int unsigned_int unsigned
    long unsigned_long
    long_long unsigned_long_long
    size_t ssize_t ptrdiff_t intptr_t uintptr_t
  ].freeze
  PRIM_FLT = %w[float double long_double].freeze

  def initialize(kind, name: nil, inner: nil, size: nil, fields: nil)
    @kind = kind; @name = name; @inner = inner; @size = size; @fields = fields
  end

  def self.prim(n);     new(:prim, name: n); end
  def self.ptr(t);      new(:ptr,  inner: t); end
  def self.array(t, n); new(:array, inner: t, size: n); end
  def self.struct(name, fields=nil); new(:struct, name: name, fields: fields); end
  def self.void;        new(:prim, name: 'void'); end

  INT  = prim('int')
  CHAR = prim('char')
  LONG = prim('long')
  DBL  = prim('double')
  VOID = void

  def int?;        @kind == :prim && PRIM_INT.include?(@name); end
  def float?;      @kind == :prim && PRIM_FLT.include?(@name); end
  def void?;       @kind == :prim && @name == 'void'; end
  def ptr?;        @kind == :ptr; end
  def array?;      @kind == :array; end
  def struct?;     @kind == :struct; end
  def aggregate?;  array? || struct?; end
  def ptr_like?;   ptr? || array?; end
  def numeric?;    int? || float?; end

  # VALUE union slot to use ('i' / 'd' / 'p').
  def slot_kind
    return 'i' if int?
    return 'd' if float?
    return 'p' if ptr_like? || struct?
    'i'
  end

  # Slots occupied by a value of this type (1 for prim/ptr; product for
  # arrays; sum-of-fields for structs).  For struct lookups we consult
  # the global STRUCTS table — since `struct point` typed at declaration
  # time may carry no `fields` of its own (the body lives in the named
  # entry of STRUCTS).
  def slot_count
    case @kind
    when :prim, :ptr then 1
    when :array      then (@size || 1) * @inner.slot_count
    when :struct
      info = STRUCTS[@name]
      return info[:size_slots] if info
      @fields ? @fields.sum { |_, t| t.slot_count } : 1
    end
  end

  # Byte size as C reports it via sizeof().  Castro internally uses
  # 8-byte slots for everything, but this matches the host ABI so
  # programs that compute `sizeof(x)` see expected values.
  def byte_size
    case @kind
    when :prim
      case @name
      when 'char', 'signed_char', 'unsigned_char', '_Bool', 'bool' then 1
      when 'void' then 1
      when 'short', 'unsigned_short' then 2
      when 'int', 'unsigned', 'unsigned_int' then 4
      when 'float' then 4
      when 'long', 'unsigned_long' then 8
      when 'long_long', 'unsigned_long_long' then 8
      when 'double' then 8
      when 'long_double' then 16
      when 'size_t', 'ssize_t', 'ptrdiff_t', 'intptr_t', 'uintptr_t' then 8
      else 4
      end
    when :ptr then 8
    when :array then (@size || 0) * @inner.byte_size
    when :struct
      @fields ? @fields.sum { |_, t| t.byte_size } : 8
    end
  end

  # Used when the type appears in a value context: arrays decay to
  # pointers (the value of `a` in `int a[N]` is `&a[0]`).
  def decay
    array? ? CType.ptr(@inner) : self
  end

  def to_s
    case @kind
    when :prim   then @name
    when :ptr    then "#{@inner}*"
    when :array  then "#{@inner}[#{@size}]"
    when :struct then "struct #{@name}"
    end
  end

  def ==(other) other.is_a?(CType) && to_s == other.to_s; end
  alias eql? ==
  def hash; to_s.hash; end
end

# =====================================================================
# Globals: a table of symbol → [slot index, type, init AST] kept across
# the whole compilation unit.
# =====================================================================
GLOBALS = {}        # name -> [slot_idx, CType]
GLOBAL_INITS = []   # array of init AST forms (executed in order)
GLOBAL_NEXT = 0     # next free slot in c->globals

def alloc_global(name, ty)
  idx = GLOBAL_NEXT
  GLOBALS[name] = [idx, ty]
  add_globals = ty.slot_count
  $G_next_after = idx + add_globals
  define_singleton_method(:_noop) { } if false # keep editor happy
  idx_after = idx + add_globals
  set_global_next(idx_after)
  idx
end

def set_global_next(n) ; Object.send(:remove_const, :GLOBAL_NEXT); Object.const_set(:GLOBAL_NEXT, n); end

# =====================================================================
# Function table (signatures only, gathered up-front)
# =====================================================================
GLOBAL_FUNCS = {}   # name -> [idx_or_nil, ret_ty (CType), [param_tys (CType)]]
                    # idx is the function's position in the order of
                    # function_definitions encountered; nil for extern
                    # declarations that have no body in this TU.

# Filled in by compile_function: FUNC_NEEDS_SETJMP[idx] = true if the
# function's body still has un-lifted `return` (so callers must use
# the setjmp-variant call node).
FUNC_NEEDS_SETJMP = []
STRUCTS = {}        # tag -> { fields: [[name, CType]], offsets: { name => slot_idx }, size_slots: N }
TYPEDEFS = {}       # name -> CType

# =====================================================================
# helpers
# =====================================================================

def text(node, src)
  src.byteslice(node.start_byte, node.end_byte - node.start_byte).force_encoding('UTF-8')
end

# Parse a tree-sitter type-specifier subtree into a CType.
def parse_type_spec(node, src)
  return CType::INT if node.nil?
  k = node.type.to_s
  case k
  when 'primitive_type', 'sized_type_specifier'
    name = text(node, src).strip
    name = name.gsub(/\bconst\b|\bvolatile\b|\brestrict\b|\bregister\b|\bstatic\b|\bextern\b|\b_Atomic\b|\binline\b/, '').strip
    name = name.gsub(/\s+/, '_')
    return CType.void if name == 'void'
    return CType.prim('double') if %w[double long_double].include?(name)
    return CType.prim('float') if name == 'float'
    case name
    when 'char', 'signed_char', 'unsigned_char' then CType.prim('char')
    when 'short', 'short_int', 'signed_short', 'signed_short_int', 'unsigned_short', 'unsigned_short_int'
      CType.prim('short')
    when 'long', 'long_int', 'signed_long', 'signed_long_int', 'unsigned_long', 'unsigned_long_int'
      CType.prim('long')
    when 'long_long', 'long_long_int', 'signed_long_long', 'unsigned_long_long', 'unsigned_long_long_int'
      CType.prim('long_long')
    when 'int', 'signed', 'signed_int', 'unsigned', 'unsigned_int' then CType::INT
    when '_Bool', 'bool' then CType.prim('_Bool')
    else CType::INT
    end
  when 'struct_specifier'
    tag_node = node.child_by_field_name('name')
    body = node.child_by_field_name('body')
    tag = tag_node ? text(tag_node, src) : "__anon_#{node.start_byte}"
    if body
      fields = parse_struct_body(body, src)
      offsets = {}
      slot = 0
      fields.each do |fname, fty|
        offsets[fname] = slot
        slot += fty.slot_count
      end
      STRUCTS[tag] = { fields: fields, offsets: offsets, size_slots: slot }
    end
    CType.struct(tag)
  when 'type_identifier'
    name = text(node, src)
    TYPEDEFS[name] || CType::INT
  when 'enum_specifier'
    CType::INT
  else
    CType::INT
  end
end

def parse_struct_body(body, src)
  fields = []
  body.each_named do |decl|
    next unless decl.type.to_s == 'field_declaration'
    base_ty = parse_type_spec(decl.child_by_field_name('type'), src)
    decl.each_named do |child|
      next unless %w[field_identifier identifier pointer_declarator array_declarator function_declarator].include?(child.type.to_s)
      fname, fty = parse_declarator(child, base_ty, src)
      next if fname.nil?
      fields << [fname, fty]
    end
  end
  fields
end

# Walk the declarator subtree starting from `decl` (e.g.
# `int *a[10]`), peeling off pointer/array layers and returning
# [name, full_type].  base_ty is the type from the base specifier
# (e.g. `int`).
def parse_declarator(decl, base_ty, src)
  return [nil, base_ty] if decl.nil?
  k = decl.type.to_s
  case k
  when 'identifier', 'field_identifier', 'type_identifier'
    [text(decl, src), base_ty]
  when 'parenthesized_declarator'
    inner = nil
    decl.each_named { |c| inner = c; break }
    parse_declarator(inner, base_ty, src)
  when 'pointer_declarator'
    inner = decl.child_by_field_name('declarator')
    if inner
      parse_declarator(inner, CType.ptr(base_ty), src)
    else
      [nil, CType.ptr(base_ty)]
    end
  when 'abstract_pointer_declarator'
    # Used in casts and abstract type-descriptors: `(void *)`, `int (*)`.
    inner = decl.child_by_field_name('declarator')
    if inner
      parse_declarator(inner, CType.ptr(base_ty), src)
    else
      [nil, CType.ptr(base_ty)]
    end
  when 'array_declarator', 'abstract_array_declarator'
    inner = decl.child_by_field_name('declarator')
    sz_node = decl.child_by_field_name('size')
    sz = sz_node ? eval_const_int(sz_node, src) : nil
    if inner
      parse_declarator(inner, CType.array(base_ty, sz), src)
    else
      [nil, CType.array(base_ty, sz)]
    end
  when 'function_declarator', 'abstract_function_declarator'
    inner = decl.child_by_field_name('declarator')
    if inner
      parse_declarator(inner, base_ty, src)
    else
      [nil, base_ty]
    end
  when 'init_declarator'
    inner = decl.child_by_field_name('declarator')
    parse_declarator(inner, base_ty, src)
  else
    [nil, base_ty]
  end
end

# Evaluate a tree-sitter expression subtree as a constant int (best-effort).
# Used for array sizes and case labels.
def eval_const_int(node, src)
  return nil if node.nil?
  k = node.type.to_s
  case k
  when 'number_literal' then parse_int_literal(text(node, src))
  when 'char_literal'   then parse_char_literal(text(node, src))
  when 'parenthesized_expression'
    inner = nil; node.each_named { |c| inner = c; break }
    eval_const_int(inner, src)
  when 'unary_expression'
    op_node = node.child_by_field_name('operator')
    return nil if op_node.nil?
    op = text(op_node, src)
    arg = eval_const_int(node.child_by_field_name('argument'), src)
    return nil if arg.nil?
    case op
    when '-' then -arg
    when '+' then arg
    when '~' then ~arg
    when '!' then arg == 0 ? 1 : 0
    end
  when 'binary_expression'
    op_node = node.child_by_field_name('operator')
    return nil if op_node.nil?
    op = text(op_node, src)
    l = eval_const_int(node.child_by_field_name('left'), src)
    r = eval_const_int(node.child_by_field_name('right'), src)
    return nil if l.nil? || r.nil?
    case op
    when '+' then l + r
    when '-' then l - r
    when '*' then l * r
    when '/' then r == 0 ? 0 : l / r
    when '%' then r == 0 ? 0 : l % r
    when '<<' then l << r
    when '>>' then l >> r
    when '&' then l & r
    when '|' then l | r
    when '^' then l ^ r
    end
  when 'sizeof_expression'
    sizeof_of(node, src)
  when 'identifier'
    ENUMS[text(node, src)]
  end
end

ENUMS = {}

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
  "'" => 39, '"' => 34, 'a' => 7, 'b' => 8, 'f' => 12, 'v' => 11,
  '?' => 63, 'e' => 27
}

def parse_char_literal(t)
  inner = t.sub(/\A(?:L|u8|u|U)?'/, '').sub(/'\z/, '')
  if inner.start_with?('\\')
    rest = inner[1..]
    if (v = CHAR_ESC[rest[0]])
      v
    elsif rest[0] == 'x' && rest.length > 1
      rest[1..].to_i(16)
    elsif rest[0] && rest[0] >= '0' && rest[0] <= '7'
      rest.to_i(8)
    else
      rest[0].ord
    end
  else
    inner.ord
  end
end

# tree-sitter-c yields a `string_literal` node containing the surrounding
# quotes plus inner string_content / escape_sequence children.
def parse_string_literal(node, src)
  out = +''
  case node.type.to_s
  when 'concatenated_string'
    node.each_named { |c| out << parse_string_literal(c, src) }
    return out
  end
  node.each do |c|
    case c.type.to_s
    when 'string_content'
      out << text(c, src)
    when 'escape_sequence'
      esc = text(c, src)
      ch = esc[1]
      if (v = CHAR_ESC[ch])
        out << v.chr
      elsif ch == 'x' && esc.length >= 3
        out << esc[2..].to_i(16).chr
      elsif ch && ch >= '0' && ch <= '7'
        out << esc[1..].to_i(8).chr
      else
        out << ch.to_s
      end
    end
  end
  out
end

# Drill through pointer_declarator/parenthesized_declarator/function_declarator
# to the function_declarator level, returning [name, parameter_list_node, ret_ty].
# `base_ret` is the type from the function's return-type specifier; pointer/array
# layers around the function name modify it (e.g. `int *foo()` returns int*).
def unwrap_decl(decl, src, base_ret)
  ty = base_ret
  cur = decl
  loop do
    return nil if cur.nil?
    case cur.type.to_s
    when 'pointer_declarator'
      ty = CType.ptr(ty)
      cur = cur.child_by_field_name('declarator')
    when 'parenthesized_declarator'
      inner = nil; cur.each_named { |c| inner = c; break }
      cur = inner
    when 'function_declarator'
      inner = cur.child_by_field_name('declarator')
      params = cur.child_by_field_name('parameters')
      while inner && inner.type.to_s == 'pointer_declarator'
        ty = CType.ptr(ty)
        inner = inner.child_by_field_name('declarator')
      end
      while inner && inner.type.to_s == 'parenthesized_declarator'
        nx = nil; inner.each_named { |c| nx = c; break }
        inner = nx
      end
      return nil if inner.nil? || inner.type.to_s != 'identifier'
      return [text(inner, src), params, ty]
    when 'array_declarator'
      sz_node = cur.child_by_field_name('size')
      sz = sz_node ? eval_const_int(sz_node, src) : nil
      ty = CType.array(ty, sz)
      cur = cur.child_by_field_name('declarator')
    else
      return nil
    end
  end
end

# =====================================================================
# Func: per-function symbol table
# =====================================================================
class Func
  attr_reader :name, :ret_ty, :params, :locals_map
  attr_accessor :next_local, :uses_goto
  def initialize(name, ret_ty)
    @name = name
    @ret_ty = ret_ty
    @params = []
    @locals_map = {}
    @next_local = 0
    @uses_goto = false
    @label_map = {}        # label name -> integer index
  end

  def add_param(name, ty)
    idx = @next_local
    @locals_map[name] = [idx, ty]
    @params << [name, ty]
    @next_local += ty.slot_count
    idx
  end

  def add_local(name, ty)
    idx = @next_local
    @locals_map[name] = [idx, ty]
    @next_local += ty.slot_count
    idx
  end

  def label_index(name)
    @label_map[name] ||= @label_map.size + 1   # 0 reserved for entry
  end

  def label_map; @label_map; end
end

# =====================================================================
# Type-aware helpers
# =====================================================================

def cast(expr, frm, to)
  return expr if frm == to
  return [:cast_id, expr] if frm.int? && to.float?
  return [:cast_di, expr] if frm.float? && to.int?
  return [:drop, expr]    if to.void?
  raise CompileError, "can't cast from void" if frm.void?
  # int <-> ptr conversions: the runtime keeps both in 8-byte slots, so
  # passing the bits through is fine for our purposes.
  expr
end

def to_bool(expr, ty)
  return expr if ty.int? || ty.ptr_like?
  return [:neq_d, expr, [:lit_d, 0.0]] if ty.float?
  raise CompileError, "can't booleanize void"
end

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
    out << '"'
    node.each_byte do |b|
      case b
      when 92  then out << '\\\\'
      when 34  then out << '\\"'
      when 10  then out << '\\n'
      when 9   then out << '\\t'
      when 13  then out << '\\r'
      when 0   then out << '\\0'
      else
        if b < 32 || b == 127
          out << format('\\x%02x', b)
        else
          out << b.chr
        end
      end
    end
    out << '"'
  when true  then out << '1'
  when false then out << '0'
  when Integer then out << node.to_s
  when Float
    out << format('%.17g', node)
  else
    raise CompileError, "emit: unsupported #{node.inspect}"
  end
end

# =====================================================================
# Loop helpers (break/continue routing)
# =====================================================================
ALWAYS_RETURN_OPS = %i[return return_void].freeze

def loop_escape_kind(expr)
  return :neither unless expr.is_a?(Array)
  has_brk = false
  has_cnt = false
  walk = lambda do |e|
    return unless e.is_a?(Array)
    case e[0]
    when :break    then has_brk = true
    when :continue then has_cnt = true
    when :while, :do_while, :for,
         :while_brk, :do_while_brk, :for_brk,
         :while_brk_only, :do_while_brk_only, :for_brk_only
      return
    else
      e.each { |c| walk.call(c) }
    end
  end
  walk.call(expr)
  if has_brk && has_cnt then :both
  elsif has_brk then :has_break
  elsif has_cnt then :has_continue
  else :neither
  end
end

def pick_loop(base, normal_form, body)
  kind = loop_escape_kind(body)
  return normal_form if kind == :neither
  brk = (base.to_s + '_brk').to_sym
  brk_only = (base.to_s + '_brk_only').to_sym
  tag = (kind == :has_break ? brk_only : brk)
  [tag, *normal_form[1..]]
end

# =====================================================================
# Tail-return lifting
# =====================================================================
def always_returns?(expr)
  return false unless expr.is_a?(Array)
  case expr[0]
  when :return, :return_void then true
  when :seq                  then always_returns?(expr[2])
  when :if, :ternary         then always_returns?(expr[2]) && always_returns?(expr[3])
  else false
  end
end

def has_return?(expr)
  return false unless expr.is_a?(Array)
  return true if ALWAYS_RETURN_OPS.include?(expr[0])
  expr.any? { |e| has_return?(e) }
end

def lift_tail(expr)
  return expr unless expr.is_a?(Array)
  case expr[0]
  when :return then expr[1]
  when :return_void then [:lit_i, 0]
  when :seq
    a, b = expr[1], expr[2]
    if always_returns?(a)
      lift_tail(a)
    elsif a.is_a?(Array) && a[0] == :if
      _, c, t, e = a
      if always_returns?(t) && always_returns?(e)
        lift_tail(a)
      elsif always_returns?(t)
        lift_tail([:if, c, t, [:seq, e, b]])
      elsif always_returns?(e)
        lift_tail([:if, c, [:seq, t, b], e])
      else
        [:seq, a, lift_tail(b)]
      end
    else
      [:seq, a, lift_tail(b)]
    end
  when :if
    [:if, expr[1], lift_tail(expr[2]), lift_tail(expr[3])]
  when :ternary
    [:ternary, expr[1], lift_tail(expr[2]), lift_tail(expr[3])]
  else
    expr
  end
end

# =====================================================================
# sizeof(type_or_expr)
# =====================================================================
def sizeof_of(sizeof_node, src, fn=nil)
  type_node = nil
  expr_node = nil
  sizeof_node.each_named do |c|
    if c.type.to_s == 'type_descriptor'
      type_node = c
    else
      expr_node = c
    end
  end
  if type_node
    base = parse_type_spec(type_node.child_by_field_name('type') || type_node, src)
    decl_node = type_node.child_by_field_name('declarator')
    if decl_node
      _, ty = parse_declarator(decl_node, base, src)
      return ty.byte_size
    end
    return base.byte_size
  end
  if expr_node && fn
    # For a bare identifier in sizeof, use the un-decayed declared type
    # (so `sizeof(arr)` for `int arr[10]` returns 40, not 8).
    if expr_node.type.to_s == 'parenthesized_expression'
      inner = nil; expr_node.each_named { |c| inner = c; break }
      expr_node = inner if inner
    end
    if expr_node && expr_node.type.to_s == 'identifier'
      nm = text(expr_node, src)
      if fn.locals_map.key?(nm)
        return fn.locals_map[nm][1].byte_size
      elsif GLOBALS.key?(nm)
        return GLOBALS[nm][1].byte_size
      end
    end
    begin
      _, ty = compile_expr(expr_node, fn, src)
      return ty.byte_size
    rescue CompileError
    end
  end
  4
end

# =====================================================================
# expression compiler — returns [sx_form, CType]
# =====================================================================

# Shared helper: produce a load expression for a variable slot of type ty.
def lvar_load(idx, ty, scope)
  # `scope` is :local or :global.  Arrays decay to "address-of" — the
  # value of `a` is `&a[0]` (a slot pointer).
  if ty.array?
    return [scope == :global ? :addr_global : :addr_local, idx]
  end
  case ty.slot_kind
  when 'i' then [scope == :global ? :gget : :lget, idx]
  when 'd' then [scope == :global ? :gget : :lget, idx]
  when 'p' then [scope == :global ? :gget : :lget, idx]
  end
end

def lvar_store(idx, ty, rhs, scope)
  raise CompileError, "can't assign to array variable" if ty.array?
  [scope == :global ? :gset : :lset, idx, rhs]
end

def builtin_call(fname, args_compiled, fn)
  # args_compiled: [[sx, ty], ...] (already compiled)
  case fname
  when 'malloc'
    raise CompileError, 'malloc takes 1 arg' if args_compiled.length != 1
    a, _ = args_compiled[0]
    [[:call_malloc, a], CType.ptr(CType.void)]
  when 'calloc'
    raise CompileError, 'calloc takes 2 args' if args_compiled.length != 2
    a, _ = args_compiled[0]
    b, _ = args_compiled[1]
    [[:call_calloc, a, b], CType.ptr(CType.void)]
  when 'free'
    a, _ = args_compiled[0]
    [[:call_free, a], CType::VOID]
  when 'strlen'
    a, _ = args_compiled[0]
    [[:call_strlen, a], CType.prim('size_t')]
  when 'strcmp'
    a, _ = args_compiled[0]; b, _ = args_compiled[1]
    [[:call_strcmp, a, b], CType::INT]
  when 'strncmp'
    a, _ = args_compiled[0]; b, _ = args_compiled[1]; c, _ = args_compiled[2]
    [[:call_strncmp, a, b, c], CType::INT]
  when 'strcpy'
    a, _ = args_compiled[0]; b, _ = args_compiled[1]
    [[:call_strcpy, a, b], CType.ptr(CType::CHAR)]
  when 'strncpy'
    a, _ = args_compiled[0]; b, _ = args_compiled[1]; c, _ = args_compiled[2]
    [[:call_strncpy, a, b, c], CType.ptr(CType::CHAR)]
  when 'strcat'
    a, _ = args_compiled[0]; b, _ = args_compiled[1]
    [[:call_strcat, a, b], CType.ptr(CType::CHAR)]
  when 'memset'
    a, _ = args_compiled[0]; b, _ = args_compiled[1]; c, _ = args_compiled[2]
    [[:call_memset, a, b, c], CType.ptr(CType.void)]
  when 'memcpy', 'memmove'
    a, _ = args_compiled[0]; b, _ = args_compiled[1]; c, _ = args_compiled[2]
    [[:call_memcpy, a, b, c], CType.ptr(CType.void)]
  when 'atoi'
    a, _ = args_compiled[0]
    [[:call_atoi, a], CType::INT]
  when 'exit', 'abort', '_Exit'
    a = args_compiled[0] ? args_compiled[0][0] : [:lit_i, 0]
    [[:call_exit, a], CType::VOID]
  when 'abs', 'labs', 'llabs'
    a, _ = args_compiled[0]
    [[:call_abs, a], CType::INT]
  else
    nil
  end
end

# Compile an expression to [sx, CType].
def compile_expr(node, fn, src)
  k = node.type.to_s

  case k
  when 'number_literal'
    t = text(node, src)
    is_hex = t.downcase.start_with?('0x')
    # Hex literals can contain `f`/`F` as digits; only treat trailing
    # `f` / `F` as a float suffix when it's *not* a hex literal.  And
    # `e` is only an exponent when it's surrounded by digits (in hex
    # `0xE` is just the digit 14).
    is_float = !is_hex && (t.include?('.') ||
                           t.downcase.include?('e') ||
                           t.match?(/[fF]$/))
    if is_float
      [[:lit_d, t.gsub(/[fFlL]$/, '').to_f], CType::DBL]
    else
      v = parse_int_literal(t)
      ty = (t.match?(/[lL]+/) ? CType::LONG : CType::INT)
      if (-2**31...2**31).cover?(v)
        [[:lit_i, v], ty]
      else
        [[:lit_i64, v & ((1 << 64) - 1)], ty]
      end
    end

  when 'char_literal'
    [[:lit_i, parse_char_literal(text(node, src))], CType::CHAR]

  when 'string_literal', 'concatenated_string'
    [[:lit_str_array, parse_string_literal(node, src)], CType.ptr(CType::CHAR)]

  when 'true'  then [[:lit_i, 1], CType::INT]
  when 'false' then [[:lit_i, 0], CType::INT]
  when 'null'  then [[:lit_i, 0], CType.ptr(CType.void)]

  when 'identifier'
    name = text(node, src)
    if fn.locals_map.key?(name)
      idx, ty = fn.locals_map[name]
      [lvar_load(idx, ty, :local), ty.decay]
    elsif GLOBALS.key?(name)
      idx, ty = GLOBALS[name]
      [lvar_load(idx, ty, :global), ty.decay]
    elsif ENUMS.key?(name)
      [[:lit_i, ENUMS[name]], CType::INT]
    elsif GLOBAL_FUNCS.key?(name) && GLOBAL_FUNCS[name][0]
      # Function-name in expression context decays to a function pointer.
      [[:func_addr, GLOBAL_FUNCS[name][0]], CType.ptr(CType::INT)]
    else
      raise CompileError, "undefined identifier: #{name}"
    end

  when 'parenthesized_expression'
    inner = nil; node.each_named { |c| inner = c; break }
    raise CompileError, 'empty parenthesized' if inner.nil?
    compile_expr(inner, fn, src)

  when 'binary_expression'
    compile_binary(node, fn, src)

  when 'unary_expression'
    compile_unary(node, fn, src)

  when 'pointer_expression'
    # Either `*p` (dereference) or `&x` (address-of).  tree-sitter-c uses
    # this node for both forms, distinguished by the `operator` field.
    op = text(node.child_by_field_name('operator'), src)
    arg = node.child_by_field_name('argument')
    if op == '*'
      compile_dereference(arg, fn, src)
    elsif op == '&'
      compile_address_of(arg, fn, src)
    else
      raise CompileError, "pointer_expression: unknown op #{op}"
    end

  when 'subscript_expression'
    arr_node = node.child_by_field_name('argument')
    idx_node = node.child_by_field_name('index')
    arr_sx, arr_ty = compile_expr(arr_node, fn, src)
    idx_sx, idx_ty = compile_expr(idx_node, fn, src)
    raise CompileError, "subscript needs pointer" unless arr_ty.ptr_like?
    elem_ty = arr_ty.array? ? arr_ty.inner : arr_ty.inner
    addr = [:ptr_add, arr_sx, cast(idx_sx, idx_ty, CType::LONG)]
    [load_through(addr, elem_ty), elem_ty.decay]

  when 'field_expression'
    compile_field(node, fn, src)

  when 'update_expression'
    compile_update(node, fn, src)

  when 'assignment_expression'
    compile_assignment(node, fn, src)

  when 'call_expression'
    compile_call(node, fn, src)

  when 'conditional_expression'
    children = []
    node.each_named { |c| children << c }
    cs_, ct_ = compile_expr(children[0], fn, src)
    ts_, tt_ = compile_expr(children[1], fn, src)
    es_, et_ = compile_expr(children[2], fn, src)
    promoted =
      if tt_.float? || et_.float? then CType::DBL
      elsif tt_.ptr_like? then tt_
      elsif et_.ptr_like? then et_
      else CType::INT
      end
    [[:ternary, to_bool(cs_, ct_), cast(ts_, tt_, promoted), cast(es_, et_, promoted)], promoted]

  when 'cast_expression'
    ty_node = node.child_by_field_name('type')
    base = parse_type_spec(ty_node.child_by_field_name('type') || ty_node, src)
    decl = ty_node.child_by_field_name('declarator')
    target = decl ? parse_declarator(decl, base, src)[1] : base
    es, et = compile_expr(node.child_by_field_name('value'), fn, src)
    [cast(es, et, target), target]

  when 'comma_expression'
    ls, _ = compile_expr(node.child_by_field_name('left'), fn, src)
    rs, rt = compile_expr(node.child_by_field_name('right'), fn, src)
    [[:seq, ls, rs], rt]

  when 'sizeof_expression'
    [[:lit_i, sizeof_of(node, src, fn)], CType.prim('size_t')]

  when 'compound_literal_expression'
    # (T){...} — just return zero of the type for now.
    ty_node = node.child_by_field_name('type')
    base = parse_type_spec(ty_node.child_by_field_name('type') || ty_node, src)
    decl = ty_node.child_by_field_name('declarator')
    target = decl ? parse_declarator(decl, base, src)[1] : base
    if target.float?
      [[:lit_d, 0.0], target]
    else
      [[:lit_i, 0], target]
    end

  else
    raise CompileError, "unhandled expr: #{k}"
  end
end

def compile_binary(node, fn, src)
  op = text(node.child_by_field_name('operator'), src)
  ls, lt = compile_expr(node.child_by_field_name('left'), fn, src)
  rs, rt = compile_expr(node.child_by_field_name('right'), fn, src)
  if %w[&& ||].include?(op)
    tag = op == '&&' ? :land : :lor
    return [[tag, to_bool(ls, lt), to_bool(rs, rt)], CType::INT]
  end
  if %w[& | ^ << >> %].include?(op)
    ls = cast(ls, lt, CType::INT); rs = cast(rs, rt, CType::INT)
    omap = { '&' => :band, '|' => :bor, '^' => :bxor, '<<' => :shl, '>>' => :shr, '%' => :mod_i }
    return [[omap[op], ls, rs], CType::INT]
  end
  # Pointer arithmetic: p + i, i + p, p - i, p - p
  if op == '+' && (lt.ptr_like? || rt.ptr_like?)
    pe, pt = lt.ptr_like? ? [ls, lt] : [rs, rt]
    ie, it = lt.ptr_like? ? [rs, rt] : [ls, lt]
    raise CompileError, 'ptr + non-int' unless it.int?
    return [[:ptr_add, pe, cast(ie, it, CType::LONG)], pt]
  end
  if op == '-' && lt.ptr_like? && rt.int?
    return [[:ptr_sub_i, ls, cast(rs, rt, CType::LONG)], lt]
  end
  if op == '-' && lt.ptr_like? && rt.ptr_like?
    return [[:ptr_diff, ls, rs], CType.prim('ptrdiff_t')]
  end
  if %w[+ - * /].include?(op)
    promoted = (lt.float? || rt.float?) ? CType::DBL : CType::INT
    ls = cast(ls, lt, promoted); rs = cast(rs, rt, promoted)
    suf = '_' + promoted.slot_kind
    omap = { '+' => 'add', '-' => 'sub', '*' => 'mul', '/' => 'div' }
    return [[(omap[op] + suf).to_sym, ls, rs], promoted]
  end
  if %w[< <= > >= == !=].include?(op)
    if lt.ptr_like? || rt.ptr_like?
      promoted = lt.ptr_like? ? lt : rt
    else
      promoted = (lt.float? || rt.float?) ? CType::DBL : CType::INT
    end
    suf = promoted.float? ? '_d' : '_i'
    ls = cast(ls, lt, promoted); rs = cast(rs, rt, promoted)
    omap = { '<' => 'lt', '<=' => 'le', '>' => 'gt', '>=' => 'ge', '==' => 'eq', '!=' => 'neq' }
    return [[(omap[op] + suf).to_sym, ls, rs], CType::INT]
  end
  raise CompileError, "binop #{op} not supported"
end

def compile_unary(node, fn, src)
  op = text(node.child_by_field_name('operator'), src)
  es, et = compile_expr(node.child_by_field_name('argument'), fn, src)
  case op
  when '-'
    return [[:neg_d, es], CType::DBL] if et.float?
    [[:neg_i, cast(es, et, CType::INT)], CType::INT]
  when '+' then [es, et]
  when '!' then [[:lnot, to_bool(es, et)], CType::INT]
  when '~' then [[:bnot, cast(es, et, CType::INT)], CType::INT]
  when '*' then compile_dereference(node.child_by_field_name('argument'), fn, src)
  when '&' then compile_address_of(node.child_by_field_name('argument'), fn, src)
  else raise CompileError, "unary #{op}"
  end
end

# Produce a load expression through pointer `addr_sx` based on element type.
def load_through(addr_sx, elem_ty)
  case elem_ty.kind
  when :prim
    elem_ty.float? ? [:load_d, addr_sx] : [:load_i, addr_sx]
  when :ptr
    [:load_p, addr_sx]
  when :array
    # array doesn't load — it decays to the address itself
    addr_sx
  when :struct
    # opaque value; load slot 0 (placeholder)
    [:load_i, addr_sx]
  end
end

def store_through(addr_sx, val_sx, ty)
  case ty.kind
  when :prim
    ty.float? ? [:store_d, addr_sx, val_sx] : [:store_i, addr_sx, val_sx]
  when :ptr
    [:store_p, addr_sx, val_sx]
  else
    [:store_i, addr_sx, val_sx]
  end
end

def compile_dereference(arg, fn, src)
  ps, pt = compile_expr(arg, fn, src)
  raise CompileError, "deref of non-pointer (#{pt})" unless pt.ptr_like?
  elem = pt.inner
  [load_through(ps, elem), elem.decay]
end

def compile_address_of(arg, fn, src)
  k = arg.type.to_s
  case k
  when 'identifier'
    name = text(arg, src)
    if fn.locals_map.key?(name)
      idx, ty = fn.locals_map[name]
      return [[:addr_local, idx], CType.ptr(ty)]
    elsif GLOBALS.key?(name)
      idx, ty = GLOBALS[name]
      return [[:addr_global, idx], CType.ptr(ty)]
    elsif GLOBAL_FUNCS.key?(name) && GLOBAL_FUNCS[name][0]
      return [[:func_addr, GLOBAL_FUNCS[name][0]], CType.ptr(CType::INT)]
    else
      raise CompileError, "address of undefined: #{name}"
    end
  when 'parenthesized_expression'
    inner = nil; arg.each_named { |c| inner = c; break }
    return compile_address_of(inner, fn, src)
  when 'subscript_expression'
    # &a[i] = a + i
    arr_node = arg.child_by_field_name('argument')
    idx_node = arg.child_by_field_name('index')
    arr_sx, arr_ty = compile_expr(arr_node, fn, src)
    idx_sx, idx_ty = compile_expr(idx_node, fn, src)
    raise CompileError, "addr-of subscript needs pointer" unless arr_ty.ptr_like?
    elem = arr_ty.array? ? arr_ty.inner : arr_ty.inner
    return [[:ptr_add, arr_sx, cast(idx_sx, idx_ty, CType::LONG)], CType.ptr(elem)]
  when 'field_expression'
    addr_sx, ftype = field_address(arg, fn, src)
    return [addr_sx, CType.ptr(ftype)]
  when 'unary_expression'
    op = text(arg.child_by_field_name('operator'), src)
    if op == '*'
      # &*x  ==  x
      return compile_expr(arg.child_by_field_name('argument'), fn, src)
    end
  end
  raise CompileError, "can't take address of #{k}"
end

def compile_assignment(node, fn, src)
  op = text(node.child_by_field_name('operator'), src)
  l_node = node.child_by_field_name('left')
  r_node = node.child_by_field_name('right')

  # Strip parens
  while l_node.type.to_s == 'parenthesized_expression'
    inner = nil; l_node.each_named { |c| inner = c; break }
    l_node = inner
  end

  rs, rt = compile_expr(r_node, fn, src)

  # Identifier (local or global)
  if l_node.type.to_s == 'identifier'
    name = text(l_node, src)
    if fn.locals_map.key?(name)
      idx, lt = fn.locals_map[name]
      return assign_to_var(idx, lt, op, rs, rt, :local)
    elsif GLOBALS.key?(name)
      idx, lt = GLOBALS[name]
      return assign_to_var(idx, lt, op, rs, rt, :global)
    end
    raise CompileError, "undefined: #{name}"
  end

  # *p = v
  if l_node.type.to_s == 'pointer_expression' &&
     text(l_node.child_by_field_name('operator'), src) == '*'
    arg = l_node.child_by_field_name('argument')
    ps, pt = compile_expr(arg, fn, src)
    raise CompileError, "deref-assign of non-pointer" unless pt.ptr_like?
    elem = pt.inner
    return assign_through(ps, elem, op, rs, rt)
  end

  # *p style via unary_expression
  if l_node.type.to_s == 'unary_expression' &&
     text(l_node.child_by_field_name('operator'), src) == '*'
    arg = l_node.child_by_field_name('argument')
    ps, pt = compile_expr(arg, fn, src)
    raise CompileError, "deref-assign of non-pointer" unless pt.ptr_like?
    elem = pt.inner
    return assign_through(ps, elem, op, rs, rt)
  end

  # a[i] = v
  if l_node.type.to_s == 'subscript_expression'
    arr_sx, arr_ty = compile_expr(l_node.child_by_field_name('argument'), fn, src)
    idx_sx, idx_ty = compile_expr(l_node.child_by_field_name('index'), fn, src)
    raise CompileError, "subscript-assign needs pointer" unless arr_ty.ptr_like?
    elem = arr_ty.array? ? arr_ty.inner : arr_ty.inner
    addr = [:ptr_add, arr_sx, cast(idx_sx, idx_ty, CType::LONG)]
    return assign_through(addr, elem, op, rs, rt)
  end

  # s.f = v   /   s->f = v
  if l_node.type.to_s == 'field_expression'
    addr_sx, ftype = field_address(l_node, fn, src)
    return assign_through(addr_sx, ftype, op, rs, rt)
  end

  raise CompileError, "lhs must be a simple identifier (Phase 1)"
end

def assign_to_var(idx, lt, op, rs, rt, scope)
  if op == '='
    return [lvar_store(idx, lt, cast(rs, rt, lt), scope), lt]
  end
  comp = { '+=' => '+', '-=' => '-', '*=' => '*', '/=' => '/', '%=' => '%',
           '&=' => '&', '|=' => '|', '^=' => '^', '<<=' => '<<', '>>=' => '>>' }
  base = comp[op] or raise CompileError, "unsupported assign op #{op}"
  current = lvar_load(idx, lt, scope)
  if %w[+ -].include?(base) && lt.ptr_like? && rt.int?
    new_val = base == '+' ? [:ptr_add, current, cast(rs, rt, CType::LONG)]
                          : [:ptr_sub_i, current, cast(rs, rt, CType::LONG)]
    return [lvar_store(idx, lt, new_val, scope), lt]
  end
  if %w[+ - * /].include?(base)
    promoted = (lt.float? || rt.float?) ? CType::DBL : CType::INT
    ls2 = cast(current, lt, promoted)
    rs2 = cast(rs, rt, promoted)
    omap = { '+' => 'add', '-' => 'sub', '*' => 'mul', '/' => 'div' }
    e = [(omap[base] + '_' + promoted.slot_kind).to_sym, ls2, rs2]
    return [lvar_store(idx, lt, cast(e, promoted, lt), scope), lt]
  end
  ls2 = cast(current, lt, CType::INT)
  rs2 = cast(rs, rt, CType::INT)
  omap = { '%' => :mod_i, '&' => :band, '|' => :bor, '^' => :bxor, '<<' => :shl, '>>' => :shr }
  e = [omap[base], ls2, rs2]
  [lvar_store(idx, lt, cast(e, CType::INT, lt), scope), lt]
end

def assign_through(addr_sx, elem, op, rs, rt)
  if op == '='
    val = cast(rs, rt, elem)
    return [store_through(addr_sx, val, elem), elem]
  end
  # compound assignment: load via tmp, compute, store
  comp = { '+=' => '+', '-=' => '-', '*=' => '*', '/=' => '/', '%=' => '%',
           '&=' => '&', '|=' => '|', '^=' => '^', '<<=' => '<<', '>>=' => '>>' }
  base = comp[op] or raise CompileError, "unsupported deref-assign op #{op}"
  loaded = load_through(addr_sx, elem)
  if %w[+ -].include?(base) && elem.ptr_like? && rt.int?
    new_val = base == '+' ? [:ptr_add, loaded, cast(rs, rt, CType::LONG)]
                          : [:ptr_sub_i, loaded, cast(rs, rt, CType::LONG)]
    return [store_through(addr_sx, new_val, elem), elem]
  end
  if %w[+ - * /].include?(base)
    promoted = (elem.float? || rt.float?) ? CType::DBL : CType::INT
    ls2 = cast(loaded, elem, promoted)
    rs2 = cast(rs, rt, promoted)
    omap = { '+' => 'add', '-' => 'sub', '*' => 'mul', '/' => 'div' }
    e = [(omap[base] + '_' + promoted.slot_kind).to_sym, ls2, rs2]
    return [store_through(addr_sx, cast(e, promoted, elem), elem), elem]
  end
  ls2 = cast(loaded, elem, CType::INT)
  rs2 = cast(rs, rt, CType::INT)
  omap = { '%' => :mod_i, '&' => :band, '|' => :bor, '^' => :bxor, '<<' => :shl, '>>' => :shr }
  e = [omap[base], ls2, rs2]
  [store_through(addr_sx, cast(e, CType::INT, elem), elem), elem]
end

def compile_update(node, fn, src)
  # ++x / x++ / --x / x--   (also *p++ etc)
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
  delta = op_node.type.to_s == '++' ? 1 : -1

  # Address of the lvalue + element type:
  case arg_node.type.to_s
  when 'identifier'
    name = text(arg_node, src)
    if fn.locals_map.key?(name)
      idx, ty = fn.locals_map[name]
      cur = lvar_load(idx, ty, :local)
      newv = update_op(cur, ty, delta)
      sett = lvar_store(idx, ty, newv, :local)
      old  = update_op_inverse(cur, ty, delta)
      return [(prefix ? sett : [:seq, sett, old]), ty]
    elsif GLOBALS.key?(name)
      idx, ty = GLOBALS[name]
      cur = lvar_load(idx, ty, :global)
      newv = update_op(cur, ty, delta)
      sett = lvar_store(idx, ty, newv, :global)
      old  = update_op_inverse(cur, ty, delta)
      return [(prefix ? sett : [:seq, sett, old]), ty]
    end
    raise CompileError, "undefined: #{name}"
  when 'pointer_expression', 'unary_expression'
    op = text(arg_node.child_by_field_name('operator'), src)
    raise CompileError, "update on non-deref" unless op == '*'
    inner = arg_node.child_by_field_name('argument')
    ps, pt = compile_expr(inner, fn, src)
    elem = pt.inner
    cur = load_through(ps, elem)
    newv = update_op(cur, elem, delta)
    sett = store_through(ps, newv, elem)
    old  = update_op_inverse(cur, elem, delta)
    return [(prefix ? sett : [:seq, sett, old]), elem]
  when 'subscript_expression'
    arr_sx, arr_ty = compile_expr(arg_node.child_by_field_name('argument'), fn, src)
    idx_sx, idx_ty = compile_expr(arg_node.child_by_field_name('index'), fn, src)
    elem = arr_ty.inner
    addr = [:ptr_add, arr_sx, cast(idx_sx, idx_ty, CType::LONG)]
    cur = load_through(addr, elem)
    newv = update_op(cur, elem, delta)
    sett = store_through(addr, newv, elem)
    old  = update_op_inverse(cur, elem, delta)
    return [(prefix ? sett : [:seq, sett, old]), elem]
  when 'field_expression'
    addr, ftype = field_address(arg_node, fn, src)
    cur = load_through(addr, ftype)
    newv = update_op(cur, ftype, delta)
    sett = store_through(addr, newv, ftype)
    old  = update_op_inverse(cur, ftype, delta)
    return [(prefix ? sett : [:seq, sett, old]), ftype]
  end
  raise CompileError, "update_expression: unsupported lvalue"
end

def update_op(cur, ty, delta)
  if ty.float?
    [:add_d, cur, [:lit_d, delta.to_f]]
  elsif ty.ptr_like?
    delta > 0 ? [:ptr_add, cur, [:lit_i, delta]]
              : [:ptr_sub_i, cur, [:lit_i, -delta]]
  else
    [:add_i, cur, [:lit_i, delta]]
  end
end

def update_op_inverse(cur, ty, delta)
  if ty.float?
    [:sub_d, cur, [:lit_d, delta.to_f]]
  elsif ty.ptr_like?
    delta > 0 ? [:ptr_sub_i, cur, [:lit_i, delta]]
              : [:ptr_add, cur, [:lit_i, -delta]]
  else
    [:sub_i, cur, [:lit_i, delta]]
  end
end

# field_expression  s.f  or  s->f
# Returns [load_sx, field_type].
def compile_field(node, fn, src)
  addr, ftype = field_address(node, fn, src)
  [load_through(addr, ftype), ftype.decay]
end

def field_address(node, fn, src)
  obj_node = node.child_by_field_name('argument')
  field_node = node.child_by_field_name('field')
  fname = text(field_node, src)

  obj_sx, obj_ty = compile_expr(obj_node, fn, src)

  # `->` is when obj is a pointer; `.` when it's a struct.  tree-sitter
  # stores the operator as an unnamed child of the node — peek the source.
  arrow = text(node, src).include?('->')

  if arrow
    raise CompileError, "-> needs pointer to struct" unless obj_ty.ptr_like?
    struct_ty = obj_ty.inner
    base_addr = obj_sx
  else
    raise CompileError, "field access on non-struct (got #{obj_ty})" unless obj_ty.struct?
    struct_ty = obj_ty
    # Need address-of the struct value.  We re-evaluate the lvalue as
    # an address (only identifiers/derefs allowed for simplicity).
    base_addr = address_of_struct_value(obj_node, fn, src)
  end

  info = STRUCTS[struct_ty.name]
  raise CompileError, "unknown struct #{struct_ty.name}" if info.nil?
  raise CompileError, "no field #{fname} in struct #{struct_ty.name}" unless info[:offsets].key?(fname)
  off_slots = info[:offsets][fname]
  ftype = info[:fields].find { |n, _| n == fname }[1]
  addr = off_slots == 0 ? base_addr : [:ptr_add, base_addr, [:lit_i, off_slots]]
  [addr, ftype]
end

def address_of_struct_value(obj_node, fn, src)
  k = obj_node.type.to_s
  case k
  when 'identifier'
    name = text(obj_node, src)
    if fn.locals_map.key?(name)
      idx, _ = fn.locals_map[name]
      return [:addr_local, idx]
    elsif GLOBALS.key?(name)
      idx, _ = GLOBALS[name]
      return [:addr_global, idx]
    end
    raise CompileError, "undefined struct: #{name}"
  when 'parenthesized_expression'
    inner = nil; obj_node.each_named { |c| inner = c; break }
    address_of_struct_value(inner, fn, src)
  when 'pointer_expression', 'unary_expression'
    op = text(obj_node.child_by_field_name('operator'), src)
    if op == '*'
      # *p — the address is just p
      ps, _ = compile_expr(obj_node.child_by_field_name('argument'), fn, src)
      return ps
    end
    raise CompileError, "address of struct: #{k} #{op}"
  when 'field_expression'
    addr, _ = field_address(obj_node, fn, src)
    addr
  when 'subscript_expression'
    arr_sx, arr_ty = compile_expr(obj_node.child_by_field_name('argument'), fn, src)
    idx_sx, idx_ty = compile_expr(obj_node.child_by_field_name('index'), fn, src)
    [:ptr_add, arr_sx, cast(idx_sx, idx_ty, CType::LONG)]
  else
    raise CompileError, "address of struct: #{k}"
  end
end

def compile_call(node, fn, src)
  callee = node.child_by_field_name('function')
  args_node = node.child_by_field_name('arguments')
  args = []
  args_node.each_named { |a| args << a }

  # Direct identifier call?
  if callee.type.to_s == 'identifier'
    fname = text(callee, src)

    # If the name resolves to a local/global variable (a function-pointer
    # value rather than a known function symbol), treat the call as
    # indirect.  This is what makes `int (*op)(int,int)` parameters
    # callable as `op(a, b)`.
    if fn.locals_map.key?(fname) || GLOBALS.key?(fname)
      idx, ty = (fn.locals_map[fname] || GLOBALS[fname])
      scope = fn.locals_map.key?(fname) ? :local : :global
      fn_value = lvar_load(idx, ty, scope)
      arg_index = fn.next_local
      fn.next_local += args.length
      seq_ops = []
      args.each_with_index do |a, i|
        asx, _ = compile_expr(a, fn, src)
        seq_ops << [:lset, arg_index + i, asx]
      end
      fn.next_local -= args.length
      chain = [:call_indirect, fn_value, args.length, arg_index]
      seq_ops.reverse_each { |s| chain = [:seq, s, chain] }
      return [chain, CType::INT]
    end

    # printf is variadic; runtime walks the format spec.
    if fname == 'printf'
      raise CompileError, 'printf needs a format' if args.empty?
      arg_index = fn.next_local
      fn.next_local += args.length
      seq_ops = []
      args.each_with_index do |a, i|
        asx, _ = compile_expr(a, fn, src)
        seq_ops << [:lset, arg_index + i, asx]
      end
      fn.next_local -= args.length
      chain = [:call_printf, args.length, arg_index]
      seq_ops.reverse_each { |s| chain = [:seq, s, chain] }
      return [chain, CType::INT]
    end

    if fname == 'putchar'
      raise CompileError, 'putchar takes 1 arg' unless args.length == 1
      asx, ats = compile_expr(args[0], fn, src)
      return [[:call_putchar, cast(asx, ats, CType::INT)], CType::INT]
    end
    if fname == 'puts'
      raise CompileError, 'puts takes 1 arg' unless args.length == 1
      asx, _ = compile_expr(args[0], fn, src)
      return [[:call_puts, asx], CType::INT]
    end

    # Other libc-ish builtins
    args_compiled = args.map { |a| compile_expr(a, fn, src) }
    bi = builtin_call(fname, args_compiled, fn)
    return bi if bi

    # User-defined function (must be in GLOBAL_FUNCS with a real idx).
    if GLOBAL_FUNCS.key?(fname)
      func_idx, ret_ty, param_tys = GLOBAL_FUNCS[fname]
      if func_idx.nil?
        raise CompileError, "call to extern-only function (no definition in TU): #{fname}"
      end
      raise CompileError, "arity mismatch: #{fname}" if args.length != param_tys.length
      arg_index = fn.next_local
      fn.next_local += args.length
      seq_ops = []
      args.each_with_index do |a, i|
        asx, ats = compile_expr(a, fn, src)
        seq_ops << [:lset, arg_index + i, cast(asx, ats, param_tys[i])]
      end
      fn.next_local -= args.length
      # Always emit a plain `:call` here; a post-pass after every
      # function body is fully compiled rewrites the call into
      # `:call_jmp` if the callee turned out to need a setjmp wrapper.
      # This makes the choice work even for self-recursion / mutual
      # recursion where the callee's `needs_setjmp` isn't yet known
      # at the time we emit the caller's body.
      chain = [:call, func_idx, args.length, arg_index]
      seq_ops.reverse_each { |s| chain = [:seq, s, chain] }
      return [chain, ret_ty]
    end

    raise CompileError, "call to undefined function: #{fname}"
  end

  # Indirect call: callee is some pointer-valued expression
  fnsx, fnty = compile_expr(callee, fn, src)
  arg_index = fn.next_local
  fn.next_local += args.length
  seq_ops = []
  args.each_with_index do |a, i|
    asx, _ = compile_expr(a, fn, src)
    seq_ops << [:lset, arg_index + i, asx]
  end
  fn.next_local -= args.length
  chain = [:call_indirect, fnsx, args.length, arg_index]
  seq_ops.reverse_each { |s| chain = [:seq, s, chain] }
  [chain, CType::INT]
end

# =====================================================================
# statement compiler
# =====================================================================

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
    base_ty = parse_type_spec(node.child_by_field_name('type'), src)
    seq_ops = []
    node.each_named do |c|
      case c.type.to_s
      when 'init_declarator'
        d = c.child_by_field_name('declarator')
        v = c.child_by_field_name('value')
        name, ty = parse_declarator(d, base_ty, src)
        next if name.nil?
        ty = infer_array_size(ty, v, src)
        idx = fn.add_local(name, ty)
        seq_ops.concat(emit_local_init(idx, ty, v, fn, src))
      when 'identifier', 'pointer_declarator', 'array_declarator'
        name, ty = parse_declarator(c, base_ty, src)
        next if name.nil?
        idx = fn.add_local(name, ty)
        seq_ops.concat(emit_local_init(idx, ty, nil, fn, src))
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
    e = nil; node.each_named { |c| e = c; break }
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
    pick_loop(:while, [:while, to_bool(cs, ct), body], body)

  when 'do_statement'
    body = compile_stmt(node.child_by_field_name('body'), fn, src)
    cs, ct = compile_expr(node.child_by_field_name('condition'), fn, src)
    pick_loop(:do_while, [:do_while, body, to_bool(cs, ct)], body)

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
    pick_loop(:for, [:for, init_sx, cs, upd_sx, body_sx], body_sx)

  when 'break_statement'    then [:break]
  when 'continue_statement' then [:continue]

  when 'switch_statement'
    compile_switch(node, fn, src)

  when 'else_clause'
    inner = nil; node.each_named { |c| inner = c; break }
    return [:nop] if inner.nil?
    compile_stmt(inner, fn, src)

  when 'labeled_statement'
    label_node = node.child_by_field_name('label')
    name = text(label_node, src)
    fn.uses_goto = true
    idx = fn.label_index(name)
    inner = nil
    node.each_named { |c| inner = c if c.type.to_s != 'statement_identifier' && c.start_byte > label_node.start_byte }
    inner_sx = inner ? compile_stmt(inner, fn, src) : [:nop]
    [:_label_marker, idx, inner_sx]

  when 'goto_statement'
    label_node = node.child_by_field_name('label')
    name = text(label_node, src)
    fn.uses_goto = true
    idx = fn.label_index(name)
    [:goto, idx]

  when 'comment', ';'
    [:nop]

  else
    raise CompileError, "unhandled stmt: #{k}"
  end
end

# emit_local_init: produce statements that initialize a freshly
# declared local at slot `idx` of type `ty`.  v_node is the optional
# initializer; nil for default (zero-fill).
def emit_local_init(idx, ty, v_node, fn, src)
  if ty.array?
    if v_node && v_node.type.to_s == 'initializer_list'
      # int a[5] = {1,2,3,4,5};
      elems = []
      v_node.each_named { |c| elems << c }
      out = []
      elems.each_with_index do |e, i|
        next if i >= ty.size
        es, et = compile_expr(e, fn, src)
        out << [:lset, idx + i, cast(es, et, ty.inner)]
      end
      return out
    end
    if v_node && v_node.type.to_s == 'string_literal'
      # char buf[N] = "hello"; — copy each byte into successive slots.
      s = parse_string_literal(v_node, src)
      out = []
      n = ty.size || (s.length + 1)
      (0...n).each do |i|
        b = i < s.length ? s.bytes[i] : 0
        out << [:lset, idx + i, [:lit_i, b]]
      end
      return out
    end
    # uninitialised — leave as zero (calloc'd / lset 0)
    return [[:lset, idx, [:lit_i, 0]]]
  end

  if v_node && v_node.type.to_s == 'initializer_list' && ty.struct?
    info = STRUCTS[ty.name]
    raise CompileError, "unknown struct #{ty.name}" unless info
    elems = []; v_node.each_named { |c| elems << c }
    out = []
    pos = 0
    elems.each do |e|
      if e.type.to_s == 'initializer_pair'
        designator = nil; val_node = nil
        e.each_named do |c|
          if %w[field_designator subscript_designator].include?(c.type.to_s)
            designator = c
          else
            val_node = c
          end
        end
        if designator && designator.type.to_s == 'field_designator'
          fname_node = nil
          designator.each_named { |c| fname_node = c; break }
          fname = text(fname_node, src)
          slot = idx + info[:offsets][fname]
          ftype = info[:fields].find { |n, _| n == fname }[1]
          es, et = compile_expr(val_node, fn, src)
          out << [:lset, slot, cast(es, et, ftype)]
        end
      else
        fname, fty = info[:fields][pos]
        next if fname.nil?
        slot = idx + info[:offsets][fname]
        es, et = compile_expr(e, fn, src)
        out << [:lset, slot, cast(es, et, fty)]
        pos += 1
      end
    end
    return out
  end

  if v_node
    es, et = compile_expr(v_node, fn, src)
    return [[:lset, idx, cast(es, et, ty)]]
  end

  if ty.float?
    [[:lset, idx, [:lit_d, 0.0]]]
  else
    [[:lset, idx, [:lit_i, 0]]]
  end
end

# =====================================================================
# switch lowering
# =====================================================================
def compile_switch(node, fn, src)
  cond_paren = nil
  body_node = nil
  node.each_named do |c|
    case c.type.to_s
    when 'parenthesized_expression', 'comma_expression' then cond_paren = c
    when 'compound_statement' then body_node = c
    end
  end
  raise CompileError, 'switch: missing condition or body' if cond_paren.nil? || body_node.nil?
  cond_inner = nil
  cond_paren.each_named { |c| cond_inner = c; break }
  cs, ct = compile_expr(cond_inner, fn, src)
  cs = cast(cs, ct, CType::INT)

  cases = []
  body_node.each_named do |st|
    next unless st.type.to_s == 'case_statement'
    val_node = st.child_by_field_name('value')
    val_start = val_node ? val_node.start_byte : nil
    stmts = []
    st.each_named do |k|
      next if val_start && k.start_byte == val_start
      stmts << compile_stmt(k, fn, src)
    end
    if val_node
      vsx, vty = compile_expr(val_node, fn, src)
      cases << [cast(vsx, vty, CType::INT), stmts]
    else
      cases << [nil, stmts]
    end
  end

  sw_idx = fn.add_local("__sw_#{node.start_byte}", CType::INT)
  matched_idx = fn.add_local("__matched_#{node.start_byte}", CType::INT)
  has_default = cases.any? { |v, _| v.nil? }
  default_idx = has_default ? fn.add_local("__default_#{node.start_byte}", CType::INT) : nil

  case_values = cases.map { |v, _| v }.compact
  default_check = case_values.inject([:lit_i, 1]) do |acc, v|
    [:land, acc, [:neq_i, [:lget, sw_idx], v]]
  end

  body_stmts = []
  body_stmts << [:lset, sw_idx, cs]
  body_stmts << [:lset, matched_idx, [:lit_i, 0]]
  body_stmts << [:lset, default_idx, default_check] if has_default

  cases.each do |val, stmts|
    cond =
      if val.nil?
        [:lor, [:lget, matched_idx], [:lget, default_idx]]
      else
        [:lor, [:lget, matched_idx], [:eq_i, [:lget, sw_idx], val]]
      end
    case_body =
      if stmts.empty?
        [:lset, matched_idx, [:lit_i, 1]]
      else
        rest = stmts.last
        stmts[0...-1].reverse_each { |s| rest = [:seq, s, rest] }
        [:seq, [:lset, matched_idx, [:lit_i, 1]], rest]
      end
    body_stmts << [:if, cond, case_body, [:nop]]
  end

  body_seq = body_stmts.last
  body_stmts[0...-1].reverse_each { |s| body_seq = [:seq, s, body_seq] }

  loop_kind = loop_escape_kind([:_dummy, body_seq])
  case loop_kind
  when :neither
    [:seq, body_seq, [:nop]]
  when :has_break
    [:do_while_brk_only, body_seq, [:lit_i, 0]]
  else
    [:do_while_brk, body_seq, [:lit_i, 0]]
  end
end

# =====================================================================
# goto lowering
# =====================================================================
#
# When a function uses `goto`, we lower its body into a label-dispatch
# loop:
#   {
#     while (1) {
#       switch (__label) {
#         case 0: <stmts up to first label>; break;
#         case 1: <stmts after L1>; break;
#         ...
#       }
#     }
#   }
# `goto L` sets __label = idx and longjmps back.  Each label-marker
# splits the body at the goto-target boundary.
def lower_goto(body, fn)
  return body unless fn.uses_goto

  # Collect label markers by walking the body tree, collecting
  # [idx, marker_position, segment_after].
  markers = collect_label_markers(body)
  # markers is [[idx, ...], ...] in source order.  We're going to lay
  # out a switch over the label index, with the entry segment as case 0
  # and each label as a successive case.

  # Replace markers with [:nop] and split the body into segments.
  # Easiest: linearize seq into a flat list, then split on markers.
  flat = flatten_seq(body)
  segments = [{ idx: 0, stmts: [] }]
  flat.each do |s|
    if s.is_a?(Array) && s[0] == :_label_marker
      idx, body_after = s[1], s[2]
      segments << { idx: idx, stmts: [body_after] }
    else
      segments.last[:stmts] << s
    end
  end

  # Build:
  #   while (1) {
  #     switch (goto_target) {
  #       case 0: seg0; break;
  #       case 1: seg1; break;
  #       ...
  #       default: break;
  #     }
  #   }
  # We don't want labels to "fall through"; goto will set the next
  # target explicitly.  After each segment, fall-through to next
  # segment — like C does after a label.

  # Combined seq for each segment, then each segment dispatches to
  # the next on fall-through by setting goto_target manually.
  cases_sx = []
  segments.each_with_index do |seg, i|
    body_sx = combine_seq(seg[:stmts])
    cases_sx << [:if,
                 [:eq_i, [:goto_target], [:lit_i, seg[:idx]]],
                 [:seq,
                  body_sx,
                  # After segment, set target to next segment's idx so
                  # control naturally flows on fall-through.
                  i + 1 < segments.length ?
                    [:goto, segments[i + 1][:idx]] :
                    [:break]],
                 [:nop]]
  end

  body_sx = combine_seq(cases_sx)
  # Wrap in a while(true)_brk so `[:break]` exits.
  [:goto_dispatch,
   [:while_brk_only, [:lit_i, 1], body_sx]]
end

def collect_label_markers(node)
  result = []
  walk = lambda do |n|
    return unless n.is_a?(Array)
    if n[0] == :_label_marker
      result << n[1]
    end
    n.each { |c| walk.call(c) }
  end
  walk.call(node)
  result
end

def flatten_seq(node)
  return [node] unless node.is_a?(Array) && node[0] == :seq
  flatten_seq(node[1]) + flatten_seq(node[2])
end

def combine_seq(stmts)
  return [:nop] if stmts.empty?
  out = stmts.last
  stmts[0...-1].reverse_each { |s| out = [:seq, s, out] }
  out
end

# =====================================================================
# top-level: signatures, globals, functions
# =====================================================================
def gather_signatures(root, src)
  next_idx = 0
  root.each_named do |fdef|
    case fdef.type.to_s
    when 'function_definition'
      ret_base = parse_type_spec(fdef.child_by_field_name('type'), src)
      np = unwrap_decl(fdef.child_by_field_name('declarator'), src, ret_base)
      next if np.nil?
      name, params, ret_ty = np
      param_tys = []
      if params
        params.each_named do |p|
          next unless p.type.to_s == 'parameter_declaration'
          base = parse_type_spec(p.child_by_field_name('type'), src)
          d = p.child_by_field_name('declarator')
          _, pty = parse_declarator(d, base, src) if d
          pty ||= base
          # `f(void)` signals "no parameters" — skip the synthetic void.
          next if pty.void? && d.nil?
          param_tys << pty.decay
        end
      end
      # Function definitions get a stable index; the runtime calls them
      # via `c->func_set[idx]` directly with no name lookup.
      GLOBAL_FUNCS[name] = [next_idx, ret_ty, param_tys]
      next_idx += 1
    when 'declaration'
      # extern function prototypes — signature only, no idx assigned.
      # Calling such a function with no real definition in this TU is
      # rejected by compile_call.
      base = parse_type_spec(fdef.child_by_field_name('type'), src)
      fdef.each_named do |c|
        next unless %w[function_declarator pointer_declarator].include?(c.type.to_s)
        cur = c
        ret = base
        while cur && cur.type.to_s == 'pointer_declarator'
          ret = CType.ptr(ret)
          cur = cur.child_by_field_name('declarator')
        end
        next if cur.nil? || cur.type.to_s != 'function_declarator'
        inner = cur.child_by_field_name('declarator')
        params = cur.child_by_field_name('parameters')
        next if inner.nil? || inner.type.to_s != 'identifier'
        name = text(inner, src)
        param_tys = []
        params.each_named do |p|
          next unless p.type.to_s == 'parameter_declaration'
          pbase = parse_type_spec(p.child_by_field_name('type'), src)
          pd = p.child_by_field_name('declarator')
          _, pty = parse_declarator(pd, pbase, src) if pd
          pty ||= pbase
          param_tys << pty
        end
        # Don't clobber an entry already given an idx by a real
        # function_definition; only register if first-seen as extern.
        unless GLOBAL_FUNCS.key?(name) && GLOBAL_FUNCS[name][0]
          GLOBAL_FUNCS[name] = [nil, ret, param_tys]
        end
      end
    end
  end
end

# Process a top-level declaration: a global variable, struct decl,
# typedef, or enum.  Returns initializer SX forms (added to GLOBAL_INITS).
def process_top_decl(node, src)
  base_ty = parse_type_spec(node.child_by_field_name('type'), src)

  # Typedef?
  if text(node, src).strip.start_with?('typedef')
    node.each_named do |c|
      name, ty = parse_declarator(c, base_ty, src)
      TYPEDEFS[name] = ty if name
    end
    return
  end

  # enum support: parse_type_spec on enum_specifier returns INT and
  # any enumerators land in ENUMS via collect_enums at parse start.

  # Plain global variable / array / struct
  node.each_named do |c|
    case c.type.to_s
    when 'init_declarator'
      d = c.child_by_field_name('declarator')
      v = c.child_by_field_name('value')
      name, ty = parse_declarator(d, base_ty, src)
      next if name.nil? || (ty.void?)
      ty = infer_array_size(ty, v, src)
      idx = alloc_global_slot(name, ty)
      # Update GLOBALS entry with refined type before init runs.
      GLOBALS[name] = [idx, ty]
      add_global_init(idx, ty, v, src)
    when 'identifier', 'pointer_declarator', 'array_declarator'
      name, ty = parse_declarator(c, base_ty, src)
      next if name.nil?
      alloc_global_slot(name, ty)
    when 'function_declarator'
      # extern decl, signatures already gathered
    end
  end
end

# If `ty` is an array with unspecified size, look at the initializer
# to deduce length: `char s[] = "abc"` → 4 slots; `int a[] = {1,2,3}` → 3.
def infer_array_size(ty, v_node, src)
  return ty unless ty.array? && ty.size.nil?
  return ty if v_node.nil?
  case v_node.type.to_s
  when 'string_literal', 'concatenated_string'
    n = parse_string_literal(v_node, src).length + 1
    CType.array(ty.inner, n)
  when 'initializer_list'
    cnt = 0; v_node.each_named { cnt += 1 }
    CType.array(ty.inner, cnt)
  else ty
  end
end

# `type_definition` is tree-sitter-c's node for a typedef declaration.
# It carries the underlying type as the first named child and one or
# more declarators (each declarator is the new alias name, possibly
# with pointer/array decorators applied).
def process_typedef(node, src)
  # tree-sitter-c lays out a `type_definition` as
  #   <type_specifier>  <declarator>  [<declarator> ...]
  # The first named child is the type; everything after is an alias
  # declarator (which may carry pointer/array decorators).
  type_node = nil
  decl_nodes = []
  node.each_named do |c|
    if type_node.nil?
      type_node = c
    else
      decl_nodes << c
    end
  end
  base = type_node ? parse_type_spec(type_node, src) : CType::INT
  decl_nodes.each do |d|
    nm, ty = parse_declarator(d, base, src)
    next if nm.nil?
    TYPEDEFS[nm] = ty
  end
end

def alloc_global_slot(name, ty)
  return GLOBALS[name][0] if GLOBALS.key?(name)
  idx = GLOBAL_NEXT
  GLOBALS[name] = [idx, ty]
  set_global_next(idx + ty.slot_count)
  idx
end

def add_global_init(idx, ty, v_node, src)
  return if v_node.nil?
  init_fn = Func.new('__init__', CType::VOID)
  if ty.array?
    if v_node.type.to_s == 'initializer_list'
      elems = []; v_node.each_named { |c| elems << c }
      elems.each_with_index do |e, i|
        next if i >= (ty.size || elems.length)
        es, et = compile_expr(e, init_fn, src)
        GLOBAL_INITS << [:gset, idx + i, cast(es, et, ty.inner)]
      end
      return
    end
    if v_node.type.to_s == 'string_literal'
      s = parse_string_literal(v_node, src)
      n = ty.size || (s.length + 1)
      (0...n).each do |i|
        b = i < s.length ? s.bytes[i] : 0
        GLOBAL_INITS << [:gset, idx + i, [:lit_i, b]]
      end
      return
    end
  end
  if ty.struct? && v_node.type.to_s == 'initializer_list'
    info = STRUCTS[ty.name]
    raise CompileError, "unknown struct #{ty.name}" unless info
    elems = []; v_node.each_named { |c| elems << c }
    pos = 0
    elems.each do |e|
      if e.type.to_s == 'initializer_pair'
        # `.field = value` form — use the designator
        designator = nil
        e.each_named { |c|
          if c.type.to_s == 'field_designator' || c.type.to_s == 'subscript_designator'
            designator = c
          end
        }
        val_node = e.child_by_field_name('value') || begin
          last = nil; e.each_named { |c| last = c if c.type.to_s != 'field_designator' && c.type.to_s != 'subscript_designator' }; last
        end
        if designator && designator.type.to_s == 'field_designator'
          fname_node = nil
          designator.each_named { |c| fname_node = c; break }
          fname = text(fname_node, src)
          slot = idx + info[:offsets][fname]
          ftype = info[:fields].find { |n, _| n == fname }[1]
          es, et = compile_expr(val_node, init_fn, src)
          GLOBAL_INITS << [:gset, slot, cast(es, et, ftype)]
        end
      else
        # Positional element
        fname, fty = info[:fields][pos]
        next if fname.nil?
        slot = idx + info[:offsets][fname]
        es, et = compile_expr(e, init_fn, src)
        GLOBAL_INITS << [:gset, slot, cast(es, et, fty)]
        pos += 1
      end
    end
    return
  end
  es, et = compile_expr(v_node, init_fn, src)
  GLOBAL_INITS << [:gset, idx, cast(es, et, ty)]
end

# Walk all enumerator constants up-front so they're visible everywhere.
def collect_enums(root, src)
  walk = lambda do |n|
    if n.type.to_s == 'enumerator'
      name = text(n.child_by_field_name('name'), src)
      val_node = n.child_by_field_name('value')
      val = val_node ? eval_const_int(val_node, src) : nil
      val ||= (ENUMS.values.last || -1) + 1
      ENUMS[name] = val
    end
    n.each { |c| walk.call(c) }
  end
  walk.call(root)
end

# =====================================================================
# Function body compilation + return-lifting + goto-lowering
# =====================================================================
def compile_function(fdef, src)
  ret_base = parse_type_spec(fdef.child_by_field_name('type'), src)
  np = unwrap_decl(fdef.child_by_field_name('declarator'), src, ret_base)
  raise CompileError, 'function with no name' if np.nil?
  name, params, ret_ty = np
  fn = Func.new(name, ret_ty)
  if params
    params.each_named do |p|
      next unless p.type.to_s == 'parameter_declaration'
      pbase = parse_type_spec(p.child_by_field_name('type'), src)
      pd = p.child_by_field_name('declarator')
      # `f(void)` signals "no parameters" — skip.
      next if pd.nil? && pbase.void?
      pname = nil; pty = pbase
      pname, pty = parse_declarator(pd, pbase, src) if pd
      next if pname.nil?
      pty = pty.decay
      fn.add_param(pname, pty)
    end
  end
  body = fdef.child_by_field_name('body')
  body_sx = compile_stmt(body, fn, src)
  body_sx = [:seq, body_sx, [:return_void]]
  body_sx = lift_tail(body_sx)
  body_sx = lower_goto(body_sx, fn) if fn.uses_goto
  has_returns = has_return?(body_sx) ? 1 : 0
  # Record whether this function needs setjmp wrapping at call sites
  # — used by compile_call to pick `call` vs `call_jmp`.
  func_idx = GLOBAL_FUNCS.key?(name) ? GLOBAL_FUNCS[name][0] : nil
  FUNC_NEEDS_SETJMP[func_idx] = has_returns == 1 if func_idx
  [name, fn, body_sx, ret_ty, has_returns]
end

# =====================================================================
# preprocessor
# =====================================================================
def run_cpp(path)
  # Use gcc -E to expand #include / #define / #if conditionals.  If gcc
  # is unavailable or fails, fall back to reading the raw source.
  out, _, st = Open3.capture3('gcc', '-E', '-P', '-x', 'c', path)
  return out if st.success?
  File.binread(path)
rescue StandardError
  File.binread(path)
end

# =====================================================================
# main driver
# =====================================================================
def main
  if ARGV.empty?
    warn 'usage: parse.rb <file.c>'
    exit 1
  end

  source_path = ARGV[0]
  use_cpp = !ENV['NO_CPP']
  src = use_cpp ? run_cpp(source_path) : File.binread(source_path)
  src = src.force_encoding('UTF-8') rescue src

  tree = PARSER.parse_string(nil, src)
  root = tree.root_node
  warn "warning: parse errors in #{source_path}" if root.has_error?

  collect_enums(root, src)
  gather_signatures(root, src)

  # First pass: process top-level declarations/typedefs/structs/globals.
  root.each_named do |c|
    case c.type.to_s
    when 'declaration'
      process_top_decl(c, src)
    when 'type_definition'
      process_typedef(c, src)
    when 'struct_specifier'
      parse_type_spec(c, src)  # registers the struct
    end
  end

  # Second pass: compile function bodies.
  funcs = []
  root.each_named do |fdef|
    next unless fdef.type.to_s == 'function_definition'
    begin
      funcs << compile_function(fdef, src)
    rescue CompileError => e
      warn "error in #{source_path}: #{e.message}"
      exit 2
    end
  end

  # Post-pass: now that every function's `has_returns` is known, swap
  # any `:call` whose callee needs setjmp to `:call_jmp`.  Without this,
  # self-recursion silently picked the no-jmp variant during the inner
  # `compile_function` (FUNC_NEEDS_SETJMP[idx] hadn't been written yet).
  patch_call_jmp = lambda do |node|
    next unless node.is_a?(Array)
    if node[0] == :call
      callee = node[1]
      node[0] = :call_jmp if FUNC_NEEDS_SETJMP[callee]
    end
    node.each { |c| patch_call_jmp.call(c) }
  end
  funcs.each { |_, _, body_sx, _, _| patch_call_jmp.call(body_sx) }

  # Output layout (signatures first, then bodies in matching order):
  #
  #   (program GSIZE NFUNCS
  #     INIT_EXPR
  #     (sig NAME P L T HR) ... (NFUNCS times)
  #     BODY_EXPR ...        (NFUNCS times in same order))
  #
  # The C side reads sigs first to register stub function_entry's, then
  # parses bodies — that way calls inside a body can resolve any other
  # function by index even when it's defined later in the SX stream.
  out = +''
  out << "(program #{GLOBAL_NEXT} #{funcs.length}\n  "
  init_expr = if GLOBAL_INITS.empty?
    [:nop]
  else
    GLOBAL_INITS.reverse.inject([:nop]) { |acc, s| [:seq, s, acc] }
  end
  emit_sx(init_expr, out)
  out << "\n"
  funcs.each do |name, fn, body_sx, ret_ty, has_returns|
    out << "  (sig #{name} #{fn.params.length} #{fn.next_local} #{ret_ty.slot_kind} #{has_returns})\n"
  end
  funcs.each do |name, fn, body_sx, ret_ty, has_returns|
    out << "  "
    emit_sx(body_sx, out)
    out << "\n"
  end
  out << ")\n"
  $stdout.write(out)
end

main if $0 == __FILE__
