# MD5 / SHA1 / SHA256 / SHA384 / SHA512.  koruby has no OpenSSL binding, so the
# hashes are its own: the compression functions are C (builtins/digest.c, reached
# through __digest_*) and everything around them is here.  Instances buffer their
# input and hash on demand, which keeps #digest non-destructive the way
# Digest::Instance requires.
module Digest
  VOWELS     = "aeiouy"
  CONSONANTS = "bcdfghklmnprstvzx"

  # Coerce to a String the way the C digest does: String as-is, else #to_str.
  def self.__str(obj)
    return obj if obj.is_a?(String)
    raise TypeError, "no implicit conversion of #{obj.class} into String" unless obj.respond_to?(:to_str)
    s = obj.to_str
    raise TypeError, "can't convert #{obj.class} into String" unless s.is_a?(String)
    s
  end

  def self.hexencode(str)
    __str(str).b.unpack("H*")[0]
  end

  # Bubble Babble Binary Data Encoding (Huima).
  def self.bubblebabble(str)
    data = __str(str).b.bytes
    out = +"x"
    seed = 1
    rounds = data.size / 2 + 1
    i = 0
    while i < rounds
      if i + 1 < rounds || data.size.odd?
        b0 = data[2 * i]
        out << VOWELS[(((b0 >> 6) & 3) + seed) % 6]
        out << CONSONANTS[(b0 >> 2) & 15]
        out << VOWELS[((b0 & 3) + seed / 6) % 6]
        if i + 1 < rounds
          b1 = data[2 * i + 1]
          out << CONSONANTS[(b1 >> 4) & 15]
          out << "-"
          out << CONSONANTS[b1 & 15]
          seed = (seed * 5 + b0 * 7 + b1) % 36
        end
      else
        out << VOWELS[seed % 6] << CONSONANTS[16] << VOWELS[seed / 6]
      end
      i += 1
    end
    out << "x"
    out
  end

  # ---- Digest::Instance / Digest::Class --------------------------------
  module Instance
    def update(str)
      raise RuntimeError, "#{self.class} does not implement update()"
    end
    alias_method :<<, :update        # the same definition, as CRuby has it


    # No argument: the digest of everything fed so far (non-destructive).
    # With a String: reset, hash just that string, reset again.
    def digest(str = nil)
      return finish if str.nil?
      reset
      update(str)
      d = finish
      reset
      d
    end

    def digest!
      d = finish
      reset
      d
    end

    def hexdigest(str = nil) = Digest.hexencode(digest(str))
    def hexdigest! = Digest.hexencode(digest!)
    def base64digest(str = nil) = [digest(str)].pack("m0")
    def base64digest! = [digest!].pack("m0")
    def bubblebabble(str = nil) = Digest.bubblebabble(digest(str))
    def to_s = hexdigest
    def inspect = "#<#{self.class.name}: #{hexdigest}>"

    def ==(other)
      return digest == other.digest if other.is_a?(Digest::Instance)
      return false unless other.respond_to?(:to_str)
      hexdigest == other.to_str
    end

    def length = digest_length
    alias_method :size, :length      # CRuby: the same definition, not a copy
    def digest_length = digest.bytesize
    def block_length
      raise NotImplementedError, "#{self.class} does not implement block_length()"
    end

    # A fresh, reset instance of the same class.
    def new = self.class.new

    def file(name)
      File.open(name, "rb") do |f|
        while (chunk = f.read(16384))
          update(chunk)
        end
      end
      self
    end
  end

  class Class
    include Instance
    def self.digest(str, *a) = new(*a).digest(str)
    def self.hexdigest(str, *a) = new(*a).hexdigest(str)
    def self.base64digest(str, *a) = new(*a).base64digest(str)
    def self.bubblebabble(str, *a) = Digest.bubblebabble(new(*a).digest(str))
    def self.file(name, *a) = new(*a).file(name)
  end

  # Buffers input; each subclass supplies __digest(data) for the whole message.
  class Base < Class
    def initialize
      @buffer = +"".b
    end

    def initialize_copy(other)
      @buffer = other.__buffer.dup
    end

    protected def __buffer = @buffer

    def update(str)
      @buffer << Digest.__str(str).b
      self
    end
    alias_method :<<, :update        # the same definition, as CRuby has it

    def reset
      @buffer = +"".b
      self
    end

    def finish = self.class.__digest(@buffer)
    def digest_length = self.class::DIGEST_LENGTH
    def block_length = self.class::BLOCK_LENGTH
  end

  class MD5 < Base
    BLOCK_LENGTH = 64
    DIGEST_LENGTH = 16
    def self.__digest(data) = __digest_md5(data)
  end

  class SHA1 < Base
    BLOCK_LENGTH = 64
    DIGEST_LENGTH = 20
    def self.__digest(data) = __digest_sha1(data)
  end

  class SHA256 < Base
    BLOCK_LENGTH = 64
    DIGEST_LENGTH = 32
    def self.__digest(data) = __digest_sha256(data)
  end

  # SHA-512 and its truncated SHA-384 sibling differ only in the initial
  # state and the output length.
  class SHA512 < Base
    BLOCK_LENGTH = 128
    DIGEST_LENGTH = 64
    def self.__digest(data) = __digest_sha512(data)
  end

  class SHA384 < SHA512
    DIGEST_LENGTH = 48
    def self.__digest(data) = __digest_sha384(data)
  end

  # Digest::SHA2 dispatches on the requested bit length.
  class SHA2 < Class
    def initialize(bitlen = 256)
      @impl = case bitlen
              when 256 then SHA256.new
              when 384 then SHA384.new
              when 512 then SHA512.new
              else raise ArgumentError, "unsupported bit length: #{bitlen}"
              end
      @bitlen = bitlen
    end

    def initialize_copy(other)
      @impl = other.__impl.dup
      @bitlen = other.__bitlen
    end

    protected def __impl = @impl
    protected def __bitlen = @bitlen

    def update(str)
      @impl.update(str)
      self
    end
    alias_method :<<, :update        # the same definition, as CRuby has it

    def reset
      @impl.reset
      self
    end

    def finish = @impl.finish
    def digest_length = @impl.digest_length
    def block_length = @impl.block_length
    def new = self.class.new(@bitlen)
    def inspect = "#<#{self.class.name}:#{@bitlen} #{hexdigest}>"
  end
end

# Kernel#Digest("SHA256") — the class for a digest name, loading digest/<name>
# on demand (pstore picks its checksum algorithm this way).
def Digest(name)
  const = name.to_sym
  if Digest.const_defined?(const)
    Digest.const_get(const)
  else
    begin
      require "digest/#{const.to_s.downcase}"
    rescue LoadError
      raise LoadError, "library not found for class Digest::#{const} -- digest/#{const.to_s.downcase}"
    end
    raise LoadError, "library not found for class Digest::#{const} -- digest/#{const.to_s.downcase}" unless Digest.const_defined?(const)
    Digest.const_get(const)
  end
end
