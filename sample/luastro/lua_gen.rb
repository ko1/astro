require 'astrogen'

# luastro's per-language ASTroGen extension.
#
# Differences from the default:
#   1. Return type is RESULT — a 16-byte struct `{ LuaValue value; uint32_t br; }`
#      that fits in two registers under SysV x86_64 so branch state never
#      hits memory.
#   2. Common parameter count is 3 — every NODE_DEF takes
#      `(CTX * restrict c, NODE * restrict n, LuaValue * restrict frame, ...)`.
#      The third parameter is the per-call frame slot array (locals + work).
#   3. We add an `ID` operand type for Lua identifier names (interned strings).
class LuaNodeDef < ASTroGen::NodeDef
  # Hopt (profile-aware hash) — generates node_hopt.c with HOPT_<name>
  # functions and wires .hopt_func on each NodeKind.  Sits alongside the
  # base framework's :hash task (Horg, file node_hash.c, .hash_func field).
  # Used by `-p` PGC flow: HORG (structural) is canonicalised so swapped
  # nodes still map to the same SD lookup key, while HOPT (profile-aware)
  # uses the *actual* kind name so the baked SD encodes the post-swap
  # specialised body.  hopt_index.txt maps (HORG, file, line) → HOPT.
  register_gen_task :hopt,
    func_typedef: "typedef node_hash_t (*node_hash_func_t)(struct Node *n);",
    func_prefix: "HOPT_",
    kind_field: "node_hash_func_t hopt_func"

  class Operand < ASTroGen::NodeDef::Node::Operand
    # Hash a Lua identifier (interned LuaString *) by its underlying bytes
    # so that identical names produce identical hashes regardless of intern
    # pointer identity.
    def hash_call(val, kind: :horg)
      case @type
      when 'LuaString *'
        "hash_cstr(lua_str_data(#{val}))"
      when 'struct LuaFieldIC *', 'struct LuaCallIC *', 'struct LuaMethodIC *'
        # Inline-cache @ref slot — runtime mutable state, not part of
        # the structural hash.  Two AST nodes with identical shape but
        # different cache values must hash equal.
        '0'
      else
        super
      end
    end

    def build_dumper(name)
      case @type
      when 'LuaString *'
        "        astro_fprintf_cstr(fp, lua_str_data(n->u.#{name}.#{self.name}));"
      when 'struct LuaFieldIC *', 'struct LuaCallIC *', 'struct LuaMethodIC *'
        "        fprintf(fp, \"<ic>\");"
      else
        super
      end
    end

    def build_specializer(name)
      if @type == 'struct LuaFieldIC *' || @type == 'struct LuaCallIC *' || @type == 'struct LuaMethodIC *'
        # @ref operand: emit a reference to the IC slot embedded in
        # the AST node so the SD body can read/update it directly.
        # The struct field is inline (the @ref strips the pointer in
        # `struct_field_join`), so we take its address.
        arg = "    fprintf(fp, \"        &n->u.#{name}.#{self.name}\");"
        return nil, arg
      end
      arg = case @type
            when 'LuaString *'
              # The parser already interned this string and stored the
              # `LuaString *` on the AST node, so the SD just reads it
              # back via `n->u.<kind>.<field>`.  Earlier we emitted a
              # literal `lua_str_intern("x")` here so the SD source
              # would be portable across runs (a different process's
              # intern pool produces different pointers); but that
              # meant every dispatch re-interned the same C string,
              # which on `nbody` ate 21.5% of cycles.  The AST is
              # always rebuilt by the parser before the SD runs, so
              # the field is fresh and process-local.
              "    fprintf(fp, \"        n->u.#{name}.#{self.name}\");"
            when 'uint64_t'
              # Default emits "(VALUE)%lluULL" which assumes VALUE is a
              # scalar; for luastro VALUE is a struct so we need a plain
              # uint64_t literal instead.
              "    fprintf(fp, \"        %lluULL\", (unsigned long long)n->u.#{name}.#{self.name});"
            else
              return super
            end
      [nil, arg]
    end
  end

  class Node < ASTroGen::NodeDef::Node
    def result_type = "RESULT"

    def common_param_count
      3
    end

    # Use our extended Operand
    def parse_operands(str)
      @operands = str.split(',').tap do
        @prefix_args = it.shift(common_param_count)
      end.map do
        case it.strip
        when /(.+)\s+([a-zA-Z_][a-zA-Z0-9_]*(?:@ref)?)$/
          Operand.new($1, $2)
        when /(.+\*)([a-zA-Z_][a-zA-Z0-9_]*(?:@ref)?)$/
          Operand.new($1, $2)
        else
          raise "ill-formed field: #{it}"
        end
      end
    end
  end

  # Wires the :hopt task's per-NODE template into a single node_hopt.c file.
  # Mirrors the framework's existing :hash → build_hash plumbing.
  def build_hopt
    <<~C__
    // This file is auto-generated from #{@file}.
    // Hopt (profile-aware) hash functions

    #{@nodes.map{|name, n| n.build_hopt_func}.join("\n")}
    C__
  end
end
