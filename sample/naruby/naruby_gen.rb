require 'astrogen'

class NaRubyNodeDef < ASTroGen::NodeDef
  class Node < ASTroGen::NodeDef::Node
    class Operand < ASTroGen::NodeDef::Node::Operand
      def hash_call(val)
        case @type
        when 'struct builtin_func *'
          "hash_builtin_func(#{val})"
        when 'builtin_func_ptr', 'state_serial_t', 'struct callcache *'
          '0'
        else
          super
        end
      end

      def build_dumper(name)
        case @type
        when 'struct builtin_func *'
          "        fprintf(fp, \"bf:%s\", n->u.#{name}.#{self.name}->name);"
        when 'builtin_func_ptr'
          "        fprintf(fp, \"<ptr>\");"
        when 'state_serial_t'
          "        fprintf(fp, \"<serial>\");"
        when 'struct callcache *'
          "        fprintf(fp, \"<cc>\");"
        else
          super
        end
      end

      def build_specializer(name)
        case @type
        when 'struct builtin_func *'
          arg = "    fprintf(fp, \"        %s\", \"n->u.#{name}.#{self.name}\");"
          return nil, arg
        when 'builtin_func_ptr'
          arg = <<~C
              if (n->u.#{name}.bf->have_src) {
                  fprintf(fp, "        (builtin_func_ptr)%s", n->u.#{name}.bf->func_name);
              }
              else {
                  fprintf(fp, "        (builtin_func_ptr)%s", "n->u.#{name}.#{self.name}");
              }
          C
          return nil, arg
        when 'state_serial_t', 'struct callcache *'
          arg = "    fprintf(fp, \"        n->u.#{name}.#{self.name}\");"
          return nil, arg
        else
          super
        end
      end
    end
  end
end
