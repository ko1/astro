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
          @type = type.sub(/\s*\brestrict\s*/, '')
          @name = name
        end

        def ref? = @ref

        def node?
          !ref? && /NODE\s\*/ =~ @type
        end

        def eval_param
          if ref?
            "#{@type} #{@name}"
          elsif node?
            "#{@type} #{@name}, node_dispatcher_func_t #{@name}_dispatcher"
          else
            "#{@type} #{@name}"
          end
        end

        def join
          "#{@type} #{@name}"
        end

        # For struct field: @ref strips the pointer — value is stored inline
        def struct_field_join
          if ref?
            "#{@type.sub(/\s*\*\s*$/, '')} #{@name}"
          else
            join
          end
        end

        def hash_call val
          case @type
          when 'uint32_t'
            "hash_uint32(#{val})"
          when 'int32_t'
            "hash_uint32((uint32_t)#{val})"
          when 'uint64_t'
            "hash_uint64(#{val})"
          when 'NODE *'
            "hash_node(#{val})"
          when 'const char *'
            "hash_cstr(#{val})"
          when 'double'
            "hash_double(#{val})"
          else
            raise "no hash function: #{self.join}"
          end
        end

        def build_dumper name
          case @type
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
          else
            raise "unknown operand type: #{self.join}"
          end
        end

        def build_specializer name
          arg = case @type
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

      def parse_operands str
        @operands = str.split(',').tap do
          @prefix_args = it.shift(2)
        end.map do
          case it.strip
          when /(.+)\s+([a-zA-Z_][a-zA-Z0-9_]*(?:@ref)?)$/
            self.class::Operand.new $1, $2
          when /(.+\*)([a-zA-Z_][a-zA-Z0-9_]*(?:@ref)?)$/
            self.class::Operand.new $1, $2
          else
            raise "ill-formed field: #{it}"
          end
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

      def result_type = "VALUE"

      def alloc_dispatcher_expr
        "DISPATCH_#{@name}"
      end

      def comma_operands(ops)
        ops.empty? ? "" : ", #{ops.join(", ")}"
      end

      def build_eval_body
        operands = @operands.map{it.eval_param}

        # Mark EVAL functions `inline` so gcc aggressively inlines them into
        # the SD_ wrappers generated by the specializer (each SD_ is a thin
        # `return EVAL_xxx(...)` that should collapse to the EVAL body in
        # place, enabling cross-node optimization within a specialized tree).
        <<~C.chomp
        static inline #{result_type}
        EVAL_#{@name}(#{@prefix_args.join(', ')}#{comma_operands(operands)})
        {
        #{@body}}
        C
      end

      def build_head_struct
        fields = @operands.map{ "    #{it.struct_field_join};\n"}.join

        fields = "    char _dummy;\n" if fields.empty?
        <<~C
        struct #{name}_struct {
        #{fields}};
        C
      end

      def build_hash_func
        <<~C
        static node_hash_t
        HASH_#{name}(NODE *n)
        {
            node_hash_t h = hash_cstr(#{@name.dump});
        #{
          @operands.map{
            hash_call = it.hash_call("n->u.#{@name}.#{it.name}")
            "    h = hash_merge(h, #{hash_call})"
          }.join(";\n")};
            return h;
        }
        C
      rescue UnsupportedOperand
        "#define HASH_#{name} NULL"
      end

      def build_allocator_decl
        alloc_ops = @operands.reject(&:ref?)
        params = alloc_ops.map{it.join}.join(', ')
        params = 'void' if params.empty?
        "NODE *ALLOC_#{name}(#{params});"
      end

      def no_inline?
        @option.include? '@noinline'
      end


      def build_allocator
        alloc_ops = @operands.reject(&:ref?)
        ref_ops = @operands.select(&:ref?)
        sname = "#{@name}_struct"
        <<~C
        NODE *
        ALLOC_#{name}(#{alloc_ops.empty? ? 'void' : alloc_ops.map{it.join}.join(', ')}) {
            NODE *_n = node_allocate(sizeof(struct NodeHead) + sizeof(struct #{sname}));
            _n->head.dispatcher = #{alloc_dispatcher_expr};
            _n->head.dispatcher_name = "DISPATCH_#{@name}";
            _n->head.kind = &kind_#{@name};
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
        <<~C
        static __attribute__((no_stack_protector)) #{result_type}
        DISPATCH_#{@name}(#{@prefix_args.join(', ')})
        {
            dispatch_info(c, n, 0);
            #{result_type} v = EVAL_#{name}(c, n#{
              comma_operands(@operands.map{
                if it.ref?
                  "&n->u.#{name}.#{it.name}"
                else
                  arg = +"n->u.#{name}.#{it.name}"
                  arg << ", n->u.#{name}.#{it.name}->head.dispatcher" if it.node?
                  arg
                end
              })
            });
            dispatch_info(c, n, 1);

            return v;
        }
        C
      end

      def build_specializer
        child_nodes = []
        args = []

        @operands.each do |op|
          n, arg = op.build_specializer(@name)
          child_nodes << n if n
          args << arg
        end

        decls = @operands.find_all{it.node?}.map do
          field_name = "n->u.#{@name}.#{it.name}"
          "    if (#{field_name}) { fprintf(fp, \"static inline #{result_type} %s(CTX *c, NODE *n);\\n\", #{field_name}->head.dispatcher_name); }"
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

            if (!is_public) fprintf(fp, "static ");
            fprintf(fp, "__attribute__((no_stack_protector)) #{result_type}\\n");
            fprintf(fp, "%s(#{@prefix_args.join(', ')})\\n", dispatcher_name);
            fprintf(fp, "{\\n");
            fprintf(fp, "    dispatch_info(c, n, false);\\n");
#{  if args.empty?
      '            fprintf(fp, "    ' + result_type + ' v = EVAL_' + name + '(c, n);\\n");'
    else
      <<~INNER.chomp
                  fprintf(fp, "    #{result_type} v = EVAL_#{name}(c, n, \\n");
              #{ args.join("\n    fprintf(fp, \",\\n\");\n")
              }
                  fprintf(fp, "\\n    );\\n");
      INNER
    end
}
            fprintf(fp, "    dispatch_info(c, n, true);\\n");
            fprintf(fp, "    return v;\\n");
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
      <<~C
      // This file is auto-generated from #{@file}.
      #define EVAL_ARG(c, n) (*n##_dispatcher)(c, n)

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

    def build_alloc
      allocators = <<~C__
      // This file is auto-generated from #{@file}.

      // kinds
      #{
        kind_tasks = self.class.gen_tasks.select(&:kind_field)
        @nodes.map{|name, n|
          fields = [
            "    .default_dispatcher_name = \"DISPATCH_#{name}\",",
            "    .default_dispatcher = DISPATCH_#{name},",
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
