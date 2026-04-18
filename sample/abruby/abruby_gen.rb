require 'astrogen'

class AbRubyNodeDef < ASTroGen::NodeDef
  register_gen_task :mark,
    func_typedef: "typedef void (*node_marker_func_t)(struct Node *n);",
    func_prefix: "MARKER_",
    kind_field: "node_marker_func_t marker"

  # Hopt (profile-aware hash) — generates node_hopt.c with HOPT_<name>
  # functions and wires .hopt_func on each NodeKind.  Sits alongside the
  # base framework's :hash task (Horg, file node_hash.c, .hash_func field).
  register_gen_task :hopt,
    func_typedef: "typedef node_hash_t (*node_hash_func_t)(struct Node *n);",
    func_prefix: "HOPT_",
    kind_field: "node_hash_func_t hopt_func"

  # Profile presence check — generates PROFILE_<name>(NODE *n) → bool.
  # Returns true if this node or any descendant carries real runtime profile
  # (method_cache with a non-zero serial).  iabrb uses it to split entries
  # into "has profile → PGSD_" and "no profile → SD_" groups, distinguishing
  # real PGC observations from mere parse-time specialisation.
  register_gen_task :profile,
    func_typedef: "typedef bool (*node_profile_func_t)(struct Node *n);",
    func_prefix: "PROFILE_",
    kind_field: "node_profile_func_t profile_func"

  class Node < ASTroGen::NodeDef::Node
    class Operand < ASTroGen::NodeDef::Node::Operand
      # method_prologue_t operand is storageless: no NODE struct field, no
      # alloc parameter.  DISPATCH passes NULL (interpreter path).  SPECIALIZE
      # queries the runtime resolver (abruby_pgo_prologue_name) for a baked
      # literal name — NULL if the resolver refuses to bake (POLY, unfilled,
      # etc.).  HOPT contributes based on the baked literal so that PGO-baked
      # and unbaked specialized sites get distinct keys (HORG skips
      # storageless operands entirely).
      def storageless?
        @type == 'method_prologue_t' || super
      end

      def dispatch_default_expr
        return "NULL" if @type == 'method_prologue_t'
        super
      end

      def hash_call(val, kind: :horg)
        return "0" if ref?
        case @type
        when 'ID'
          "hash_cstr(rb_id2name(#{val}))"
        when 'method_prologue_t'
          # `val` is the NODE* (the storageless hash path passes n).  The
          # resolver returns a stable C-string identifier for the baked
          # prologue or "none" if we're not baking here.  Only contributes
          # to HOPT — HORG rejects storageless operands before reaching here.
          "hash_cstr(abruby_pgo_prologue_name_for(#{val}))"
        else
          super
        end
      end

      def build_dumper(name)
        return nil if ref?
        return nil if storageless?
        case @type
        when 'ID'
          "        fprintf(fp, \"%s\", rb_id2name(n->u.#{name}.#{self.name}));"
        else
          super
        end
      end

      def build_specializer(name)
        if ref?
          # pass runtime address (not specialized)
          arg = "    fprintf(fp, \"        &n->u.#{name}.#{self.name}\");"
          return nil, arg
        end
        case @type
        when 'ID'
          arg = "    fprintf(fp, \"        n->u.#{name}.#{self.name}\");"
          return nil, arg
        when 'method_prologue_t'
          # The resolver returns either a C identifier like
          # "prologue_ast_simple_1" (bakeable) or "none" (don't bake).
          # We emit the identifier, or "NULL" for the non-baked path —
          # matching the interpreter DISPATCH behavior.
          arg = <<~ARG.chomp
                {
                    const char *_pgo = abruby_pgo_prologue_name_for(n);
                    fprintf(fp, "        %s", strcmp(_pgo, "none") == 0 ? "NULL" : _pgo);
                }
          ARG
          return nil, arg
        else
          super
        end
      end
    end

    def result_type = "RESULT"

    # Wrap SD_/PGSD_ bodies with a frame-invariance assertion so clang's
    # __builtin_assume can help CSE c->current_frame reloads across opaque
    # cfunc calls inside the evaluated tree.  @noinline nodes (def / class /
    # module / block_literal) are whole method/class-body entries — they
    # themselves push or replace c->current_frame, so the assume would lie.
    # Skip them.
    def specializer_prologue
      return nil if no_inline?
      "struct abruby_frame *_sd_cached_frame = c->current_frame;"
    end

    def specializer_epilogue
      return nil if no_inline?
      "ABRUBY_ASSUME(c->current_frame == _sd_cached_frame);"
    end

    def alloc_dispatcher_expr
      if no_inline?
        "DISPATCH_#{@name}"
      else
        "(OPTION.compiled_only ? NULL : DISPATCH_#{@name})"
      end
    end

    def build_marker
      node_ops = @operands.reject(&:ref?).select(&:node?)
      marks = node_ops.map { |op| "    MARK(n->u.#{@name}.#{op.name});" }
      <<~C
      static void
      MARKER_#{@name}(NODE *n)
      {
      #{marks.join("\n")}
      }
      C
    end

    def build_profile
      node_ops = @operands.reject(&:ref?).select(&:node?)
      child_checks = node_ops.map do |op|
        field = "n->u.#{@name}.#{op.name}"
        # Child might be NULL for optional branches (else_node, etc.) and
        # for operand slots that some AST shapes leave unset.
        "    if (#{field} && #{field}->head.kind->profile_func(#{field})) return true;"
      end
      <<~C
      static bool
      PROFILE_#{@name}(NODE *n)
      {
          // runtime profile signals:
          //   - swap_dispatcher fired on this node (type-specialisation observed)
          //   - method_cache filled with a non-zero serial (call site seen)
          if (n->head.flags.kind_swapped) return true;
          const struct method_cache *_mc = abruby_pgo_mc_for(n);
          if (_mc && _mc->serial != 0) return true;
      #{child_checks.join("\n")}
          return false;
      }
      C
    end
  end

  def build_mark
    <<~C
    // This file is auto-generated from #{@file}.
    // GC mark functions

    #{@nodes.map{|name, n| n.build_marker}.join("\n")}
    C
  end

  def build_profile
    <<~C__
    // This file is auto-generated from #{@file}.
    // Profile presence check (true iff any descendant has runtime profile)

    #{@nodes.map{|name, n| n.build_profile}.join("\n")}
    C__
  end

  def build_hopt
    <<~C__
    // This file is auto-generated from #{@file}.
    // Hopt (structural + profile) hash functions

    #{@nodes.map{|name, n| n.build_hopt_func}.join("\n")}
    C__
  end
end
