require 'astrogen'

# Castro-specific override of the framework specializer for
# `uint64_t` operands.  The default emits `(VALUE)<lit>ULL`, which
# assumes VALUE is a scalar typedef (e.g. wastro's
# `typedef uint64_t VALUE`).  In castro VALUE is a tagged union, so
# the cast is invalid; the EVAL parameter is `uint64_t` anyway, so a
# plain numeric literal is the right thing.
class CastroNodeDef < ASTroGen::NodeDef
  class Node < ASTroGen::NodeDef::Node
    # Castro threads the active VALUE-slot frame pointer
    # (`VALUE * restrict fp`) through every dispatcher / EVAL as a
    # common parameter, the same trick wastro uses for its
    # `union wastro_slot * restrict frame`.  This keeps `fp` in a
    # register across the call chain, removing the `c->fp += / -=`
    # memory traffic at every node_call site (and the post-call
    # `mov 0x8(%rdi),%rcx` reload gcc otherwise has to emit because
    # it can't prove the call preserves CTX state).
    def common_param_count
      3
    end

    # Customised specializer for the two function-call node kinds.
    # Default framework behaviour bakes the call as
    #   `(*body->head.dispatcher)(c, body, fp + arg_index)`
    # which is an indirect call.  Castro knows the callee at
    # SD-generation time (`func_idx -> func_set[idx].body -> hash`),
    # so we instead emit
    #   `extern SD_<callee_hash>(CTX*, NODE*, VALUE*);`
    #   `SD_<callee_hash>(c, body, fp + arg_index);`
    # — a direct call resolved by the linker at dlopen.  Recursive
    # calls become `call SD_<self>` (BTB perfect) and cross-function
    # calls become straight extern calls (no `body->head.dispatcher`
    # load and no `mov rdx; call *rdx` indirection).
    def build_specializer
      return super if @name != 'node_call' && @name != 'node_call_jmp'
      castro_build_call_specializer
    end

    private

    def castro_build_call_specializer
      is_jmp = (@name == 'node_call_jmp')
      call_line = if is_jmp
        %(        fprintf(fp, "    VALUE v = castro_invoke_jmp(c, body, SD_%lx, fp + %u);\\n", (unsigned long)callee_hash, arg_index);)
      else
        %(        fprintf(fp, "    VALUE v = SD_%lx(c, body, fp + %u);\\n", (unsigned long)callee_hash, arg_index);)
      end

      <<~C
      static void
      SPECIALIZE_#{@name}(FILE *fp, NODE *n, bool is_public)
      {
          uint32_t func_idx = n->u.#{@name}.func_idx;
          uint32_t arg_index = n->u.#{@name}.arg_index;
          // Look up the callee body via the live func_set snapshot
          // (set by main.c just before astro_cs_compile runs).
          extern struct function_entry *castro_specialize_func_set;
          if (!castro_specialize_func_set) {
              fprintf(stderr, "SPECIALIZE_#{@name}: func_set snapshot not initialised\\n");
              exit(1);
          }
          NODE *callee = castro_specialize_func_set[func_idx].body;
          node_hash_t callee_hash = HASH(callee);

          const char *dispatcher_name = alloc_dispatcher_name(n);
          n->head.dispatcher_name = dispatcher_name;

          fprintf(fp, "// (#{@name} idx=%u arg=%u) -> SD_%lx\\n",
                  func_idx, arg_index, (unsigned long)callee_hash);
          fprintf(fp, "extern VALUE SD_%lx(CTX *restrict c, NODE *restrict n, VALUE *restrict fp);\\n",
                  (unsigned long)callee_hash);

          if (!is_public) fprintf(fp, "static inline ");
          fprintf(fp, "__attribute__((no_stack_protector)) VALUE\\n");
          fprintf(fp, "%s(CTX *restrict c, NODE *restrict n, VALUE *restrict fp)\\n{\\n",
                  dispatcher_name);
          fprintf(fp, "    dispatch_info(c, n, false);\\n");
          fprintf(fp, "    NODE *body = c->func_set[%u].body;\\n", func_idx);
      #{call_line}
          fprintf(fp, "    dispatch_info(c, n, true);\\n");
          fprintf(fp, "    return v;\\n");
          fprintf(fp, "}\\n\\n");
      }
      C
    end

    public

    class Operand < ASTroGen::NodeDef::Node::Operand
      def build_specializer(name)
        case @type
        when 'uint64_t'
          arg = "    fprintf(fp, \"        %lluULL\", (unsigned long long)n->u.#{name}.#{self.name});"
          return nil, arg
        else
          super
        end
      end
    end
  end
end
