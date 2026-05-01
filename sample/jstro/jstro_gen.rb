require 'astrogen'

# jstro's per-language ASTroGen extension.  Modeled after lua_gen.rb.
# Differences from default:
#   1. Return type is RESULT (== JsValue, a uint64_t).
#   2. Common parameter count is 3 — every NODE_DEF takes
#      (CTX *c, NODE *n, JsValue *frame, ...).
#   3. Adds operand types for JsString *, JsPropIC *, JsCallIC *, etc.
class JstroNodeDef < ASTroGen::NodeDef
  # Hopt (profile-aware hash) — generates node_hopt.c with HOPT_<name>
  # functions and wires .hopt_func on each NodeKind.  Used by `-p` PGC
  # flow so swapped specialised variants get distinct PGSDs while the
  # original (canonical) kind name is the HORG lookup key.
  register_gen_task :hopt,
    func_typedef: "typedef node_hash_t (*node_hash_func_t)(struct Node *n);",
    func_prefix: "HOPT_",
    kind_field: "node_hash_func_t hopt_func"

  class Operand < ASTroGen::NodeDef::Node::Operand
    def hash_call(val, kind: :horg)
      case @type
      when 'JsString *', 'struct JsString *'
        # hash by underlying bytes so identical names hash equal regardless
        # of intern-pool pointer identity (mostly defensive — strings are
        # interned, so pointer equality already implies content equality).
        # Handle NULL (anonymous function names, optional identifiers) so
        # hash_cstr doesn't dereference garbage.
        "(#{val} ? hash_cstr(js_str_data(#{val})) : 0)"
      when 'struct JsPropIC *', 'struct JsCallIC *', 'struct JsObjLitIC *'
        '0'
      else
        super
      end
    end

    def build_dumper(name)
      case @type
      when 'JsString *', 'struct JsString *'
        "        astro_fprintf_cstr(fp, n->u.#{name}.#{self.name} ? js_str_data(n->u.#{name}.#{self.name}) : NULL);"
      when 'struct JsPropIC *', 'struct JsCallIC *', 'struct JsObjLitIC *'
        "        fprintf(fp, \"<ic>\");"
      else
        super
      end
    end

    def build_specializer(name)
      if @type == 'struct JsPropIC *' || @type == 'struct JsCallIC *' || @type == 'struct JsObjLitIC *'
        # @ref operand: address-of the inline IC slot.
        arg = "    fprintf(fp, \"        &n->u.#{name}.#{self.name}\");"
        return nil, arg
      end
      arg = case @type
            when 'JsString *', 'struct JsString *'
              "    fprintf(fp, \"        n->u.#{name}.#{self.name}\");"
            when 'uint64_t'
              "    fprintf(fp, \"        %lluULL\", (unsigned long long)n->u.#{name}.#{self.name});"
            else
              return super
            end
      [nil, arg]
    end
  end

  # Wires the :hopt task's per-NODE template into node_hopt.c.
  def build_hopt
    <<~C__
    // This file is auto-generated from #{@file}.
    // Hopt (profile-aware) hash functions

    #{@nodes.map{|name, n| n.build_hopt_func}.join("\n")}
    C__
  end

  class Node < ASTroGen::NodeDef::Node
    def result_type = "RESULT"

    def common_param_count
      3
    end

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
