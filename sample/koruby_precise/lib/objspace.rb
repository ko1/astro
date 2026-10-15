# objspace — the parts koruby can answer truthfully.  Allocation tracing is not
# implemented (the allocator has no hook, and a moving GC would need the side
# table keyed by identity), so the trace_* entry points raise NotImplementedError
# and the allocation_* queries answer nil, as CRuby does for an untraced object.
#
# reachable_objects_from / memsize_of read the object's own GC edges (C: __objspace_reachable /
# __objspace_memsize); dump / dump_all write CRuby's JSON shape for the fields koruby has.

module ObjectSpace
  module_function

  def trace_object_allocations_start
    raise NotImplementedError, "ObjectSpace.trace_object_allocations_start() function is unimplemented on this machine"
  end

  def trace_object_allocations_stop
    raise NotImplementedError, "ObjectSpace.trace_object_allocations_stop() function is unimplemented on this machine"
  end

  def trace_object_allocations_clear
    raise NotImplementedError, "ObjectSpace.trace_object_allocations_clear() function is unimplemented on this machine"
  end

  def trace_object_allocations
    raise NotImplementedError, "ObjectSpace.trace_object_allocations() function is unimplemented on this machine"
  end

  def allocation_sourcefile(obj) = nil
  def allocation_sourceline(obj) = nil
  def allocation_class_path(obj) = nil
  def allocation_method_id(obj) = nil
  def allocation_generation(obj) = nil

  def reachable_objects_from(obj) = __objspace_reachable(obj)
  def memsize_of(obj) = __objspace_memsize(obj)

  def memsize_of_all(klass = nil)
    raise TypeError, "class or module required" unless klass.nil? || klass.is_a?(Module)
    __heap_objects__(klass).sum { |o| __objspace_memsize(o) }
  end

  def dump(obj, output: :string)
    __dump_to(output, "rubyobj") { |io| io << __dump_json(obj) << "\n" }
  end

  def dump_all(output: :file, full: false, since: nil, shapes: true)
    __dump_to(output, "rubyheap") do |io|
      __heap_objects__(nil).each { |o| io << __dump_json(o) << "\n" }
    end
  end

  # output: :string (a String), :file / nil (a File in Dir.tmpdir, left open at its end), :stdout, or an IO
  def __dump_to(output, prefix)
    case output
    when :string
      s = +""
      yield s
      s
    when :file, nil
      require "tmpdir"
      f = File.open(File.join(Dir.tmpdir, "#{prefix}#{Process.pid}-#{rand(1 << 32).to_s(36)}.json"), "w+")
      yield f
      f
    when :stdout
      yield $stdout
      nil
    else
      raise ArgumentError, "wrong output option: #{output.inspect}" unless output.respond_to?(:write)
      yield output
      output
    end
  end

  DUMP_TYPES = { String => "STRING", Array => "ARRAY", Hash => "HASH", Float => "FLOAT", Integer => "BIGNUM",
                 Symbol => "SYMBOL", Range => "STRUCT", Regexp => "REGEXP", MatchData => "MATCH",
                 Rational => "RATIONAL", Complex => "COMPLEX", Proc => "DATA", Thread => "DATA" }.freeze

  def __dump_json(obj)
    return __json(obj.inspect) if obj.nil? || obj == true || obj == false || (obj.is_a?(Integer) && !__objspace_memsize(obj).positive?)
    type = case obj
           when Class then "CLASS"
           when Module then "MODULE"
           else DUMP_TYPES.find { |k, _| obj.is_a?(k) }&.last || "OBJECT"
           end
    f = { "address" => format("0x%x", obj.__id__), "type" => type }
    f["class"] = format("0x%x", obj.class.__id__) unless obj.is_a?(Module)
    case obj
    when String
      f["bytesize"] = obj.bytesize
      f["value"] = obj
      f["encoding"] = obj.encoding.name
    when Array, Hash
      f["length"] = obj.size
    when Module
      f["name"] = obj.name if obj.name
    when Float
      f["value"] = obj.to_s
    end
    f["memsize"] = __objspace_memsize(obj)
    "{" + f.map { |k, v| "#{__json(k)}:#{v.is_a?(Integer) ? v : __json(v.to_s)}" }.join(",") + "}"
  end

  def __json(s)
    s = s.to_s
    s = s.encode("UTF-8", invalid: :replace, undef: :replace) unless s.encoding == Encoding::UTF_8 && s.valid_encoding?
    '"' + s.gsub(/["\\\u0000-\u001f]/) { |ch|
      case ch
      when '"' then '\\"'
      when "\\" then "\\\\"
      when "\n" then "\\n"
      when "\t" then "\\t"
      when "\r" then "\\r"
      else format("\\u%04x", ch.ord)
      end
    } + '"'
  end
  private_class_method :__dump_to, :__dump_json, :__json
end
