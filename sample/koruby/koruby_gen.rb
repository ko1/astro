require 'astrogen'

class KoRubyNodeDef < ASTroGen::NodeDef
  class Node < ASTroGen::NodeDef::Node
    class Operand < ASTroGen::NodeDef::Node::Operand
      def hash_call(val, kind: :horg)
        return "0" if ref?
        case @type
        when 'ID'
          "hash_cstr(ko_id_name(#{val}))"
        when 'intptr_t'
          "hash_uint64((uint64_t)#{val})"
        when 'struct method_cache *', 'struct call_cache *', 'struct ko_proc *', 'struct ko_class *'
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
          "        fprintf(fp, \"%s\", ko_id_name(n->u.#{name}.#{self.name}));"
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
        when 'struct method_cache *', 'struct call_cache *', 'struct ko_proc *', 'struct ko_class *'
          arg = "    fprintf(fp, \"        n->u.#{name}.#{self.name}\");"
          return nil, arg
        else
          super
        end
      end
    end
  end
end
