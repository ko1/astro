require 'optparse'

module ASTroGen
  class NodeDef
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
          @type = type.sub(/\s*\brestrict\s*/, '')
          @name = name
        end

        def node?
          /NODE\s\*/ =~ @type
        end

        def eval_param
          if node?
            "#{@type} #{@name}, node_dispatcher_func_t #{@name}_dispatcher"
          else
            "#{@type} #{@name}"
          end
        end

        def join
          "#{@type} #{@name}"
        end

        def hash_call val
          case @type
          when 'uint32_t'
            "hash_uint32(#{val})"
          when 'int32_t'
            "hash_uint32((uint32_t)#{val})"
          when 'NODE *'
            "hash_node(#{val})"
          when 'const char *'
            "hash_cstr(#{val})"
          when 'struct builtin_func *'
            "hash_builtin_func(#{val})"
          when 'builtin_func_ptr'
            '0'
          when 'state_serial_t'
            '0'
          when 'struct callcache *'
            '0'
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
          when 'const char *'
            "        fprintf(fp, \"\\\"%s\\\"\", n->u.#{name}.#{self.name});"
          when 'struct builtin_func *'
            "        fprintf(fp, \"bf:%s\", n->u.#{name}.#{self.name}->name);"
          when 'builtin_func_ptr'
            "        fprintf(fp, \"<ptr>\");"
          when 'state_serial_t'
            "        fprintf(fp, \"<serial>\");"
          when 'struct callcache *'
            "        fprintf(fp, \"<cc>\");"
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
          when 'const char *'
            "    fprintf(fp, \"        \\\"%s\\\"\", n->u.#{name}.#{self.name});"
          when 'struct builtin_func *'
            "    fprintf(fp, \"        %s\", \"n->u.#{name}.#{self.name}\");"
          when 'builtin_func_ptr'
            <<~C
                if (n->u.#{name}.bf->have_src) {
                    fprintf(fp, "        (builtin_func_ptr)%s", n->u.#{name}.bf->func_name);
                }
                else {
                    fprintf(fp, "        (builtin_func_ptr)%s", "n->u.#{name}.#{self.name}");
                }
            C
          when 'state_serial_t'
            "    fprintf(fp, \"        n->u.#{name}.#{self.name}\");"
          when 'struct callcache *'
            "    fprintf(fp, \"        n->u.#{name}.#{self.name}\");"
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
          when /(.+)\s+([a-zA-Z_][a-zA-Z0-9_]*)$/
            Operand.new $1, $2
          when /(.+\*)([a-zA-Z_][a-zA-Z0-9_]*)$/
            Operand.new $1, $2
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

      def build_eval_body
        operands = @operands.map{it.eval_param}

        <<~C.chomp
        static VALUE
        EVAL_#{@name}(#{@prefix_args.join(', ')}, #{operands.join(", ")})
        {
        #{@body}}
        C
      end

      def build_head_struct
        <<~C
        struct #{name}_struct {
        #{@operands.map{ "    #{it.join};\n"}.join}};
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
        "NODE *ALLOC_#{name}(#{@operands.map{it.join}.join(', ')});"
      end

      def no_inline?
        @option.include? '@noinline'
      end

      def build_allocator
        sname = "#{@name}_struct"
        <<~C
        NODE *
        ALLOC_#{name}(#{@operands.map{it.join}.join(', ')}) {
            NODE *_n = node_allocate(sizeof(struct NodeHead) + sizeof(struct #{sname}));
            _n->head.dispatcher = DISPATCH_#{@name};
            _n->head.dispatcher_name = "DISPATCH_#{@name}";
            _n->head.kind = &kind_#{@name};
            _n->head.parent = NULL;
            _n->head.jit_status = JIT_STATUS_Unknown;
            _n->head.dispatch_cnt = 0;
            _n->head.flags.has_hash_value = false;
            _n->head.flags.is_specialized = false;
            _n->head.flags.is_specializing = false;
            _n->head.flags.is_dumping = false;
            _n->head.flags.no_inline = #{no_inline? ? true : false};
        #{@operands.map{"    _n->u.#{name}.#{it.name} = #{it.name};"}.join("\n")}
        #{@operands.map{"    if (_n->u.#{name}.#{it.name}) {_n->u.#{name}.#{it.name}->head.parent = _n;}" if it.node?}.join("\n")}
            OPTIMIZE(_n);
            if (OPTION.record_all) code_repo_add(NULL, _n, false);
            return _n;
        }
        C
      end

      def build_eval_dispatch
        <<~C
        static VALUE
        DISPATCH_#{@name}(#{@prefix_args.join(', ')})
        {
            dispatch_info(c, n, 0);
            VALUE v = EVAL_#{name}(c, n, #{
              @operands.map{
                arg = +"n->u.#{name}.#{it.name}"
                arg << ", n->u.#{name}.#{it.name}->head.dispatcher" if it.node?
                arg
              }.join(", ")
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
          "    if (#{field_name}) { fprintf(fp, \"static inline VALUE %s(CTX *c, NODE *n);\\n\", #{field_name}->head.dispatcher_name); }"
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
            fprintf(fp, "VALUE\\n");
            fprintf(fp, "%s(#{@prefix_args.join(', ')})\\n", dispatcher_name);
            fprintf(fp, "{\\n");
            fprintf(fp, "    dispatch_info(c, n, false);\\n");
            fprintf(fp, "    VALUE v = EVAL_#{name}(c, n, \\n");
        #{ args.join("\n    fprintf(fp, \",\\n\");\n")
        }
            fprintf(fp, "\\n    );\\n");
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

      def build_dumper
        op_dumpers = @operands.map do
          it.build_dumper @name
        end

        <<~C
        static void
        DUMP_#{@name}(FILE *fp, NODE *n, bool oneline)
        {
            if (oneline) {
                fprintf(fp, "(#{@name} ");
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
        Node.new($1, $2, option)
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

    def build_alloc
      allocators = <<~C__
      // This file is auto-generated from #{@file}.
      // kinds
      #{
        @nodes.map{|name, n|
          <<~STRUCT
          static const struct NodeKind kind_#{name} = {
              .default_dispatcher_name = "DISPATCH_#{name}",
              .default_dispatcher = DISPATCH_#{name},
              .hash_func = HASH_#{name},
              .specializer = SPECIALIZE_#{name},
              .dumper = DUMP_#{name},
          };
          STRUCT
        }.join("\n")
      }

      // allocators

      #{@nodes.map{|name, n| n.build_allocator}.join("\n")}
      C__
    end

    def build_head
      output = [<<~C]
      // This file is autogenerated from #{@file}.
      C
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
      # for exec
      %w(eval dispatch hash dump alloc specialize).each do
        File.write("#{@opt[:output_prefix]}_#{it}.c", send("build_#{it}"))
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

  def self.start argv
    opt = parse_opt(argv)

    nd = NodeDef.new(opt[:input], opt)
    nd.parse
    nd.gen
  end
end

if __FILE__ == $0
  system("make -C ../sample/naruby") || raise
end
