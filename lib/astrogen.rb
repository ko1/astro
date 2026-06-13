require 'optparse'

module ASTroGen
  class NodeDef
    # Task registration: each task generates a node_<task>.c file
    # Options:
    #   func_typedef: C typedef for the function pointer (added to node_head.h)
    #   func_prefix:  prefix for per-node functions (e.g., "HASH_" → HASH_node_if)
    #   kind_field:   field declaration for NodeKind struct (e.g., "node_hash_func_t hash_func")
    GenTask = Struct.new(:name, :func_typedef, :func_prefix, :kind_field, :generate_file)

    def self.gen_tasks
      @gen_tasks ||= (superclass.respond_to?(:gen_tasks) ? superclass.gen_tasks.dup : [])
    end

    def self.register_gen_task(name, func_typedef: nil, func_prefix: nil, kind_field: nil, generate_file: true)
      gen_tasks.reject! { |t| t.name == name }
      gen_tasks << GenTask.new(name, func_typedef, func_prefix, kind_field, generate_file)
    end

    # Default tasks (framework-provided)
    register_gen_task :eval
    register_gen_task :dispatch
    register_gen_task :alloc
    register_gen_task :hash,
      func_typedef: "typedef node_hash_t (*node_hash_func_t)(struct Node *n);",
      func_prefix: "HASH_",
      kind_field: "node_hash_func_t hash_func"
    register_gen_task :specialize,
      func_typedef: "typedef void (*node_specializer_func_t)(FILE *fp, struct Node *n, bool is_public);",
      func_prefix: "SPECIALIZE_",
      kind_field: "node_specializer_func_t specializer"
    register_gen_task :dump,
      func_typedef: "typedef void (*node_dumper_func_t)(FILE *fp, struct Node *n, bool oneline);",
      func_prefix: "DUMP_",
      kind_field: "node_dumper_func_t dumper"
    register_gen_task :replace,
      func_typedef: "typedef void (*node_replacer_func_t)(struct Node *parent, struct Node *old_child, struct Node *new_child);",
      func_prefix: "REPLACER_",
      kind_field: "node_replacer_func_t replacer"
    register_gen_task :emit_ast,
      func_typedef: "typedef void (*node_emit_ast_func_t)(FILE *fp, struct Node *n);",
      func_prefix: "EMIT_AST_",
      kind_field: "node_emit_ast_func_t emit_ast"

    def initialize file, opt
      @file = file
      @opt = opt
      @verbose = opt[:verbose]
      info{ opt.inspect }

      @nodes = {}
    end

    class UnsupportedOperand < RuntimeError
    end

    class Node
      class Operand
        attr_reader :name

        def initialize type, name
          @ref = name.end_with?('@ref')
          name = name.sub(/@ref$/, '') if @ref
          # `@child` operand (v2 strict-arg mode):
          #   - body signature receives the operand as VALUE (pre-evaluated)
          #   - storage in the NODE struct is `NODE *` (= a child node)
          #   - DISPATCH wrapper evaluates the child via its dispatcher and
          #     spills the result to sp[i] before calling EVAL
          # Author writes e.g. `VALUE lv@child` and the body treats lv as
          # an already-computed VALUE.  This is opt-in; without @child the
          # existing `NODE *lhs` convention (lazy eval inside body via
          # EVAL_ARG) remains the default.
          @child = name.end_with?('@child')
          name = name.sub(/@child$/, '') if @child
          @type = type.sub(/\s*\brestrict\s*/, '')
          @name = name
        end

        def ref? = @ref
        def child? = @child

        # Storage type in the NODE struct.  For @child operands, the
        # author-written type is VALUE (body's view) but storage is NODE *.
        # All struct/alloc/hash/dump/specialize logic must use this.
        def storage_type
          child? ? 'NODE *' : @type
        end

        def node?
          # @child operands are NODE *-backed in storage, so structural
          # passes (hash, dump, replace) should treat them as nodes.
          # The body-side view (VALUE) is handled separately in eval_param.
          return true if child?
          !ref? && /NODE\s\*/ =~ @type
        end

        # "Storageless" operands are not stored in the NODE struct (no field,
        # no allocator parameter, no dump, no replace).  They appear only in
        # the EVAL/DISPATCH signature as a parameter, with DISPATCH providing
        # a default value and SPECIALIZE providing a baked (possibly PGO-
        # derived) value.  Subclasses override based on operand type.
        def storageless? = false

        # For storageless operands: C expression emitted by DISPATCH as the
        # argument value.  Subclasses override.
        def dispatch_default_expr
          raise "dispatch_default_expr not implemented for #{@type}"
        end

        def eval_param
          if ref?
            "#{@type} #{@name}"
          elsif child?
            # body receives the pre-evaluated VALUE; no dispatcher param
            "#{@type} #{@name}"
          elsif node?
            "#{@type} #{@name}, node_dispatcher_func_t #{@name}_dispatcher"
          else
            "#{@type} #{@name}"
          end
        end

        # Parameter type/name for the allocator and any storage-shape context.
        # @child uses storage_type (NODE *) since that's what AST passes in.
        def join
          "#{storage_type} #{@name}"
        end

        # For struct field: @ref strips the pointer — value is stored inline
        def struct_field_join
          if ref?
            "#{@type.sub(/\s*\*\s*$/, '')} #{@name}"
          else
            join
          end
        end

        # `kind` selects HORG (structural) vs HOPT (structural+profile).
        # HORG is the default; HOPT is opt-in for embedders that split the
        # two (e.g. abruby's two-hash PGC design).  The only structural
        # difference is how child NODE* operands are recursed: HORG uses
        # hash_node (cached Horg), HOPT uses hash_node_opt (cached Hopt).
        def hash_call val, kind: :horg
          # Use storage_type so @child operands (stored as NODE *) hash
          # via the node recursion rather than as a VALUE scalar.
          case storage_type
          when 'uint32_t'
            "hash_uint32(#{val})"
          when 'int32_t'
            "hash_uint32((uint32_t)#{val})"
          when 'uint64_t'
            "hash_uint64(#{val})"
          when 'NODE *'
            kind == :hopt ? "hash_node_opt(#{val})" : "hash_node(#{val})"
          when 'const char *'
            "hash_cstr(#{val})"
          when 'double'
            "hash_double(#{val})"
          when 'void *'
            # Opaque per-process pointer.  Hash to a constant so two
            # patterns with the same structural shape hash identically
            # — the operand value is determined by other operands /
            # build phase, not by the pointer itself.
            "0ULL"
          when 'VALUE'
            # Plain VALUE-typed operand (e.g. baked-immediate literal).
            # VALUE is intptr_t; hash as a 64-bit scalar.
            "hash_uint64((uint64_t)(uintptr_t)(#{val}))"
          else
            raise "no hash function: #{self.join}"
          end
        end

        # Emit the C statement(s) that, when run inside an EMIT_AST_<node>
        # function, write out this operand's value as a C expression
        # suitable for use as an argument to ALLOC_<node>.
        #
        # `node_name` is the parent NODE's @name (e.g. "node_add"), used to
        # locate the operand in n->u.<node_name>.<this.name>.
        #
        # Default handling covers scalar types (int/uint/double/VALUE),
        # const char *, NODE *.  Languages with custom operand types must
        # override in their Operand subclass.
        def build_emit_ast name
          return nil if ref? || storageless?
          field = "n->u.#{name}.#{self.name}"
          case storage_type
          when 'NODE *'
            # In "program" emission mode this calls a per-emit-context
            # helper that writes either `_n[id]` (DAG-aware reference)
            # or `NULL` for cycle back-edges.  In the simple recursive
            # mode (used by samples without node sharing), the same
            # helper recurses into astro_emit_ast_c.  The choice is made
            # at runtime by the caller of the top-level emit function.
            "    astro_emit_ast_c_child(fp, #{field});"
          when 'int32_t'
            "    fprintf(fp, \"%d\", #{field});"
          when 'uint32_t'
            "    fprintf(fp, \"%uU\", #{field});"
          when 'uint64_t'
            "    fprintf(fp, \"%lluULL\", (unsigned long long)#{field});"
          when 'intptr_t'
            "    fprintf(fp, \"(intptr_t)%lldLL\", (long long)#{field});"
          when 'uintptr_t'
            "    fprintf(fp, \"(uintptr_t)%lluULL\", (unsigned long long)#{field});"
          when 'double'
            "    fprintf(fp, \"%.17g\", #{field});"
          when 'const char *'
            "    astro_fprintf_cstr(fp, #{field});"
          when 'VALUE'
            "    fprintf(fp, \"(VALUE)0x%lxL\", (long)#{field});"
          when 'void *'
            # Opaque per-process pointer; cannot be embedded.  Emit a
            # NULL placeholder — node-specific code must rebuild the
            # pointer post-construction if it actually uses the slot.
            "    fprintf(fp, \"(void *)NULL\");"
          else
            raise UnsupportedOperand, "no emit_ast for #{self.join}"
          end
        end

        def build_dumper name
          return nil if storageless?
          case storage_type
          when 'NODE *'
            "        DUMP(fp, n->u.#{name}.#{self.name}, oneline);"
          when 'uint32_t'
            "        fprintf(fp, \"%u\", n->u.#{name}.#{self.name});"
          when 'int32_t'
            "        fprintf(fp, \"%d\", n->u.#{name}.#{self.name});"
          when 'uint64_t'
            "        fprintf(fp, \"%lluULL\", (unsigned long long)n->u.#{name}.#{self.name});"
          when 'const char *'
            # Escape the string so embedded '"', newlines, backslashes, etc.
            # don't break either the generated // comment header or the C
            # literal contexts that reproduce the AST as source text.
            "        astro_fprintf_cstr(fp, n->u.#{name}.#{self.name});"
          when 'double'
            "        fprintf(fp, \"%.17g\", n->u.#{name}.#{self.name});"
          when 'void *'
            # The pointer value isn't reproducible across runs and the
            # contents are too large to render usefully in textual
            # form — emit the operand's NAME as a placeholder
            # (`<ac>` for an `ac` operand, etc.) so the dump reads
            # naturally and the operand list stays well-formed.
            "        fputs(\"<#{self.name}>\", fp);"
          when 'VALUE'
            # Print as hex; VALUE could be a tagged int, singleton, or
            # heap pointer (latter not reproducible across runs).
            "        fprintf(fp, \"0x%lx\", (long)n->u.#{name}.#{self.name});"
          else
            raise "unknown operand type: #{self.join}"
          end
        end

        # `sp_slot` is set externally by Node#build_specializer before calling
        # `build_specializer` on @child operands.  It's the sp[] index this
        # operand was assigned among siblings.
        attr_accessor :sp_slot
        # Owner Node — set by parse_operands.  @child uses this to call back
        # into Node#child_storage_expr for the per-language storage choice.
        attr_accessor :owner

        def build_specializer name
          # @child operand: arg in the EVAL call is whatever the owner Node's
          # `child_storage_expr(slot)` returns (default: a C local).  The
          # spill statement is emitted by Node#build_specializer as a
          # "setup" statement BEFORE the return EVAL_xxx(...) call.  Here we
          # return cn (recurse SPECIALIZE into child) + the bare arg expr.
          if child?
            cn = "    SPECIALIZE(fp, n->u.#{name}.#{self.name});"
            arg = "    fprintf(fp, \"        #{@owner.child_storage_expr(@sp_slot)}\");"
            return cn, arg
          end
          arg = case storage_type
          when 'NODE *'
            cn = "    SPECIALIZE(fp, n->u.#{name}.#{self.name});"
            "    fprintf(fp, \"        n->u.#{name}.#{self.name},\\n\");\n" +
            "    fprintf(fp, \"        %s\", DISPATCHER_NAME(n->u.#{name}.#{self.name}));"
          when 'uint32_t'
            "    fprintf(fp, \"        %u\", n->u.#{name}.#{self.name});"
          when 'int32_t'
            "    fprintf(fp, \"        %d\", n->u.#{name}.#{self.name});"
          when 'uint64_t'
            "    fprintf(fp, \"        (VALUE)%lluULL\", (unsigned long long)n->u.#{name}.#{self.name});"
          when 'const char *'
            "    astro_fprint_cstr(fp, n->u.#{name}.#{self.name});"
          when 'double'
            "    fprintf(fp, \"        %.17g\", n->u.#{name}.#{self.name});"
          when 'void *'
            # Pointers can't be safely baked into a code-store SD —
            # the value is per-process.  Emit NULL; nodes that take
            # a void* operand should arrange for the pointer to be
            # resolved at runtime (or AOT for those nodes is a no-op).
            "    fprintf(fp, \"        (void *)NULL\");"
          when 'VALUE'
            # Bake VALUE as a hex literal cast.  Fine for tagged ints &
            # singletons; heap pointers are non-reproducible across runs
            # and shouldn't be baked anyway.
            "    fprintf(fp, \"        (VALUE)0x%lxL\", (long)n->u.#{name}.#{self.name});"
          else
            raise "unknown operand type: #{self.join}"
          end
          return cn, arg
        end
      end

      attr_reader :name

      def initialize name, fields_str, option
        @name = name
        parse_operands(fields_str)
        @option = option&.split(/\s+/) || []
      end

      # Embedder hooks to wrap each SPECIALIZE-generated SD_/PGSD_ body with
      # prologue (right after `{`, before dispatch_info) and epilogue (right
      # before `return v;`, after dispatch_info).  Return literal C source
      # (as a String, or nil for nothing).  Default empty.  Embedders use
      # this to cache stable fields at entry and assert invariance at exit —
      # e.g. abruby caches c->current_frame so clang's __builtin_assume can
      # help the compiler CSE reloads inside the evaluated tree.
      def specializer_prologue = nil
      def specializer_epilogue = nil

      # --------------------------------------------------------------------
      # @child storage hooks.  ASTroGen base guarantees the @child contract
      # ("body receives a pre-evaluated VALUE") but lets each language pick
      # WHERE to keep that value between the dispatcher call and the EVAL
      # call.
      #
      #   Default (language-neutral): a plain C local.  Correct for samples
      #   with no GC or a conservative GC (the C stack is scanned), and for
      #   any sample whose dispatcher has no scratch-area parameter.
      #
      #   Precise-GC samples MUST override: spill into a root-scanned slot
      #   area instead, so the value survives a sibling-eval GC.  See
      #   sample/baruby_precise/baruby_gen.rb for the "sp = top, negative
      #   offsets" convention used by the precise family.
      #
      # `child_storage_decl(slot)` is emitted ONCE at the top of the
      # DISPATCH / SD body (or empty if the storage doesn't need a decl).
      # `child_storage_expr(slot)` is used as the lvalue for the spill
      # assignment AND as the arg expression passed to EVAL.
      # --------------------------------------------------------------------
      def child_storage_decl(slot)
        "VALUE _c#{slot};"
      end

      def child_storage_expr(slot)
        "_c#{slot}"
      end

      def child_dispatch_args(slot, field)
        # Language-neutral dispatcher call: `(*dispatcher)(c, node)`.
        # Samples whose dispatchers take extra args (fp / sp / slots ...)
        # override this to append them.
        "c, #{field}"
      end

      # C statement emitted at the top of DISPATCH / SD bodies to claim
      # this NODE's slot area (or "" for none).  Languages that thread a
      # slot-area cursor (sp / slots) through their dispatchers override
      # this to advance it by slot_count — see baruby_precise's
      # "sp = top" convention.  Neutral default: nothing; samples whose
      # @child storage is C locals need no prologue.
      def slot_area_prologue
        ""
      end

      # Identifier of the slot-area cursor parameter that `$name` slot
      # references in NODE_DEF bodies substitute against (and that the
      # default child_storage_expr of slot-threading samples indexes).
      # Neutral default is "sp"; a sample whose dispatcher names the
      # cursor differently (e.g. koruby_precise v2's `slots`) overrides
      # this in its lang_gen.rb.
      def cursor_name
        "sp"
      end

      # Slot count for this NODE_DEF = @child operand count + max tmp slot
      # index discovered in the body.  Used to:
      #   1. Lay out slots in [@child..., $tmp...] order below sp
      #   2. Bake into NodeKind.slot_count so parent dispatches can advance
      #      sp by the right amount when invoking this NODE as a child
      #   3. Compute negative offsets in substitute_sp_slots / child_storage_expr
      def slot_count
        @slot_count ||= compute_slot_count
      end

      def compute_slot_count
        child_count = @operands.count(&:child?)
        tmp_names = []
        child_names = @operands.select(&:child?).map(&:name).to_set
        scan_body(@body || "") do |match|
          next unless match.start_with?('$')
          name = match[1..]
          next if child_names.include?(name)  # @child snapshot, not a tmp
          tmp_names << name unless tmp_names.include?(name)
        end
        child_count + tmp_names.size
      end

      # Canonical family name used in structural hashes.  Specialized variants
      # (e.g. node_fixnum_plus → node_plus, node_call1_ast → node_call1) opt
      # in via `NODE_DEF @canonical=BASE` in node.def.  Defaults to @name.
      def canonical_name
        opt = @option.find { |o| o.start_with?('@canonical=') }
        opt ? opt.sub(/^@canonical=/, '') : @name
      end

      # How many leading parameters in NODE_DEF are "common" (shared by
      # every node, threaded through the dispatcher protocol unchanged)
      # vs operands (per-node payload).  Default 2 covers `(CTX *c,
      # NODE *n)`.  Embedders can override to extend the dispatcher
      # signature with their own context — e.g. wastro returns 3 to
      # carry an explicit `union wastro_slot *frame` as the third
      # parameter, so every NODE_DEF in node.def writes it visibly.
      def common_param_count
        2
      end

      def parse_operands str
        # Operand-name suffix annotations accepted here:
        #   @ref    — pointer to caller slot (existing)
        #   @child  — v2 strict-arg style: storage NODE *, body sees VALUE
        suffix_re = /(?:@ref|@child)?/
        owner = self
        @operands = str.split(',').tap do
          @prefix_args = it.shift(common_param_count)
        end.map do
          case it.strip
          when /(.+)\s+([a-zA-Z_][a-zA-Z0-9_]*#{suffix_re.source})$/
            op = self.class::Operand.new $1, $2
          when /(.+\*)([a-zA-Z_][a-zA-Z0-9_]*#{suffix_re.source})$/
            op = self.class::Operand.new $1, $2
          else
            raise "ill-formed field: #{it}"
          end
          op.owner = owner   # back-ref so @child operands can ask the Node
                             # for child_storage_expr / child_storage_decl
          op
        end
      end

      def parse_body lines
        head = lines.shift.chomp
        raise "illformed body header: #{head.inspect}" unless head == "{"
        @body = +""
        loop do
          line = lines.shift.chomp

          break if line == "}"
          @body << line << "\n"
        end
      end

      # Substitute $foo references in @body to sp[N] expressions.
      # Slot allocation:
      #   @child operands get slots 0..K-1 (declaration order)
      #     accessed via $<child_name>, e.g. $lv, $rv for `lv@child, rv@child`
      #   Author-declared $<other_name> tmp slots get slots K..K+M-1
      #     (in order of first appearance in body)
      #
      # Result: substituted body string + total slot count (K + M).
      # Skips $foo occurrences inside C comments and string/char literals.
      def substitute_sp_slots(body)
        child_ops = @operands.select(&:child?)
        slot_map = {}
        child_ops.each_with_index { |op, i| slot_map[op.name] = i }
        k = child_ops.size

        # Pass 1: find all $<name> occurrences in body (skipping comments/strings)
        # and assign slot indices to new names not in @child.
        tmp_idx = k
        scan_body(body) do |match|
          if match.start_with?('$')
            name = match[1..]
            unless slot_map.key?(name)
              slot_map[name] = tmp_idx
              tmp_idx += 1
            end
          end
        end

        # Pass 2: substitute $<name> → sp[slot - total_slots] (= negative
        # offset from sp top, per new convention).  Skip non-slot text.
        total_slots = slot_map.size
        new_body = body.gsub(/
          (
            "(?:[^"\\]|\\.)*"          # string literal
            | '(?:[^'\\]|\\.)*'        # char literal
            | \/\/[^\n]*                # line comment
            | \/\*.*?\*\/               # block comment (multiline)
            | \$\w+                     # ← slot reference
          )
        /xm) do |m|
          if m.start_with?('$')
            name = m[1..]
            slot = slot_map[name]
            raise "unknown slot $#{name} in #{@name}" unless slot
            offset = slot - total_slots
            "#{cursor_name}[#{offset}]"
          else
            m
          end
        end

        [new_body, total_slots]
      end

      # Helper: walk body tokens (string lit, char lit, line/block comment,
      # $<name>), yielding each match.  Used by substitute_sp_slots to
      # discover slot names in pass 1.
      def scan_body(body)
        body.scan(/
          "(?:[^"\\]|\\.)*"
          | '(?:[^'\\]|\\.)*'
          | \/\/[^\n]*
          | \/\*.*?\*\/
          | \$\w+
        /xm) { |m| yield m }
      end

      def result_type = "VALUE"

      def alloc_dispatcher_expr
        "DISPATCH_#{@name}"
      end

      def comma_operands(ops)
        ops.empty? ? "" : ", #{ops.join(", ")}"
      end

      # Convert prefix-arg declarations ("CTX *c", "NODE *n", "void *frame")
      # to call-site names ("c", "n", "frame").  Used when emitting calls
      # from generated DISPATCH_/SD_ wrappers to the corresponding
      # EVAL_ function so that the extra hidden args propagate without
      # the codegen knowing their identifiers up front.
      def prefix_call_args
        @prefix_args.map{|s| s.strip.split(/\s+/).last.sub(/^\*+/, '') }
      end

      def build_eval_body
        operands = @operands.map{it.eval_param}
        # Substitute `$<name>` references in the body to `sp[N]`.  @child
        # operands get slots 0..K-1; author-declared `$<tmp>` get slots
        # K..K+M-1.  Existing NODE_DEFs without `$<name>` usage are
        # unchanged (substitute_sp_slots is a no-op for those).
        substituted_body, _slot_count = substitute_sp_slots(@body)

        # EVAL_<name> is always force-inlined into its single caller (the
        # DISPATCH or SD wrapper).  Without this, gcc occasionally gives
        # up on bigger EVAL bodies — node_loop's `for(;;) { ... 4 branches
        # ... }` is the canonical example — and emits `EVAL_xxx.isra.0`
        # out-of-line, breaking SROA at the boundary.  EVAL has exactly
        # one call site per SD so forcing inline never cascades.
        # The `@always_inline` option in node.def controls the SD wrapper
        # separately; see build_specializer.
        <<~C.chomp
        static inline __attribute__((always_inline)) #{result_type}
        EVAL_#{@name}(#{@prefix_args.join(', ')}#{comma_operands(operands)})
        {
        #{substituted_body}}
        C
      end

      def build_head_struct
        fields = @operands.reject(&:storageless?).map{ "    #{it.struct_field_join};\n"}.join

        fields = "    char _dummy;\n" if fields.empty?
        <<~C
        struct #{name}_struct {
        #{fields}};
        C
      end

      def build_hash_func
        # Structural hash (Horg):
        #   - Use canonical_name so swap_dispatcher family members share a hash
        #   - Skip storageless operands (profile-derived, not part of structure)
        <<~C
        static node_hash_t
        HASH_#{name}(NODE *n)
        {
            node_hash_t h = hash_cstr(#{canonical_name.dump});
        #{
          @operands.reject(&:storageless?).map{
            val = "n->u.#{@name}.#{it.name}"
            hash_call = it.hash_call(val, kind: :horg)
            "    h = hash_merge(h, #{hash_call})"
          }.join(";\n")};
            return h;
        }
        C
      rescue UnsupportedOperand
        "#define HASH_#{name} NULL"
      end

      def build_hopt_func
        # Profile-aware hash (Hopt):
        #   - Use the *actual* node name (specialized variants differ)
        #   - Include storageless operands so profile-derived fields (e.g.
        #     baked prologue identifier) contribute to the key
        #   - Recurse into children via hash_node_opt (HOPT) so profile info
        #     propagates bottom-up
        <<~C
        static node_hash_t
        HOPT_#{name}(NODE *n)
        {
            node_hash_t h = hash_cstr(#{@name.dump});
        #{
          @operands.map{
            val = it.storageless? ? "n" : "n->u.#{@name}.#{it.name}"
            hash_call = it.hash_call(val, kind: :hopt)
            "    h = hash_merge(h, #{hash_call})"
          }.join(";\n")};
            return h;
        }
        C
      rescue UnsupportedOperand
        "#define HOPT_#{name} NULL"
      end

      def build_allocator_decl
        alloc_ops = @operands.reject{|o| o.ref? || o.storageless?}
        params = alloc_ops.map{it.join}.join(', ')
        params = 'void' if params.empty?
        "NODE *ALLOC_#{name}(#{params});"
      end

      def no_inline?
        @option.include? '@noinline'
      end


      def build_allocator
        alloc_ops = @operands.reject{|o| o.ref? || o.storageless?}
        ref_ops = @operands.select(&:ref?)
        sname = "#{@name}_struct"
        <<~C
        NODE *
        ALLOC_#{name}(#{alloc_ops.empty? ? 'void' : alloc_ops.map{it.join}.join(', ')}) {
            NODE *_n = node_allocate(sizeof(struct NodeHead) + sizeof(struct #{sname}));
            _n->head.dispatcher = #{alloc_dispatcher_expr};
            _n->head.dispatcher_name = "DISPATCH_#{@name}";
            _n->head.kind = &kind_#{@name};
        #ifdef ASTRO_NODEHEAD_SLOT_COUNT
            _n->head.slot_count = #{slot_count};
        #endif
        #ifdef ASTRO_NODEHEAD_PARENT
            _n->head.parent = NULL;
        #endif
        #ifdef ASTRO_NODEHEAD_JIT_STATUS
            _n->head.jit_status = JIT_STATUS_Unknown;
        #endif
        #ifdef ASTRO_NODEHEAD_DISPATCH_CNT
            _n->head.dispatch_cnt = 0;
        #endif
            _n->head.flags.has_hash_value = false;
            _n->head.flags.is_specialized = false;
            _n->head.flags.is_specializing = false;
            _n->head.flags.is_dumping = false;
            _n->head.flags.no_inline = #{no_inline? ? true : false};
        #{alloc_ops.map{"    _n->u.#{name}.#{it.name} = #{it.name};"}.join("\n")}
        #ifdef ASTRO_NODEHEAD_PARENT
        #{alloc_ops.map{"    if (_n->u.#{name}.#{it.name}) {_n->u.#{name}.#{it.name}->head.parent = _n;}" if it.node?}.join("\n")}
        #endif
        #{ref_ops.map{"    memset(&_n->u.#{name}.#{it.name}, 0, sizeof(_n->u.#{name}.#{it.name}));"}.join("\n")}
            OPTIMIZE(_n);
            if (OPTION.record_all) code_repo_add(NULL, _n, false);
            return _n;
        }
        C
      end

      def build_eval_dispatch
        # @child operands need pre-evaluation in DISPATCH: each child is
        # dispatched first, its result spilled to sp[i], then passed as a
        # plain VALUE to EVAL.  This implements v2 strict-arg / ANF style
        # with auto-snapshot for deopt/GC safety.
        child_ops = @operands.select(&:child?)
        if child_ops.empty?
          # Backward-compatible fast path: no @child operands, emit the
          # forwarder DISPATCH.  Still need the slot-area prologue if this
          # NODE_DEF declared $tmp slots (M > 0).
          sp_advance = slot_area_prologue.empty? ? "" : "    #{slot_area_prologue}\n"
          <<~C
          static __attribute__((no_stack_protector)) #{result_type}
          DISPATCH_#{@name}(#{@prefix_args.join(', ')})
          {
          #{sp_advance}    return EVAL_#{name}(#{prefix_call_args.join(', ')}#{
                comma_operands(@operands.map{
                  if it.storageless?
                    it.dispatch_default_expr
                  elsif it.ref?
                    "&n->u.#{name}.#{it.name}"
                  else
                    arg = +"n->u.#{name}.#{it.name}"
                    arg << ", n->u.#{name}.#{it.name}->head.dispatcher" if it.node?
                    arg
                  end
                })
              });
          }
          C
        else
          # v2 strict-arg DISPATCH:
          #   1. Pre-evaluate each @child by calling its dispatcher.
          #   2. Spill the result to its storage slot (snapshot) before
          #      evaluating the next @child — for precise-GC samples the
          #      slot is root-scanned, protecting against GC moving the
          #      value during a sibling's evaluation.
          #   3. Pass the @child storage exprs as plain VALUE args to EVAL.
          # WHERE the snapshot lives is the per-language child_storage_*
          # hook choice (default: C local; precise-GC samples spill into
          # their scanned sp[] area — see baruby_precise).
          #
          # UNWRAP is the embedder's macro for extracting VALUE from the
          # dispatcher's return type (RESULT for baruby/castro, VALUE for
          # calc, etc.).  It must propagate non-NORMAL state via early
          # return from this DISPATCH function.
          #
          # The DISPATCH wrapper is NOT marked inline; gcc decides.  In
          # practice LTO inlines it into the parent's dispatcher.
          n_children = child_ops.size
          # Assign sp[i] indices to each @child in left-to-right order.
          child_slot = {}
          child_ops.each_with_index{|op, i| child_slot[op.name] = i }

          # Per-language storage decls (default: `VALUE _c<i>;` C locals;
          # empty for samples whose storage needs no decl, e.g. sp[] slots).
          decl_stmts = child_ops.map{|op|
            child_storage_decl(child_slot[op.name])
          }.reject(&:empty?).map{|s| "    #{s}" }.join("\n")

          # Pre-eval + spill statements.  The LHS is whatever the language
          # picks via child_storage_expr (default: C local).  The dispatcher
          # call args come from child_dispatch_args so languages with extra
          # dispatcher params (fp / sp) can append them.
          spill_stmts = child_ops.map{|op|
            slot = child_slot[op.name]
            field = "n->u.#{name}.#{op.name}"
            "    #{child_storage_expr(slot)} = UNWRAP((*#{field}->head.dispatcher)(#{child_dispatch_args(slot, field)}));"
          }.join("\n")

          # Body call args: @child uses child_storage_expr; others as before.
          body_args = comma_operands(@operands.map{
            if it.storageless?
              it.dispatch_default_expr
            elsif it.ref?
              "&n->u.#{name}.#{it.name}"
            elsif it.child?
              child_storage_expr(child_slot[it.name])
            else
              arg = +"n->u.#{name}.#{it.name}"
              arg << ", n->u.#{name}.#{it.name}->head.dispatcher" if it.node?
              arg
            end
          })

          # Slot-area prologue (e.g. cursor advance for sp-threading
          # samples; "" for the neutral C-local default).
          sp_advance = slot_area_prologue.empty? ? "" : "    #{slot_area_prologue}\n"
          <<~C
          static __attribute__((no_stack_protector)) #{result_type}
          DISPATCH_#{@name}(#{@prefix_args.join(', ')})
          {
          #{decl_stmts.empty? ? "" : decl_stmts + "\n"}#{sp_advance}#{spill_stmts}
              return EVAL_#{name}(#{prefix_call_args.join(', ')}#{body_args});
          }
          C
        end
      end

      def build_specializer
        # Assign sp slots to @child operands (left-to-right ordering).
        child_ops = @operands.select(&:child?)
        child_ops.each_with_index { |op, i| op.sp_slot = i }
        n_children = child_ops.size

        child_nodes = []
        args = []

        @operands.each do |op|
          n, arg = op.build_specializer(@name)
          child_nodes << n if n
          args << arg
        end

        # @child operands need pre-eval setup emitted into the generated
        # SD body BEFORE the `return EVAL_xxx(...)` call.  Each setup
        # statement direct-calls the child's specialized dispatcher (SD_<hash>)
        # — NOT through head.dispatcher pointer — so gcc can inline it at
        # AOT compile time.  DISPATCHER_NAME() resolves to the SD's symbol
        # (a static inline forward-declared just above this SD).
        #
        # The LHS of each `... = UNWRAP(...)` comes from child_storage_expr
        # so languages can swap precise-GC sp[] for C-local under libgc etc.
        # A separate child_storage_decl line (default empty) is emitted at
        # the top of the SD body for languages that need to declare locals.
        setup_decl_emitters = child_ops.filter_map do |op|
          d = child_storage_decl(op.sp_slot)
          next nil if d.empty?
          "    fprintf(fp, \"    #{d}\\n\");"
        end
        # iter 60: child-self-advance — `sp` to child is parent's top (= our
        # advanced sp).  Child's DISPATCH/SD prologue does the advance.
        setup_emitters = setup_decl_emitters + child_ops.map do |op|
          field = "n->u.#{@name}.#{op.name}"
          "    fprintf(fp, \"    #{child_storage_expr(op.sp_slot)} = UNWRAP(%s(#{child_dispatch_args(op.sp_slot, field)}));\\n\", DISPATCHER_NAME(#{field}));"
        end

        # Slot-area prologue at top of SD body (per-language hook).
        unless slot_area_prologue.empty?
          setup_emitters.unshift("    fprintf(fp, \"    #{slot_area_prologue}\\n\");")
        end

        # Pass sp unchanged.  Body sees @child snapshot at sp[0..N) and
        # uses sp[N..] as its own scratch (same convention as DISPATCH).
        eval_prefix_call_args = prefix_call_args

        decls = @operands.find_all{it.node?}.map do
          field_name = "n->u.#{@name}.#{it.name}"
          "    if (#{field_name}) { fprintf(fp, \"static inline #{result_type} %s(#{@prefix_args.join(', ')});\\n\", #{field_name}->head.dispatcher_name); }"
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

            // comment — gated by ASTRO_SD_COMMENTS env var.  On big
            // programs with many no_inline callees, the framework's
            // auto-DUMP commentary balloons the SD source size by orders
            // of magnitude (gcc still has to lex through it).  Default
            // off; set ASTRO_SD_COMMENTS=1 when debugging the SD chain.
            if (astro_emit_sd_comments_p()) {
                fprintf(fp, "// ");
                DUMP(fp, n, true);
                fprintf(fp, "\\n");
            }

        #{ decls.join("\n") }

            // Inlining policy:
            //   - is_public  (function body root reachable from outside —
            //                 e.g. astro_cs_compile entry): no inline hint;
            //                 the SD is a real callable boundary that
            //                 runtime dispatch can cross via function ptr.
            //   - !is_public + @always_inline option (set per-node in
            //                 node.def, e.g. on control-flow nodes that
            //                 must collapse into the function body so gcc
            //                 sees the full SROA chain): force-inline past
            //                 gcc's size budget.
            //   - !is_public default: `static inline` hint, gcc decides.
            //                 Aggressive force-inlining everywhere blows up
            //                 register pressure and icache (measured 4-7×
            //                 regressions on pi / sha256 / fannkuch), so we
            //                 ship gcc's heuristic by default and opt the
            //                 specific nodes that need it via @always_inline.
            if (!is_public) fprintf(fp, "static inline #{@option.include?('@always_inline') ? '__attribute__((always_inline)) ' : ''}");
            fprintf(fp, "__attribute__((no_stack_protector)) #{result_type}\\n");
            fprintf(fp, "%s(#{@prefix_args.join(', ')})\\n", dispatcher_name);
            fprintf(fp, "{\\n");
#{ specializer_prologue ? "            fprintf(fp, \"    #{specializer_prologue}\\n\");" : "" }
        #{ setup_emitters.join("\n        ") }
#{  # Direct `return EVAL_...(...)` — no named temp.  gcc fails to apply NRVO
    # through the deep static-inline SD chain, and a named `RESULT v` leaves
    # CLOBBER(eol) markers in inner loop bodies that block tree-ssa loop
    # deletion even after SCEV folds the closed form.  Direct return
    # collapses that — measured 30-77% AOT-cached speedups on castro's
    # tight-loop benchmarks.
    if args.empty?
      '            fprintf(fp, "    return EVAL_' + name + '(' + eval_prefix_call_args.join(', ') + ');\\n");'
    else
      <<~INNER.chomp
                fprintf(fp, "    return EVAL_#{name}(#{eval_prefix_call_args.join(', ')}, \\n");
            #{ args.join("\n    fprintf(fp, \",\\n\");\n")
            }
                fprintf(fp, "\\n    );\\n");
      INNER
    end
}
            fprintf(fp, "}\\n\\n");
        }
        C
      rescue UnsupportedOperand => e
        p e
        <<~C
        #define SPECIALIZE_#{@name}  NULL
        C
      end

      def build_replace
        node_ops = @operands.select(&:node?)
        if node_ops.empty?
          return "#define REPLACER_#{@name} NULL\n"
        end
        checks = node_ops.map do |op|
          "    if (parent->u.#{@name}.#{op.name} == old_child) parent->u.#{@name}.#{op.name} = new_child;"
        end
        <<~C
        static void
        REPLACER_#{@name}(NODE *parent, NODE *old_child, NODE *new_child)
        {
        #{checks.join("\n")}
        }
        C
      end

      # Emit a per-NODE_DEF function EMIT_AST_<name>(FILE *fp, NODE *n)
      # that writes a textual `ALLOC_<name>(...)` call reproducing the node.
      # Recursive NODE * operands recurse through astro_emit_ast_c.
      #
      # When an operand has no defined emit_ast (e.g. host-specific struct
      # pointers), the whole node falls back to a stub that errors out at
      # runtime — the host should override in its Operand subclass to
      # provide a sensible representation.
      def build_emit_ast
        alloc_ops = @operands.reject{|o| o.ref? || o.storageless?}
        begin
          op_emits = alloc_ops.map { it.build_emit_ast(@name) }
        rescue UnsupportedOperand => e
          return <<~C
          static void
          EMIT_AST_#{@name}(FILE *fp, NODE *n)
          {
              (void)n;
              fprintf(stderr, "astro_emit_ast: #{@name} has un-embeddable operand (#{e.message})\\n");
              fprintf(fp, "/* UNEMBEDDABLE #{@name} */");
          }
          C
        end

        if alloc_ops.empty?
          # ALLOC_<name>(void) → no args.
          body = "    fprintf(fp, \"ALLOC_#{@name}()\");"
        else
          parts = []
          parts << "    fprintf(fp, \"ALLOC_#{@name}(\");"
          op_emits.each_with_index do |emit, i|
            parts << "    fprintf(fp, \"#{i.zero? ? '' : ', '}\");" if i > 0
            parts << emit
          end
          parts << "    fprintf(fp, \")\");"
          body = parts.join("\n")
        end

        <<~C
        static void
        EMIT_AST_#{@name}(FILE *fp, NODE *n)
        {
            (void)n;
        #{body}
        }
        C
      end

      def build_dumper
        op_dumpers = @operands.filter_map do
          it.build_dumper @name
        end

        <<~C
        static void
        DUMP_#{@name}(FILE *fp, NODE *n, bool oneline)
        {
            if (oneline) {
                fprintf(fp, "(#{@name}#{op_dumpers.empty? ? "" : " "}");
          #{op_dumpers.join(";\n        fprintf(fp, \" \");\n");}
                fprintf(fp, ")");
          }
          else {
            // ...
          }
        }
        C
      end
    end

    def info
      return unless @verbose
      puts yield
    end

    def parse_def_head(lines, option)
      head = lines.shift
      if /^(.+)\((.+)\)$/ =~ head
        self.class::Node.new($1, $2, option)
      else
        raise "illformed node header: #{head}"
      end
    end

    def parse_def lines, option
      node = parse_def_head(lines, option)
      @nodes[node.name] = node
      node.parse_body(lines)
      node.build_eval_body
    end

    def parse
      lines = File.readlines(@opt[:input])
      output = []
      while line = lines.shift&.chomp
        case line
        when /^NODE_DEF(\s+(@.+))?$/
          option = $2
          output << parse_def(lines, option)
        else
          output << line
        end
      end
      @output = output
    end

    def build_eval
      eval_body = @output.join("\n")

      # When a language sets `common_param_count > 2`, each generated
      # EVAL_xxx receives extra named parameters in scope (e.g. wastro's
      # `union wastro_slot *frame`).  The EVAL_ARG macro takes only `c`
      # and `n` as its two literal args; everything past index 2 in the
      # prefix list is forwarded by name from the enclosing function's
      # scope.  When common_param_count == 2 (default), no extras and the
      # macro stays at the legacy 2-arg form.
      sample = @nodes.values.first
      # New "sp = top" convention: when sp is in the prefix args, EVAL_ARG
      # auto-advances by the child's slot_count so the child receives sp
      # at the top of ITS own slot area.  For samples without sp (3-arg,
      # libgc), no transformation.
      # iter 60: EVAL_ARG passes sp unchanged.  Each DISPATCH/SD function
      # internally advances sp by its own `slot_count` (baked literal) on
      # entry, so children always receive "parent's top" and compute their
      # own area inside.  This avoids runtime slot_count loads in EVAL_ARG
      # entirely.
      extra_call_args = sample ? sample.prefix_call_args.drop(2) : []
      extra_args_str = extra_call_args.empty? ? "" : ", " + extra_call_args.join(", ")

      <<~C
      // This file is auto-generated from #{@file}.

      // EVAL_ARG_CHECK is a per-call hook the embedder can override.  Default
      // is a no-op; define it before including this file (e.g. in node.c for
      // ABRUBY_DEBUG builds) to run diagnostics on every child dispatch.
      #ifndef EVAL_ARG_CHECK
      #define EVAL_ARG_CHECK(n) ((void)0)
      #endif
      #define EVAL_ARG(c, n) (EVAL_ARG_CHECK(n), (*n##_dispatcher)(c, n#{extra_args_str}))

      #{eval_body}
      C
    end

    def build_dispatch
      dispatchers = <<~C__
      // This file is auto-generated from #{@file}.
      // dispatchers

      #{@nodes.map{|name, n| n.build_eval_dispatch}.join("\n")}
      C__
    end

    def build_hash
      hash_functions = <<~C__
      // This file is auto-generated from #{@file}.
      // hash functions

      #{@nodes.map{|name, n| n.build_hash_func}.join("\n")}
      C__
    end

    def build_specialize
      specializers = <<~C__
      // This file is auto-generated from #{@file}.
      // specializers

      #{@nodes.map{|name, n| n.build_specializer}.join("\n")}
      C__
    end

    def build_dump
      dumpers = <<~C__
      // This file is auto-generated from #{@file}.
      // dumpers

      #{@nodes.map{|name, n| n.build_dumper}.join("\n")}
      C__
    end

    def build_replace
      <<~C__
      // This file is auto-generated from #{@file}.
      // replacer functions
      #{@nodes.map{|name, n| n.build_replace}.join("\n")}
      C__
    end

    # node_emit_ast.c: per-NODE_DEF emitter that writes C source for
    # reconstructing the node via ALLOC_<name>(...) calls.  Used by
    # `--generate-executable` to embed a parsed AST into a generated exe.
    #
    # The dispatcher `astro_emit_ast_c_child` is declared `__attribute__((weak))`
    # with a recursive-mode fallback definition so samples that don't
    # include the framework's astro_node.c still link.  Samples that DO
    # include astro_node.c (calc, naruby) get the runtime's stronger
    # definition, which switches into flat-DAG mode when a program-emit
    # context is active.
    def build_emit_ast
      <<~C__
      // This file is auto-generated from #{@file}.
      // AST → C source emitters (used by --generate-executable).

      // Local fallback child dispatcher.  astro_node.c #defines
      // ASTRO_EMIT_AST_C_CHILD_DEFINED before #include'ing this file,
      // which suppresses the local definition so the runtime's
      // stronger (DAG-aware) version is used instead.  Samples that
      // don't include astro_node.c get the static fallback below
      // (recursive mode only — fine for samples that never invoke
      // --generate-executable).
      #ifndef ASTRO_EMIT_AST_C_CHILD_DEFINED
      __attribute__((unused)) static void
      astro_emit_ast_c_child(FILE *fp, struct Node *child)
      {
          if (!child) { fprintf(fp, "NULL"); return; }
          if (child->head.kind && child->head.kind->emit_ast) {
              (*child->head.kind->emit_ast)(fp, child);
          } else {
              fprintf(fp, "NULL");
          }
      }
      #endif

      #{@nodes.map{|name, n| n.build_emit_ast}.join("\n")}
      C__
    end

    def build_alloc
      kind_tasks = self.class.gen_tasks.select(&:kind_field)
      allocators = <<~C__
      // This file is auto-generated from #{@file}.

      // kinds
      #{
        @nodes.map{|name, n|
          fields = [
            "    .default_dispatcher_name = \"DISPATCH_#{name}\",",
            "    .default_dispatcher = DISPATCH_#{name},",
            "    .slot_count = #{n.slot_count},",
          ]
          kind_tasks.each do |task|
            fields << "    .#{task.kind_field.split.last} = #{task.func_prefix}#{name},"
          end
          "const struct NodeKind kind_#{name} = {\n#{fields.join("\n")}\n};"
        }.join("\n\n")
      }

      // allocators

      #{@nodes.map{|name, n| n.build_allocator}.join("\n")}
      C__
    end

    def build_head
      kind_tasks = self.class.gen_tasks.select(&:kind_field)

      output = [<<~C]
      // This file is autogenerated from #{@file}.
      C

      # typedefs for function pointers
      typedefs = self.class.gen_tasks.filter_map(&:func_typedef).uniq
      output << typedefs.join("\n") + "\n" unless typedefs.empty?

      # NodeKind struct
      kind_fields = [
        "    const char *default_dispatcher_name;",
        "    node_dispatcher_func_t default_dispatcher;",
        # slot_count: how many sp slots this NODE_DEF uses (= K @children
        # + M tmp slots).  Parent advances sp by this when invoking the
        # NODE as @child, positioning the child's sp at the top of its
        # own slot area (per new "sp = top" convention).
        "    uint32_t slot_count;",
      ]
      kind_tasks.each { |t| kind_fields << "    #{t.kind_field};" }

      output << <<~C

      struct NodeKind {
      #{kind_fields.join("\n")}
      };
      C

      # Node structs
      @nodes.each{|name, n|
        output << n.build_head_struct
      }

      output << <<~C
      struct Node {
          struct NodeHead head;

          union {
      #{@nodes.map{|name, n| "        struct #{name}_struct #{name};"}.join("\n")}
          }u;
      };

      // allocators
      #{@nodes.map{|name, n| n.build_allocator_decl}.join(";\n")}
      C

      # ASTRO_SD_PROTO(N): one specialized-dispatcher function
      # declaration, with N substituted for the function's name.  Used
      # by the static SD table emitted during --generate-executable so
      # that the per-language signature does not need to be hardcoded
      # in the runtime.  Derived from the first NODE_DEF's result type
      # and prefix args — all dispatchers in a language share this
      # signature.
      sample = @nodes.values.first
      if sample
        sig = "#{sample.result_type} N(#{sample.instance_variable_get(:@prefix_args).join(', ')})"
        output << <<~C
        // Static SD prototype macro (used by astro_cs_emit_static_table).
        #ifndef ASTRO_SD_PROTO
        #define ASTRO_SD_PROTO(N) #{sig}
        #endif
        C
      end

      output.join("\n")
    end

    def gen
      self.class.gen_tasks.each do |task|
        next unless task.generate_file
        File.write("#{@opt[:output_prefix]}_#{task.name}.c", send("build_#{task.name}"))
      end

      File.write(@opt[:output_head], build_head)
    end
  end

  def self.parse_opt argv
    opt = {
      verbose: $VERBOSE,
      input: 'node.def',
      output_prefix: 'node',
      output_head: 'node_head.h',
      output_dir: Dir.pwd,
    }
    op = OptionParser.new
    op.on '--verbose' do
      opt[:verbose] = true
    end
    op.on '--input=[FILE]' do |input_file|
      opt[:input] = input_file
    end
    op.on '--output-dir=[DIR]' do |output_dir|
      opt[:output_dir] = output_file
    end
    op.on '--output-prefix=[FILE]' do |output_prefix|
      opt[:output_prefix] = output_prefix
    end
    op.on '--output-head=[FILE]' do |output_head|
      opt[:output_head] = output_head
    end
    op.parse!(argv)
    opt
  end

  def self.start argv, node_def_class: NodeDef
    opt = parse_opt(argv)

    nd = node_def_class.new(opt[:input], opt)
    nd.parse
    nd.gen
  end
end

if __FILE__ == $0
  system("make -C ../sample/naruby") || raise
end
