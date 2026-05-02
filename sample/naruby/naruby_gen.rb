require 'astrogen'

class NaRubyNodeDef < ASTroGen::NodeDef
  class Node < ASTroGen::NodeDef::Node
    # All dispatchers return `RESULT` (= VALUE + state bits) so `return`
    # propagates as a non-NORMAL state without setjmp.  Same as castro /
    # abruby; details in context.h.
    def result_type = "RESULT"

    # Three-arg dispatcher: `(CTX *c, NODE *n, VALUE *fp)`.  fp is the
    # current local-variable frame, register-passed.  This keeps the
    # frame in a register across the recursive call chain so node_lget /
    # node_lset don't bounce through `c->fp` memory traffic on every
    # access.  Castro pattern.
    def common_param_count
      3
    end

    # Override the framework's per-NODE-operand forward-decl emission.
    # The default writes `static inline RESULT <name>(...);` for every
    # NODE * operand, which is correct for ordinary children whose
    # specialized SD is emitted into the SAME .c file (so a static
    # inline forward decl matches the upcoming `static inline ... { ...
    # }` definition just below).
    #
    # naruby's `sp_body` operands (on `node_call2`, `node_pg_call_<N>`)
    # are special: they point to a function body that's compiled as its
    # OWN AOT entry — the body's SD lives in a different TU.  A `static
    # inline` forward decl for that cross-TU SD is wrong: gcc emits
    # "used but never defined" and the call doesn't link.  Normally we
    # need `extern RESULT` instead.
    #
    # The wrinkle: an `sp_body` NODE can hash-collide with a child
    # operand of the same call site (e.g. `def f9(n) = n` makes
    # `f9_body = node_lget(0)`, and the recursive call's `a0` is also
    # `node_lget(0)` — same HASH, same `SD_<hash>`).  In that case the
    # framework's static-inline emission for the child IS in this file
    # and an extern decl would conflict.  Defer the sp_body extern to
    # after all child SPECIALIZE walks have run, then check
    # `astro_spec_dedup_has` to know whether the symbol is being
    # emitted in-file.
    def build_specializer
      child_nodes = []
      args = []

      @operands.each do |op|
        n, arg = op.build_specializer(@name)
        child_nodes << n if n
        args << arg
      end

      # Standard decls for non-sp_body NODE * operands.
      decls = @operands.find_all{|op| op.node? && op.name != 'sp_body' }.map do
        field_name = "n->u.#{@name}.#{it.name}"
        "    if (#{field_name}) { fprintf(fp, \"static inline #{result_type} %s(#{@prefix_args.join(', ')});\\n\", #{field_name}->head.dispatcher_name); }"
      end

      # sp_body extern decl, gated on dedup to avoid colliding with an
      # in-file static-inline emission of the same hash.
      sp_op = @operands.find{|op| op.node? && op.name == 'sp_body' }
      if sp_op
        field_name = "n->u.#{@name}.#{sp_op.name}"
        decls << <<~C.chomp
              if (#{field_name}) {
                  node_hash_t _sp_h = hash_node(#{field_name});
                  if (!astro_spec_dedup_has(_sp_h)) {
                      fprintf(fp, "extern #{result_type} SD_%lx(#{@prefix_args.join(', ')});\\n",
                              (unsigned long)_sp_h);
                  }
              }
        C
      end

      if @option.include? '@noinline'
        return <<~C
        static void
        SPECIALIZE_#{@name}(FILE *fp, NODE *n, bool is_public)
        {
            /* do nothing */
        }
        C
      end

      <<~C
      static void
      SPECIALIZE_#{@name}(FILE *fp, NODE *n, bool is_public)
      {
      #{ child_nodes.join("\n")}
          const char *dispatcher_name = alloc_dispatcher_name(n); // SD_%lx % hash_node(n)
          n->head.dispatcher_name = dispatcher_name;

          // comment
          fprintf(fp, "// ");
          DUMP(fp, n, true);
          fprintf(fp, "\\n");

      #{ decls.join("\n") }

          if (!is_public) fprintf(fp, "static inline #{@option.include?('@always_inline') ? '__attribute__((always_inline)) ' : ''}");
          fprintf(fp, "__attribute__((no_stack_protector)) #{result_type}\\n");
          fprintf(fp, "%s(#{@prefix_args.join(', ')})\\n", dispatcher_name);
          fprintf(fp, "{\\n");
      #{
        if args.empty?
          '            fprintf(fp, "    return EVAL_' + @name + '(' + prefix_call_args.join(', ') + ');\\n");'
        else
          <<~INNER.chomp
                  fprintf(fp, "    return EVAL_#{@name}(#{prefix_call_args.join(', ')}, \\n");
              #{ args.join("\n    fprintf(fp, \",\\n\");\n")
              }
                  fprintf(fp, "\\n    );\\n");
          INNER
        end
      }
          fprintf(fp, "}\\n\\n");
      }
      C
    end

    class Operand < ASTroGen::NodeDef::Node::Operand
      def hash_call(val, kind: :horg)
        case @type
        when 'struct builtin_func *'
          "hash_builtin_func(#{val})"
        when 'builtin_func_ptr', 'state_serial_t', 'struct callcache *'
          '0'
        when 'uint32_t'
          # `locals_cnt` is callee-side metadata (callee's frame size),
          # patched after parse like sp_body.  Excluded from HASH so
          # call sites stay structurally equivalent regardless of the
          # callee's local count.
          if self.name == 'locals_cnt'
            '0'
          else
            super
          end
        when 'NODE *'
          # `sp_body` is a parse-time link to the function body (PG mode).
          # It's mutated at parse time (callsite_resolve) and again at
          # runtime if a method is redefined (node_call2_slowpath sets
          # `n->u.node_call2.sp_body = fe->body`).  Excluding it from the
          # structural hash keeps HASH stable across these mutations and
          # also breaks the recursion (`fib_body → call → sp_body=fib_body`)
          # that would otherwise blow up HASH().
          if self.name == 'sp_body'
            '0'
          else
            super
          end
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
          # @ref operands are stored inline in the NODE union; the
          # caller passes the address.  Without the &-prefix the SD
          # would silently get the struct's first word as the pointer
          # (= cc.serial as a callcache*), and every call would slowpath
          # against garbage.
          if ref?
            arg = "    fprintf(fp, \"        &n->u.#{name}.#{self.name}\");"
          else
            arg = "    fprintf(fp, \"        n->u.#{name}.#{self.name}\");"
          end
          return nil, arg
        when 'NODE *'
          # sp_body holds the linked function body for `node_call2`.  HASH
          # excludes sp_body so HASH(sp_body) is structurally stable
          # across runs (and against the no-op patch that callsite_resolve
          # writes at parse time).  We bake a direct call to
          # `SD_<HASH(sp_body)>` as a compile-time constant so the SD
          # makes a real direct call instead of loading
          # `sp_body->head.dispatcher` at runtime.
          #
          # Redefinition is handled at runtime by node_call2_slowpath
          # demoting `n->head.dispatcher` back to DISPATCH_node_call2
          # (the SD baked here would otherwise dispatch to the OLD
          # body's SD with the NEW body as argument — type-mismatched).
          # Demotion only kicks in on the second cache miss (cc->serial
          # was non-zero), so the first cold call still wires up cleanly.
          #
          # We don't recurse SPECIALIZE into sp_body — the function body
          # is its own AOT entry compiled separately by build_code_store
          # in main.c, so SD_<HASH(sp_body)> exists as a public symbol
          # in the same all.so.  We do, however, retag the body's
          # `head.dispatcher_name` here so the framework's decls loop
          # emits the matching `extern RESULT SD_<hash>(...)` forward
          # declaration in the same generated .c file.
          if self.name == 'sp_body'
            # No cn (no SPECIALIZE recursion into the body — it's a
            # separate AOT entry compiled by build_code_store).  The
            # extern decl is emitted by NaRubyNodeDef::Node#build_specializer
            # in the decls section, gated on `astro_spec_dedup_has` to
            # avoid colliding with a same-hash child operand.  Here we
            # only emit the call-site argument: the field reference and
            # the constant `SD_<hash>` baked from hash_node.
            #
            # IMPORTANT: do NOT mutate `n->u.<...>.sp_body->head.*`
            # here — body NODE state belongs to body's own AOT bake.
            arg = "    fprintf(fp, \"        n->u.#{name}.#{self.name},\\n\");\n" +
                  "    fprintf(fp, \"        SD_%lx\", (unsigned long)hash_node(n->u.#{name}.#{self.name}));"
            return nil, arg
          end
          super
        else
          super
        end
      end
    end
  end
end
