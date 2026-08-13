# IO::Buffer — byte-buffer emulation on top of a binary backing String.
# CRuby の IO::Buffer は malloc/mmap 領域だが、koruby では共有参照される
# String (@source) + オフセットで表現する:
#   - .for(string) / #slice は同じ String オブジェクトを参照するので
#     write-through が成立する (bytesplice/setbyte は in-place 変更)。
#   - free/transfer は @source を手放して NULL 状態にする。generation
#     (@gen) を bump し、slice 側は親の @gen スナップショットと照合して
#     valid? を答える (親メモリが解放された slice は invalid)。
#   - mmap 系 (.map) はファイル内容のコピーで代用する (flush はしない)。
class IO
  class Buffer
    include Comparable

    class AccessError < RuntimeError; end
    class LockedError < RuntimeError; end
    class AllocationError < RuntimeError; end
    class InvalidatedError < RuntimeError; end
    class MaskError < ArgumentError; end

    EXTERNAL = 1
    INTERNAL = 2
    MAPPED   = 4
    SHARED   = 8
    LOCKED   = 32
    PRIVATE  = 64
    READONLY = 128

    PAGE_SIZE    = 4096
    DEFAULT_SIZE = 65536

    # type → [byte size, pack format]  (大文字 = big-endian / network order)
    DATA_TYPES = {
      U8:  [1, 'C'],  S8:  [1, 'c'],
      U16: [2, 'S>'], u16: [2, 'S<'], S16: [2, 's>'], s16: [2, 's<'],
      U32: [4, 'L>'], u32: [4, 'L<'], S32: [4, 'l>'], s32: [4, 'l<'],
      U64: [8, 'Q>'], u64: [8, 'Q<'], S64: [8, 'q>'], s64: [8, 'q<'],
      F32: [4, 'g'],  f32: [4, 'e'],  F64: [8, 'G'],  f64: [8, 'E'],
    }.freeze

    def self.size_of(type)
      if type.is_a?(Array)
        total = 0
        type.each { |t| total += size_of(t) }
        return total
      end
      entry = DATA_TYPES[type]
      raise ArgumentError, "Invalid type name!" unless entry
      entry[0]
    end

    # 内部コンストラクタ (allocate 経由)。公開 API ではない。
    def __setup(source, offset, size, flags, slice, parent, addr = nil)
      @source = source          # backing String (nil = NULL buffer)
      @offset = offset          # base offset into @source
      @size = size
      @flags = flags
      @slice = slice            # inspect の SLICE 表示
      @parent = parent          # slice の親 Buffer (validity 追跡)
      @parent_gen = parent ? parent.__gen : nil
      @gen = 0                  # free/transfer で bump → 子 slice が invalid 化
      @addr = addr || (source ? ((object_id << 6) & 0xffff_ffff_ffff) | 0x7000_0000_0000 : 0)
      self
    end

    def __gen; @gen; end
    def __source; @source; end
    def __offset; @offset; end
    def __string_backed; @string_backed; end
    def __mark_string_backed; @string_backed = true; self; end

    def self.__wrap(source, offset, size, flags, slice, parent = nil, addr = nil)
      allocate.__setup(source, offset, size, flags, slice, parent, addr)
    end

    def initialize(size = DEFAULT_SIZE, flags = nil)
      raise TypeError, "not an Integer" unless size.is_a?(Integer)
      raise ArgumentError, "Size can't be negative!" if size < 0
      unless flags.nil?
        raise TypeError, "not an Integer" unless flags.is_a?(Integer)
        raise ArgumentError, "Flags can't be negative!" if flags < 0
      end
      if size == 0
        __setup(nil, 0, 0, 0, false, nil)
      else
        flags ||= (size < PAGE_SIZE ? INTERNAL : MAPPED)
        if (flags & (INTERNAL | MAPPED)) == 0     # メモリの出所が決まらない
          raise AllocationError, "Could not allocate buffer!"
        end
        __setup("\0".b * size, 0, size, flags, false, nil)
      end
    end

    # dup/clone は writable な INTERNAL コピーに detach する (CRuby 同様)。
    def initialize_copy(other)
      data = other.null? ? nil : other.__source.byteslice(other.__offset, other.size)
      __setup(data, 0, other.size, data ? INTERNAL : 0, false, nil)
    end

    # block なし: 文字列内容の read-only なコピー (以後 String を書き換えても
    # buffer には反映されない)。block あり: 元 String を直接指す writable な
    # buffer を渡し、block を抜けたら free する。
    def self.for(string)
      raise TypeError, "expected a String" unless string.is_a?(String)
      unless block_given?
        # コピーだが「String がメモリを保持する」性質は同じ: free 後も
        # slice は有効なまま (CRuby と同じ扱い)。
        return __wrap(string.b, 0, string.bytesize, EXTERNAL | READONLY, true).__mark_string_backed
      end
      flags = EXTERNAL | (string.frozen? ? READONLY : 0)
      buffer = __wrap(string, 0, string.bytesize, flags, true).__mark_string_backed
      begin
        yield buffer
      ensure
        buffer.free
      end
    end

    # 一時的な writable external buffer を渡し、block 後にその内容を binary
    # String として返す (buffer 自体は free 済み)。
    def self.string(length)
      raise LocalJumpError, "no block given" unless block_given?
      raise TypeError, "not an Integer" unless length.is_a?(Integer)
      raise RangeError, "bignum too big to convert into `long'" if length > 0x7fff_ffff_ffff_ffff
      raise ArgumentError, "negative string size (or size too big)" if length < 0
      backing = "\0".b * length
      buffer = __wrap(backing, 0, length, EXTERNAL, true).__mark_string_backed
      begin
        yield buffer
      ensure
        buffer.free
      end
      backing
    end

    # mmap の代わりにファイル内容のコピーを持つ (書き戻しはしない)。可否判定
    # と例外は CRuby の io_buffer_map_file に合わせる。
    def self.map(file, size = nil, offset = 0, flags = 0)
      raise TypeError, "not an IO" unless file.respond_to?(:read)
      unless size.nil? || size.is_a?(Integer)
        raise TypeError, "not an Integer"
      end
      raise ArgumentError, "Size can't be negative!" if size && size < 0
      raise ArgumentError, "Size can't be zero!" if size == 0
      unless offset.is_a?(Integer)
        raise TypeError, "no implicit conversion of #{offset.class} into Integer"
      end
      raise ArgumentError, "Offset can't be negative!" if offset < 0
      total = file.size
      raise ArgumentError, "Invalid negative or zero file size!" if total <= 0
      raise ArgumentError, "Offset too large!" if offset + (size || 0) > total
      raise ArgumentError, "Size can't be larger than file size!" if size && size > total
      # PRIVATE (= copy-on-write) は読み取り専用ファイルでも許される。共有
      # マップだけが書き込み権限を要求する。
      if (flags & (READONLY | PRIVATE)) == 0 && !__writable_io?(file)
        raise Errno::EACCES, "io_buffer_map_file:mmap"
      end
      size ||= total - offset
      pos = file.pos
      file.seek(offset)
      data = (file.read(size) || "").b
      file.seek(pos)
      data << ("\0".b * (size - data.bytesize)) if data.bytesize < size
      # PRIVATE は copy-on-write の私有ページ: shared でも external でもない。
      base = (flags & PRIVATE) == 0 ? (EXTERNAL | SHARED) : 0
      __wrap(data, 0, size, base | MAPPED | flags, false)
    end

    def self.__writable_io?(file)
      return true unless file.respond_to?(:write)
      begin
        file.write("")     # 追記なしの書き込み可否テスト
        true
      rescue IOError, SystemCallError
        false
      end
    end
    private_class_method :__writable_io?

    def size; @size; end
    def null?; @source.nil?; end
    def empty?; @size == 0; end
    def external?; (@flags & EXTERNAL) != 0; end
    def internal?; (@flags & INTERNAL) != 0; end
    def mapped?; (@flags & MAPPED) != 0; end
    def shared?; (@flags & SHARED) != 0; end
    def locked?; (@flags & LOCKED) != 0; end
    def private?; (@flags & PRIVATE) != 0; end
    def readonly?; (@flags & READONLY) != 0; end

    def valid?
      return true unless @parent
      return true if @parent.__string_backed        # String がメモリを保持
      @parent.__gen == @parent_gen && @offset + @size <= @parent.size
    end

    # 親が free / transfer / 縮小された slice への操作は InvalidatedError。
    def __valid!
      raise InvalidatedError, "Buffer has been invalidated!" unless valid?
    end

    def locked
      raise LockedError, "Buffer already locked!" if locked?
      @flags |= LOCKED
      begin
        yield self
      ensure
        @flags &= ~LOCKED
      end
    end

    def free
      raise LockedError, "Buffer is locked!" if locked?
      @gen += 1
      @source = nil
      @offset = 0
      @size = 0
      @flags = 0
      @slice = false
      @addr = 0
      self
    end

    def transfer
      raise LockedError, "Cannot transfer ownership of locked buffer!" if locked?
      moved = self.class.__wrap(@source, @offset, @size, @flags, @slice, nil, @addr)
      moved.__mark_string_backed if @string_backed
      @gen += 1
      @source = nil
      @offset = 0
      @size = 0
      @slice = false
      @addr = 0
      moved
    end

    def resize(new_size)
      raise LockedError, "Cannot resize locked buffer!" if locked?
      raise TypeError, "not an Integer" unless new_size.is_a?(Integer)
      raise ArgumentError, "Size can't be negative!" if new_size < 0
      # 外部メモリ (String-backed / 共有マップ) は伸縮できない。free 済み
      # (NULL) なら普通のバッファとして作り直せる。
      if @source && (shared? || (external? && !private?))
        raise AccessError, "Cannot resize external buffer!"
      end
      old = @source ? @source.byteslice(@offset, [@size, new_size].min) : "".b
      if new_size == 0
        @source = nil
        @offset = 0
        @size = 0
      else
        data = old.b
        data << ("\0".b * (new_size - data.bytesize)) if data.bytesize < new_size
        @source = data
        @offset = 0
        @size = new_size
        @slice = false           # 実体を握り直したので slice ではなくなる
        if (@flags & (INTERNAL | MAPPED)) == 0     # NULL からの復帰: サイズで種別を決める
          @flags = (new_size < PAGE_SIZE ? INTERNAL : MAPPED)
        end
      end
      self
    end

    def slice(offset = 0, length = nil)
      raise ArgumentError, "Offset can't be negative!" if offset < 0
      length ||= @size - offset
      raise ArgumentError, "Length can't be negative!" if length < 0
      if offset + length > @size
        raise ArgumentError, "Specified offset+length is bigger than the buffer size!"
      end
      child = self.class.__wrap(@source, @offset + offset, length,
                                (@flags & ~(INTERNAL | LOCKED)), true, self)
      child.__mark_string_backed if @string_backed
      child
    end

    def get_value(type, offset)
      width, fmt = __type(type)
      __valid!
      __bounds(offset, width)
      @source.byteslice(@offset + offset, width).unpack1(fmt)
    end

    def get_values(types, offset)
      raise ArgumentError, "Argument types should be an array!" unless types.is_a?(Array)
      types.map do |t|
        v = get_value(t, offset)
        offset += self.class.size_of(t)
        v
      end
    end

    def set_value(type, offset, value)
      width, fmt = __type(type)
      __writable!
      __bounds(offset, width)
      @source.bytesplice(@offset + offset, width, [value].pack(fmt))
      offset + width
    end

    def set_values(types, offset, values)
      raise ArgumentError, "Argument types should be an array!" unless types.is_a?(Array)
      raise ArgumentError, "Argument values should be an array!" unless values.is_a?(Array)
      raise ArgumentError, "Argument types and values should have the same length!" unless types.size == values.size
      i = 0
      while i < types.size
        offset = set_value(types[i], offset, values[i])
        i += 1
      end
      offset
    end

    def each(type = :U8, offset = 0, count = nil)
      return enum_for(:each, type, offset, count) unless block_given?
      width = self.class.size_of(type)
      remaining = count || (@size - offset) / width
      while remaining > 0
        yield offset, get_value(type, offset)
        offset += width
        remaining -= 1
      end
      self
    end

    def values(type = :U8, offset = 0, count = nil)
      width = self.class.size_of(type)
      count ||= (@size - offset) / width
      result = []
      while count > 0
        result << get_value(type, offset)
        offset += width
        count -= 1
      end
      result
    end

    def each_byte(offset = 0, count = nil)
      return enum_for(:each_byte, offset, count) unless block_given?
      remaining = count || (@size - offset)
      while remaining > 0
        yield get_value(:U8, offset)
        offset += 1
        remaining -= 1
      end
      self
    end

    def get_string(offset = 0, length = nil, encoding = Encoding::BINARY)
      __valid!
      raise ArgumentError, "Offset can't be negative!" if offset < 0
      length ||= @size - offset
      raise ArgumentError, "Length can't be negative!" if length < 0
      if offset + length > @size
        raise ArgumentError, "Specified offset+length is bigger than the buffer size!"
      end
      s = @source ? @source.byteslice(@offset + offset, length) : ""
      s.force_encoding(encoding)
    end

    def set_string(string, offset = 0, length = nil, source_offset = 0)
      raise TypeError, "expected a String" unless string.is_a?(String)
      __writable!
      raise ArgumentError, "Offset can't be negative!" if offset < 0
      length ||= string.bytesize - source_offset
      if offset + length > @size
        raise ArgumentError, "Specified offset+length is bigger than the buffer size!"
      end
      @source.bytesplice(@offset + offset, length, string.byteslice(source_offset, length))
      length
    end

    def clear(value = 0, offset = 0, length = nil)
      __writable!
      length ||= @size - offset
      if offset < 0 || length < 0 || offset + length > @size
        raise ArgumentError, "Specified offset+length is bigger than the buffer size!"
      end
      @source.bytesplice(@offset + offset, length, (value & 0xff).chr.b * length) if length > 0
      self
    end

    def copy(source, offset = 0, length = nil, source_offset = 0)
      __writable!
      src = source.is_a?(String) ? source : source.get_string
      length ||= src.bytesize - source_offset
      set_string(src, offset, length, source_offset)
    end

    def <=>(other)
      unless other.is_a?(IO::Buffer)
        raise TypeError, "wrong argument type #{__type_name(other)} (expected IO::Buffer)"
      end
      shorter = @size < other.size ? @size : other.size
      i = 0
      while i < shorter
        d = get_value(:U8, i) - other.get_value(:U8, i)
        return d unless d == 0
        i += 1
      end
      @size <=> other.size
    end

    def ==(other)
      unless other.is_a?(IO::Buffer)
        raise TypeError, "wrong argument type #{__type_name(other)} (expected IO::Buffer)"
      end
      @size == other.size && (@size == 0 || get_string == other.get_string)
    end

    # mask を繰り返し適用する elementwise 論理演算 (CRuby: mask is cycled)。
    def __mask_op(mask)
      unless mask.is_a?(IO::Buffer)
        raise TypeError, "wrong argument type #{__type_name(mask)} (expected IO::Buffer)"
      end
      raise MaskError, "Zero-length mask given!" if mask.size == 0
      result = self.class.new(@size == 0 ? 0 : @size)
      i = 0
      msize = mask.size
      while i < @size
        result.set_value(:U8, i, yield(get_value(:U8, i), mask.get_value(:U8, i % msize)))
        i += 1
      end
      result
    end

    def &(mask); __mask_op(mask) { |a, b| a & b }; end
    def |(mask); __mask_op(mask) { |a, b| a | b }; end
    def ^(mask); __mask_op(mask) { |a, b| a ^ b }; end

    def ~
      result = self.class.new(@size == 0 ? 0 : @size)
      i = 0
      while i < @size
        result.set_value(:U8, i, ~get_value(:U8, i) & 0xff)
        i += 1
      end
      result
    end

    def and!(mask); __mask_op!(mask) { |a, b| a & b }; end
    def or!(mask);  __mask_op!(mask) { |a, b| a | b }; end
    def xor!(mask); __mask_op!(mask) { |a, b| a ^ b }; end

    def __mask_op!(mask)
      unless mask.is_a?(IO::Buffer)
        raise TypeError, "wrong argument type #{__type_name(mask)} (expected IO::Buffer)"
      end
      raise MaskError, "Zero-length mask given!" if mask.size == 0
      __writable!
      i = 0
      msize = mask.size
      while i < @size
        set_value(:U8, i, yield(get_value(:U8, i), mask.get_value(:U8, i % msize)))
        i += 1
      end
      self
    end

    def not!
      __writable!
      i = 0
      while i < @size
        set_value(:U8, i, ~get_value(:U8, i) & 0xff)
        i += 1
      end
      self
    end

    # limit を与えると先頭 limit バイトだけ出し、残数を末尾に注記する
    # (#inspect が 256 バイトで打ち切るのに使う)。
    def hexdump(limit = nil)
      lines = []
      off = 0
      stop = (limit && limit < @size) ? limit : @size
      while off < stop
        n = stop - off
        n = 16 if n > 16
        hex = "".b
        asc = "".b
        j = 0
        while j < n
          b = get_value(:U8, off + j)
          hex << format("%02x ", b)
          asc << (b >= 32 && b < 127 ? b.chr : ".")
          j += 1
        end
        lines << format("0x%08x  %-47s %s", off, hex.rstrip, asc)
        off += 16
      end
      lines << "(and #{@size - stop} more bytes not printed)" if stop < @size
      lines.join("\n")
    end

    # CRuby の flag 表示順: NULL EXTERNAL INTERNAL MAPPED SHARED LOCKED PRIVATE
    # READONLY SLICE
    def __flags_string
      names = []
      names << "NULL" if @source.nil?
      names << "EXTERNAL" if external?
      names << "INTERNAL" if internal?
      names << "MAPPED" if mapped?
      names << "SHARED" if shared?
      names << "LOCKED" if locked?
      names << "PRIVATE" if private?
      names << "READONLY" if readonly?
      names << "SLICE" if @slice
      names.join(" ")
    end

    def to_s
      format("#<IO::Buffer 0x%016x+%d %s>", @addr, @size, __flags_string)
    end

    def inspect
      s = to_s
      s = s + "\n" + hexdump(256) if @size > 0 && @source
      s
    end

    def read(io, length = nil, offset = 0)
      __writable!
      data = length ? io.read(length) : io.read(@size - offset)
      return 0 if data.nil?
      set_string(data, offset)
    end

    def write(io, length = nil, offset = 0)
      length ||= @size - offset
      io.write(get_string(offset, length))
    end

    def pread(io, from, length = nil, offset = 0)
      __writable!
      pos = io.pos
      io.seek(from)
      data = io.read(length || (@size - offset))
      io.seek(pos)
      return 0 if data.nil?
      set_string(data, offset)
    end

    def pwrite(io, to, length = nil, offset = 0)
      pos = io.pos
      io.seek(to)
      n = io.write(get_string(offset, length || (@size - offset)))
      io.seek(pos)
      n
    end

    # 例外メッセージ用の型名: nil/true/false はクラス名でなくリテラル表記
    # (CRuby の rb_builtin_class_name と同じ)。
    def __type_name(obj)
      case obj
      when nil then "nil"
      when true then "true"
      when false then "false"
      else obj.class.to_s
      end
    end

    def __type(type)
      entry = DATA_TYPES[type]
      raise ArgumentError, "Invalid type name!" unless entry
      entry
    end

    def __bounds(offset, width)
      raise ArgumentError, "Offset can't be negative!" if offset < 0
      if offset + width > @size
        raise ArgumentError, "Type extends beyond end of buffer! (offset=#{offset} > size=#{@size})"
      end
    end

    def __writable!
      __valid!
      raise AccessError, "Buffer is not writable!" if readonly? || @source.nil?
    end

    private :__mask_op, :__mask_op!, :__type, :__type_name, :__bounds, :__writable!
  end
end
