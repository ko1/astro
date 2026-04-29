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
  class Operand < ASTroGen::NodeDef::Node::Operand
    # Hash a Lua identifier (interned LuaString *) by its underlying bytes
    # so that identical names produce identical hashes regardless of intern
    # pointer identity.
    def hash_call(val, kind: :horg)
      case @type
      when 'LuaString *'
        "hash_cstr(lua_str_data(#{val}))"
      else
        super
      end
    end

    def build_dumper(name)
      case @type
      when 'LuaString *'
        "        astro_fprintf_cstr(fp, lua_str_data(n->u.#{name}.#{self.name}));"
      else
        super
      end
    end

    def build_specializer(name)
      arg = case @type
            when 'LuaString *'
              # Re-intern by C string at SD load time so the SD_ source can
              # be cached on disk and reloaded across runs.
              "    fprintf(fp, \"        lua_str_intern(\");\n" +
              "    astro_fprint_cstr(fp, lua_str_data(n->u.#{name}.#{self.name}));\n" +
              "    fprintf(fp, \")\");"
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
end
