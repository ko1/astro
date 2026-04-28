require 'astrogen'

# Wastro overrides ASTroGen's default `VALUE` return type for node
# evaluators to a small `{ VALUE value; uint32_t br_depth; }` struct
# that fits in two registers (rax+rdx on SysV x86_64).  Branch state
# (`br` / `return` / `unreachable`) is carried in the return register
# pair instead of a CTX field, eliminating per-iteration memory traffic
# in tight loops.  See context.h for the RESULT struct definition and
# the UNWRAP macro that propagates a non-normal br_depth.
class WastroNodeDef < ASTroGen::NodeDef
  class Node < ASTroGen::NodeDef::Node
    def result_type = "RESULT"

    # Wastro lifts wasm locals out of the operand stack into a caller-
    # allocated `struct wastro_frame_<funcid>` per call boundary, and
    # passes its address through the dispatcher chain so gcc can SROA
    # the locals into registers in the inner loop.  Every NODE_DEF body
    # gets `void *frame` in scope as a hidden parameter; lifted local
    # ops cast it to the per-function struct to access typed fields.
    # See node.def for the local-op nodes and node_call_N where the
    # frame is allocated.
    def extra_prefix_args
      ["void *frame"]
    end

    # ----- Phase 3: per-function typed-frame SD codegen -----
    #
    # For lifted local ops, node_call_N, and node_function_frame, the
    # default ASTroGen specializer (which just emits a wrapper that
    # forwards to EVAL_<name>) doesn't help gcc SROA — the operand
    # stack stays uint64_t-typed.  We override the specializer for
    # these nodes to emit C source that:
    #
    #   - declares per-function typed structs (`struct wastro_frame_N`
    #     with int32_t / double / etc. fields, one per wasm local)
    #   - in node_call_N: stack-allocates the callee's struct directly
    #     in the caller's SD, writes typed args into typed fields, and
    #     dispatches the bare body with `&F`
    #   - in node_local_get / set / tee: casts `frame` to the right
    #     struct type and accesses the named field
    #   - in node_function_frame (entry adapter): copies untyped args
    #     from `((VALUE *)frame)[i]` into the typed struct fields, then
    #     dispatches the bare body
    #
    # All other nodes use the standard ASTroGen specializer.

    def build_specializer
      case @name
      when 'node_local_get', 'node_local_set', 'node_local_tee'
        build_local_op_specializer
      when /^node_call_[0-4]$/
        build_call_specializer
      when 'node_function_frame'
        build_function_frame_specializer
      else
        super
      end
    end

    # node_local_get(uint32_t frame_id, uint32_t index)
    #   → ((struct wastro_frame_<fid> *)frame)->L<idx>
    # node_local_set(uint32_t frame_id, uint32_t index, NODE *expr)
    #   → ((struct wastro_frame_<fid> *)frame)->L<idx> = AS_<t>(expr)
    # node_local_tee(uint32_t frame_id, uint32_t index, NODE *expr)
    #   → tmp = AS_<t>(expr); store; return FROM_<t>(tmp)
    def build_local_op_specializer
      <<~C
      static void
      SPECIALIZE_#{@name}(FILE *fp, NODE *n, bool is_public)
      {
          extern void wastro_emit_frame_struct(FILE *, uint32_t);
          extern const char *wastro_local_ctype(uint32_t, uint32_t);
          extern const char *wastro_local_as_macro(uint32_t, uint32_t);
          extern const char *wastro_local_from_macro(uint32_t, uint32_t);

          uint32_t fid = n->u.#{@name}.frame_id;
          uint32_t idx = n->u.#{@name}.index;
          const char *from = wastro_local_from_macro(fid, idx);
          const char *as   = wastro_local_as_macro(fid, idx);
      #{'    NODE *expr = n->u.' + @name + '.expr;' if @name != 'node_local_get'}
      #{'    SPECIALIZE(fp, expr);' if @name != 'node_local_get'}

          wastro_emit_frame_struct(fp, fid);

          const char *dispatcher_name = alloc_dispatcher_name(n);
          n->head.dispatcher_name = dispatcher_name;

          fprintf(fp, "// ");
          DUMP(fp, n, true);
          fprintf(fp, "\\n");

      #{'    if (expr) { fprintf(fp, "static inline RESULT %s(CTX *c, NODE *n, void *frame);\\n", expr->head.dispatcher_name); }' if @name != 'node_local_get'}

          if (!is_public) fprintf(fp, "static inline ");
          fprintf(fp, "__attribute__((no_stack_protector)) RESULT\\n");
          fprintf(fp, "%s(CTX *c, NODE *n, void *frame)\\n", dispatcher_name);
          fprintf(fp, "{\\n");
          fprintf(fp, "    dispatch_info(c, n, false);\\n");
      #{specializer_inner}
          fprintf(fp, "    dispatch_info(c, n, true);\\n");
          fprintf(fp, "    return v;\\n");
          fprintf(fp, "}\\n\\n");
      }
      C
    end

    # The inner C statement that produces `RESULT v` for each local-op
    # variant.  Generated as a fragment to keep build_local_op_specializer
    # readable (the surrounding boilerplate is identical across get/set/tee).
    def specializer_inner
      case @name
      when 'node_local_get'
        # RESULT v = RESULT_OK(FROM_<t>(((struct wastro_frame_<fid> *)frame)->L<idx>));
        <<~CODE.chomp
              fprintf(fp, "    RESULT v = RESULT_OK(%s(((struct wastro_frame_%u *)frame)->L%u));\\n",
                      from, fid, idx);
        CODE
      when 'node_local_set'
        # ((struct wastro_frame_<fid> *)frame)->L<idx> = AS_<t>(UNWRAP(SD_<expr>(c, expr, frame)));
        # RESULT v = RESULT_OK(0);
        <<~CODE.chomp
              fprintf(fp, "    ((struct wastro_frame_%u *)frame)->L%u = %s(UNWRAP(%s(c, n->u.#{@name}.expr, frame)));\\n",
                      fid, idx, as, DISPATCHER_NAME(expr));
              fprintf(fp, "    RESULT v = RESULT_OK(0);\\n");
        CODE
      when 'node_local_tee'
        # tmp_t tmp = AS_<t>(UNWRAP(SD_<expr>(c, expr, frame)));
        # ((struct wastro_frame_<fid> *)frame)->L<idx> = tmp;
        # RESULT v = RESULT_OK(FROM_<t>(tmp));
        <<~CODE.chomp
              fprintf(fp, "    %s _tee_tmp = %s(UNWRAP(%s(c, n->u.#{@name}.expr, frame)));\\n",
                      wastro_local_ctype(fid, idx), as, DISPATCHER_NAME(expr));
              fprintf(fp, "    ((struct wastro_frame_%u *)frame)->L%u = _tee_tmp;\\n",
                      fid, idx);
              fprintf(fp, "    RESULT v = RESULT_OK(%s(_tee_tmp));\\n", from);
        CODE
      end
    end

    # node_call_N(uint32_t func_index, uint32_t local_cnt, NODE *a0..aN-1, NODE *body)
    #   AOT specializer emits:
    #     struct wastro_frame_<callee_id> F;
    #     F.L0 = AS_<t0>(UNWRAP(SD_arg0(c, a0, frame)));
    #     ...
    #     F.L<k> = 0;  // for k >= param_cnt (declared locals zero-init)
    #     RESULT r = SD_<body>(c, body, &F);
    #     return RESULT_OK(r.value);
    def build_call_specializer
      arity = @name.sub('node_call_', '').to_i
      arg_specialize = (0...arity).map { |i|
        "    SPECIALIZE(fp, n->u.#{@name}.a#{i});"
      }.join("\n")
      arg_decls = (0...arity).map { |i|
        '    if (n->u.' + @name + '.a' + i.to_s + ') { fprintf(fp, "static inline RESULT %s(CTX *c, NODE *n, void *frame);\\n", n->u.' + @name + '.a' + i.to_s + '->head.dispatcher_name); }'
      }.join("\n")
      arg_inits = (0...arity).map { |i|
        <<~CODE.chomp
              fprintf(fp, "    F.L%u = %s(UNWRAP(%s(c, n->u.#{@name}.a#{i}, frame)));\\n",
                      (uint32_t)#{i}, wastro_local_as_macro(fid, #{i}),
                      DISPATCHER_NAME(n->u.#{@name}.a#{i}));
        CODE
      }.join("\n")

      <<~C
      static void
      SPECIALIZE_#{@name}(FILE *fp, NODE *n, bool is_public)
      {
          extern void wastro_emit_frame_struct(FILE *, uint32_t);
          extern const char *wastro_local_ctype(uint32_t, uint32_t);
          extern const char *wastro_local_as_macro(uint32_t, uint32_t);

          uint32_t fid = n->u.#{@name}.func_index;
          uint32_t local_cnt = n->u.#{@name}.local_cnt;
      #{arg_specialize}
          SPECIALIZE(fp, n->u.#{@name}.body);

          wastro_emit_frame_struct(fp, fid);

          const char *dispatcher_name = alloc_dispatcher_name(n);
          n->head.dispatcher_name = dispatcher_name;

          fprintf(fp, "// ");
          DUMP(fp, n, true);
          fprintf(fp, "\\n");

      #{arg_decls}
          if (n->u.#{@name}.body) { fprintf(fp, "static inline RESULT %s(CTX *c, NODE *n, void *frame);\\n", n->u.#{@name}.body->head.dispatcher_name); }

          if (!is_public) fprintf(fp, "static inline ");
          fprintf(fp, "__attribute__((no_stack_protector)) RESULT\\n");
          fprintf(fp, "%s(CTX *c, NODE *n, void *frame)\\n", dispatcher_name);
          fprintf(fp, "{\\n");
          fprintf(fp, "    dispatch_info(c, n, false);\\n");

          // Allocate the callee's typed frame on the caller's SD stack.
          fprintf(fp, "    struct wastro_frame_%u F;\\n", fid);

          // Initialize param slots from arg evaluations (typed writes).
      #{arg_inits}

          // Zero-initialize the rest (declared locals).
          for (uint32_t k = #{arity}; k < local_cnt; k++) {
              fprintf(fp, "    F.L%u = 0;\\n", k);
          }

          // Dispatch the bare body with &F as the new frame.  Using the
          // body's static dispatcher name (DISPATCHER_NAME via runtime
          // read at cycle break) keeps the call inlinable.
          fprintf(fp, "    RESULT r = %s(c, n->u.#{@name}.body, &F);\\n",
                  DISPATCHER_NAME(n->u.#{@name}.body));
          fprintf(fp, "    RESULT v = RESULT_OK(r.value);\\n");
          fprintf(fp, "    dispatch_info(c, n, true);\\n");
          fprintf(fp, "    return v;\\n");
          fprintf(fp, "}\\n\\n");
      }
      C
    end

    # node_function_frame(uint32_t func_idx, NODE *body)
    #   AOT specializer emits an entry adapter:
    #     struct wastro_frame_<id> F;
    #     F.L0 = AS_<t>(((VALUE *)frame)[0]);   // copy untyped args in
    #     ...
    #     F.L<k> = 0;                            // zero declared locals
    #     RESULT r = SD_<body>(c, body, &F);
    #     return RESULT_OK(r.value);
    def build_function_frame_specializer
      <<~C
      static void
      SPECIALIZE_#{@name}(FILE *fp, NODE *n, bool is_public)
      {
          extern struct wastro_function WASTRO_FUNCS[];
          extern void wastro_emit_frame_struct(FILE *, uint32_t);
          extern const char *wastro_local_as_macro(uint32_t, uint32_t);

          uint32_t fid = n->u.#{@name}.func_idx;
          uint32_t param_cnt = WASTRO_FUNCS[fid].param_cnt;
          uint32_t local_cnt = WASTRO_FUNCS[fid].local_cnt;

          SPECIALIZE(fp, n->u.#{@name}.body);
          wastro_emit_frame_struct(fp, fid);

          const char *dispatcher_name = alloc_dispatcher_name(n);
          n->head.dispatcher_name = dispatcher_name;

          fprintf(fp, "// ");
          DUMP(fp, n, true);
          fprintf(fp, "\\n");

          if (n->u.#{@name}.body) {
              fprintf(fp, "static inline RESULT %s(CTX *c, NODE *n, void *frame);\\n",
                      n->u.#{@name}.body->head.dispatcher_name);
          }

          if (!is_public) fprintf(fp, "static inline ");
          fprintf(fp, "__attribute__((no_stack_protector)) RESULT\\n");
          fprintf(fp, "%s(CTX *c, NODE *n, void *frame)\\n", dispatcher_name);
          fprintf(fp, "{\\n");
          fprintf(fp, "    dispatch_info(c, n, false);\\n");

          // Allocate the typed frame.
          fprintf(fp, "    struct wastro_frame_%u F;\\n", fid);

          // Copy untyped VALUE[] args into typed fields.
          for (uint32_t i = 0; i < param_cnt; i++) {
              fprintf(fp, "    F.L%u = %s(((VALUE *)frame)[%u]);\\n",
                      i, wastro_local_as_macro(fid, i), i);
          }
          // Zero-init declared locals.
          for (uint32_t i = param_cnt; i < local_cnt; i++) {
              fprintf(fp, "    F.L%u = 0;\\n", i);
          }

          fprintf(fp, "    RESULT r = %s(c, n->u.#{@name}.body, &F);\\n",
                  DISPATCHER_NAME(n->u.#{@name}.body));
          fprintf(fp, "    RESULT v = RESULT_OK(r.value);\\n");
          fprintf(fp, "    dispatch_info(c, n, true);\\n");
          fprintf(fp, "    return v;\\n");
          fprintf(fp, "}\\n\\n");
      }
      C
    end
  end
end
