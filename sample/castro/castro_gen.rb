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

    # Use the 2-register RESULT struct (= VALUE + state) as the
    # dispatcher / EVAL return type.  Same trick abruby uses: framework
    # auto-generated DISPATCH / SD wrappers and EVAL_ARG come out
    # already typed RESULT, no astrogen.rb edits needed.
    def result_type
      'RESULT'
    end

    # Custom specializer for `node_call`.  Default framework would
    # emit an indirect call through `body->head.dispatcher`; we know
    # the callee at SPECIALIZE time (`func_idx → func_bodies[idx] →
    # hash`) so we emit
    #   `extern RESULT SD_<callee_hash>(...)`
    #   `RESULT v = SD_<callee_hash>(c, body, fp + arg);`
    # which the linker resolves intra-`.so` via `-Wl,-Bsymbolic` —
    # `addr32 call SD_<self>` for recursion (BTB-perfect), straight
    # extern call for cross-function.  The `body` is fetched from
    # `c->func_bodies[%u]` with the index baked as a uint32_t
    # immediate, which keeps the SD source deterministic across runs
    # (so ccache reuses the SOs) and keeps the runtime body load to a
    # single indexed access against `c` (a register parameter).
    def build_specializer
      return super if @name != 'node_call'
      castro_build_call_specializer
    end

    private

    def castro_build_call_specializer
      <<~C
      static void
      SPECIALIZE_#{@name}(FILE *fp, NODE *n, bool is_public)
      {
          uint32_t func_idx = n->u.#{@name}.func_idx;
          uint32_t arg_count = n->u.#{@name}.arg_count;
          uint32_t arg_index = n->u.#{@name}.arg_index;
          uint32_t local_cnt = n->u.#{@name}.local_cnt;
          extern NODE **castro_specialize_func_bodies;
          if (!castro_specialize_func_bodies) {
              fprintf(stderr, "SPECIALIZE_#{@name}: func_bodies snapshot not initialised\\n");
              exit(1);
          }
          NODE *callee = castro_specialize_func_bodies[func_idx];
          node_hash_t callee_hash = HASH(callee);

          const char *dispatcher_name = alloc_dispatcher_name(n);
          n->head.dispatcher_name = dispatcher_name;

          fprintf(fp, "// (#{@name} idx=%u arg=%u local=%u) -> SD_%lx\\n",
                  func_idx, arg_index, local_cnt, (unsigned long)callee_hash);
          fprintf(fp, "extern RESULT SD_%lx(CTX *restrict c, NODE *restrict n, VALUE *restrict fp);\\n",
                  (unsigned long)callee_hash);

          if (!is_public) fprintf(fp, "static inline ");
          fprintf(fp, "__attribute__((no_stack_protector)) RESULT\\n");
          fprintf(fp, "%s(CTX *restrict c, NODE *restrict n, VALUE *restrict fp)\\n{\\n",
                  dispatcher_name);
          fprintf(fp, "    dispatch_info(c, n, false);\\n");
          fprintf(fp, "    NODE *body = c->func_bodies[%u];\\n", func_idx);
          // Allocate the callee's frame as a stack VLA with a baked
          // literal size — gcc's SROA promotes its slots to registers
          // when the inlined call chain doesn't escape `&F`, which is
          // the whole point of this transform (see node.def
          // node_call's commentary).
          fprintf(fp, "    VALUE F[%u];\\n", local_cnt);
          for (uint32_t i = 0; i < arg_count; i++) {
              fprintf(fp, "    F[%u] = fp[%u];\\n", i, arg_index + i);
          }
          for (uint32_t i = arg_count; i < local_cnt; i++) {
              fprintf(fp, "    F[%u].i = 0;\\n", i);
          }
          // In valid C, BREAK / CONTINUE / GOTO are caught inside the
          // callee, RETURN is its normal exit — discard the callee's
          // state entirely so the caller's surrounding SD sees a
          // compile-time-constant `state==0` and DCEs the UNWRAP
          // branch.
          fprintf(fp, "    RESULT v = SD_%lx(c, body, F);\\n",
                  (unsigned long)callee_hash);
          fprintf(fp, "    dispatch_info(c, n, true);\\n");
          fprintf(fp, "    return RESULT_OK(v.value);\\n");
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
