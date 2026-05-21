require 'astrogen'

class KoRubyNodeDef < ASTroGen::NodeDef
  class Node < ASTroGen::NodeDef::Node
    class Operand < ASTroGen::NodeDef::Node::Operand
      def hash_call(val, kind: :horg)
        return "0" if ref?
        case @type
        when 'ID'
          "hash_cstr(korb_id_name(#{val}))"
        when 'intptr_t'
          "hash_uint64((uint64_t)#{val})"
        when 'struct method_cache *', 'struct call_cache *', 'struct ivar_cache *', 'struct korb_proc *', 'struct korb_class *'
          "0"
        else
          super
        end
      end

      def build_dumper(name)
        return nil if ref?
        return nil if storageless?
        case @type
        when 'ID'
          "        fprintf(fp, \"%s\", korb_id_name(n->u.#{name}.#{self.name}));"
        when 'intptr_t'
          "        fprintf(fp, \"%ld\", (long)n->u.#{name}.#{self.name});"
        when 'struct method_cache *', 'struct call_cache *'
          "        fprintf(fp, \"<cache>\");"
        else
          super
        end
      end

      def build_specializer(name)
        if ref?
          arg = "    fprintf(fp, \"        &n->u.#{name}.#{self.name}\");"
          return nil, arg
        end
        case @type
        when 'ID'
          arg = "    fprintf(fp, \"        n->u.#{name}.#{self.name}\");"
          return nil, arg
        when 'intptr_t'
          arg = "    fprintf(fp, \"        (intptr_t)%ld\", (long)n->u.#{name}.#{self.name});"
          return nil, arg
        when 'struct method_cache *', 'struct call_cache *', 'struct ivar_cache *', 'struct korb_proc *', 'struct korb_class *'
          arg = "    fprintf(fp, \"        n->u.#{name}.#{self.name}\");"
          return nil, arg
        else
          super
        end
      end

      # --generate-executable: emit a C expression that reconstructs this
      # operand at exe runtime.  ID values are not stable across processes
      # (interning order varies), so we look the original name up via
      # `korb_id_name(id)` at emit time and re-intern via `korb_intern(...)`
      # at exe runtime.  method_cache / ivar_cache pointers point at
      # per-call-site fresh memory — at exe runtime we allocate a new
      # one for each node so each call site has its own cache slot.
      def build_emit_ast(name)
        return nil if ref? || storageless?
        field = "n->u.#{name}.#{self.name}"
        case @type
        when 'ID'
          # Bake the symbol name as a C string literal; exe re-interns
          # at startup via korb_intern.
          <<~C.chomp
              fprintf(fp, "korb_intern(");
              astro_fprintf_cstr(fp, korb_id_name(#{field}));
              fprintf(fp, ")");
          C
        when 'struct method_cache *'
          # Fresh cache per call site; emit unconditionally (even when
          # the runtime ptr is NULL — node_func_call etc. always have a
          # mc allocated by parse.c).
          "    fprintf(fp, \"koruby_alloc_method_cache()\");"
        when 'struct ivar_cache *'
          "    fprintf(fp, \"koruby_alloc_ivar_cache()\");"
        when 'struct call_cache *', 'struct korb_proc *', 'struct korb_class *'
          # No emit support — these aren't expected outside @ref slots;
          # if we ever see one as a regular operand, bake NULL and rely
          # on first-use init.  The embedded AST builder will need a
          # paired runtime fixup if these slots are actually consumed.
          "    fprintf(fp, \"NULL\");"
        else
          super
        end
      end
    end
  end
end
