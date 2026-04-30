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
      case @name
      when 'node_call' then castro_build_call_specializer
      when 'node_call_static' then castro_build_call_static_specializer
      else super
      end
    end

    private

    # SPECIALIZE override for `node_call` (recursive call form).  See
    # node.def for context; key reason for overriding the framework
    # default is to emit a direct `extern SD_<callee_hash>` call (so
    # the linker resolves intra-`.so` to a perfect `addr32 call` even
    # for self-recursive sites that the framework's `is_specializing`
    # cycle break would otherwise downgrade to runtime indirect).  The
    # stack VLA `F[%u]` matches the wastro-style frame allocation in
    # node.def's body — same intent, but with the `local_cnt` baked
    # as a literal integer so gcc treats `F` as a fixed-size array
    # (better SROA than a VLA whose size is propagated via parameter).
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
          // Allocate the callee's frame as a stack array with a baked
          // literal size — gcc's SROA promotes its slots to registers
          // when the inlined call chain doesn't escape `&F`, which is
          // the whole point of this transform (see node.def
          // node_call's commentary).
          fprintf(fp, "    VALUE F[%u];\\n", local_cnt);
          // Argument copy uses scalar `.i` field assignment rather
          // than aggregate `F[i] = fp[j]`.  Both move the same 8
          // bytes (VALUE is a union of identically-sized members
          // that share storage), but SRA promotes the slot to a
          // scalar only when ALL writes are scalar-typed — an
          // aggregate write keeps the slot in `MEM[union VALUE *]`
          // form and SCEV later bails on `evolution of base is not
          // affine` because it can't prove the loop-invariant slots
          // (n / j in the matmul k-loop) stay constant across
          // iterations.  With scalar `.i` writes here and scalar
          // `.i` reads in EVAL_node_lget, F[0..arg_count-1] become
          // SSA scalars, gcc hoists them out of the inner loop, and
          // SR replaces `imul k,n` with pointer-stride
          // accumulation.  matmul k-loop drops from 10 to 4
          // instructions; identical bit transfer for double / void*
          // args because of union storage.
          for (uint32_t i = 0; i < arg_count; i++) {
              fprintf(fp, "    F[%u].i = fp[%u].i;\\n", i, arg_index + i);
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

    # SPECIALIZE override for `node_call_static` (non-recursive call).
    # Framework default would route through the auto-generated
    # `EVAL_node_call_static` whose VLA is sized by the parameter
    # `local_cnt` — even after inline propagation gcc keeps it as a
    # VLA and that limits SROA.  Bake the size as a literal here so
    # `F` becomes a fixed-size array: gcc fully partial-SROA's its
    # slots, and loop induction variables that live in `F[i]` get
    # promoted to registers across iterations rather than spilling on
    # every step.  Mandelbrot / sieve-style inner loops benefit.
    def castro_build_call_static_specializer
      <<~C
      static void
      SPECIALIZE_#{@name}(FILE *fp, NODE *n, bool is_public)
      {
          uint32_t arg_count = n->u.#{@name}.arg_count;
          uint32_t arg_index = n->u.#{@name}.arg_index;
          uint32_t local_cnt = n->u.#{@name}.local_cnt;
          NODE *callee = n->u.#{@name}.callee;
          if (!callee) {
              fprintf(stderr, "SPECIALIZE_#{@name}: callee not patched\\n");
              exit(1);
          }
          // Specialize the callee body before naming this SD's
          // dispatcher — that way the recursive walk has a chance to
          // emit the callee's chain as static-inline siblings, and
          // we can refer to its root SD by name.
          SPECIALIZE(fp, callee);
          const char *dispatcher_name = alloc_dispatcher_name(n);
          n->head.dispatcher_name = dispatcher_name;
          const char *callee_disp = DISPATCHER_NAME(callee);

          fprintf(fp, "// (#{@name} arg=%u local=%u)\\n",
                  arg_index, local_cnt);
          if (callee->head.flags.no_inline) {
              fprintf(fp, "extern RESULT %s(CTX *restrict c, NODE *restrict n, VALUE *restrict fp);\\n",
                      callee_disp);
          } else {
              fprintf(fp, "static inline RESULT %s(CTX *restrict c, NODE *restrict n, VALUE *restrict fp);\\n",
                      callee_disp);
          }

          if (!is_public) fprintf(fp, "static inline ");
          fprintf(fp, "__attribute__((no_stack_protector)) RESULT\\n");
          fprintf(fp, "%s(CTX *restrict c, NODE *restrict n, VALUE *restrict fp)\\n{\\n",
                  dispatcher_name);
          fprintf(fp, "    dispatch_info(c, n, false);\\n");
          fprintf(fp, "    VALUE F[%u];\\n", local_cnt);
          // Same SRA-friendly scalar `.i` arg copy as in
          // castro_build_call_specializer above — keeps F slots
          // promotable so loop-invariant args reach SCEV / IVOPTS.
          for (uint32_t i = 0; i < arg_count; i++) {
              fprintf(fp, "    F[%u].i = fp[%u].i;\\n", i, arg_index + i);
          }
          for (uint32_t i = arg_count; i < local_cnt; i++) {
              fprintf(fp, "    F[%u].i = 0;\\n", i);
          }
          fprintf(fp, "    RESULT v = %s(c, n->u.#{@name}.callee, F);\\n",
                  callee_disp);
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
