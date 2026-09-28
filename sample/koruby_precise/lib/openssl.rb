# A pure-Ruby OpenSSL subset: message digests, HMAC, constant-time compare and
# the random-byte helpers.  koruby has no libcrypto binding, so the hashes come
# from lib/digest.rb.  Real cryptography (Cipher, PKey, X509, SSL sockets) is NOT here
# and is not faked: touching those constants raises, so a gem that needs TLS
# fails where it asks for it instead of somewhere far away.
#
# This covers what non-crypto gems actually reach for — `parallel` (and so
# rubocop) signs its worker pipe with HMAC-SHA256, and plenty of libraries want
# only a digest object.
require "digest"
require "securerandom"

module OpenSSL
  VERSION = "3.3.0"
  OPENSSL_VERSION = "koruby pure-Ruby OpenSSL subset"
  OPENSSL_LIBRARY_VERSION = OPENSSL_VERSION
  OPENSSL_VERSION_NUMBER = 0

  class OpenSSLError < StandardError; end

  # `OpenSSL::Digest::SHA256` and friends: the ::Digest implementation plus what
  # OpenSSL adds — #name, and an optional initial String on `new`.  Everything
  # else is ::Digest::Instance's, unchanged.
  class Digest
    class DigestError < OpenSSLError; end

    # name (as OpenSSL spells it) => the ::Digest class that implements it
    IMPLS = {
      "MD5" => ::Digest::MD5, "SHA1" => ::Digest::SHA1,
      "SHA256" => ::Digest::SHA256, "SHA384" => ::Digest::SHA384,
      "SHA512" => ::Digest::SHA512
    }.freeze

    ALGORITHMS = IMPLS.each_with_object({}) do |(name, impl), h|
      klass = Class.new(impl) do
        define_method(:name) { name }
        def initialize(data = nil)
          super()
          update(data) if data
        end
      end
      const_set(name, klass)
      h[name] = klass
    end.freeze

    # OpenSSL takes the algorithm as a name String or a digest INSTANCE; a
    # Symbol or a class is a TypeError, not a lookup.  (An instance answers
    # `is_a?(::Digest::Class)`, not `is_a?(OpenSSL::Digest)`: koruby reuses the
    # ::Digest implementations rather than reimplementing the hashes, so the
    # subclasses descend from those instead of from this factory.)
    def self.resolve(algorithm)
      return algorithm.class if algorithm.is_a?(::Digest::Class)
      raise TypeError, "wrong argument type #{algorithm.class} (expected String)" unless algorithm.is_a?(String)
      ALGORITHMS[algorithm.upcase.delete("-_")] ||
        raise(DigestError, "Unsupported digest algorithm (#{algorithm}).")
    end

    def self.new(algorithm, data = nil) = resolve(algorithm).new(data)

    def self.digest(algorithm, data) = resolve(algorithm).digest(data)
    def self.hexdigest(algorithm, data) = resolve(algorithm).hexdigest(data)
    def self.base64digest(algorithm, data) = resolve(algorithm).base64digest(data)
  end

  # Keyed hashing (RFC 2104).  `digest` is a name or a Digest class.
  class HMAC
    def self.digest(digest, key, data) = new(key, digest).update(data).digest
    def self.hexdigest(digest, key, data) = new(key, digest).update(data).hexdigest
    def self.base64digest(digest, key, data) = new(key, digest).update(data).base64digest

    def initialize(key, digest)
      @impl = OpenSSL::Digest.resolve(digest)
      block = @impl.new.block_length
      key = @impl.digest(key) if key.bytesize > block
      key = key.b.ljust(block, "\x00")
      @ipad = xor_pad(key, 0x36)
      @opad = xor_pad(key, 0x5c)
      reset
    end

    def update(data)
      @inner.update(data)
      self
    end
    alias << update

    def digest = @impl.digest(@opad + @inner.digest)
    def hexdigest = digest.unpack1("H*")
    alias to_s hexdigest
    alias inspect hexdigest

    def base64digest = [digest].pack("m0")

    def reset
      @inner = @impl.new
      @inner.update(@ipad)
      self
    end

    def initialize_copy(other)
      super
      @inner = other.instance_variable_get(:@inner).dup
    end

    private

    def xor_pad(key, byte)
      key.bytes.map! { |b| b ^ byte }.pack("C*")
    end
  end

  # Byte-by-byte compare that does not return early — the point is that the
  # time it takes says nothing about where the first difference is.
  def self.fixed_length_secure_compare(a, b)
    a = __str(a).b
    b = __str(b).b
    raise ArgumentError, "inputs must be of equal length" if a.bytesize != b.bytesize
    diff = 0
    a.each_byte.with_index { |x, i| diff |= x ^ b.getbyte(i) }
    diff.zero?
  end

  # Lengths may differ here: both sides are hashed first.  The `& (a == b)` is
  # OpenSSL's: two different objects that merely #to_str alike are not equal.
  def self.secure_compare(a, b)
    hashed_a = ::Digest::SHA256.digest(__str(a))
    hashed_b = ::Digest::SHA256.digest(__str(b))
    fixed_length_secure_compare(hashed_a, hashed_b) & (a == b)
  end

  # The Integer/String coercions the C extension does with StringValue/NUM2INT.
  def self.__str(v)
    return v if v.is_a?(String)
    raise TypeError, "no implicit conversion of #{v.class} into String" unless v.respond_to?(:to_str)
    s = v.to_str
    raise TypeError, "can't convert #{v.class} to String" unless s.is_a?(String)
    s
  end

  def self.__int(v)
    return v if v.is_a?(Integer)
    raise TypeError, "no implicit conversion of #{v.class} into Integer" unless v.respond_to?(:to_int)
    i = v.to_int
    raise TypeError, "can't convert #{v.class} to Integer" unless i.is_a?(Integer)
    i
  end

  # PBKDF2-HMAC (RFC 8018 §5.2).  Password hashing in the gems that use it
  # (rodauth, devise) goes through here, so it is worth having even without
  # libcrypto; scrypt and the rest are not implemented.
  module KDF
    class KDFError < OpenSSLError; end

    def self.pbkdf2_hmac(pass, salt:, iterations:, length:, hash:)
      pass = OpenSSL.__str(pass)
      salt = OpenSSL.__str(salt)
      iterations = OpenSSL.__int(iterations)
      length = OpenSSL.__int(length)
      raise TypeError, "wrong argument type #{hash.class} (expected String)" unless
        hash.is_a?(String) || hash.is_a?(::Digest::Class)
      return "".b if length <= 0
      mac = HMAC.new(pass, hash)
      hlen = OpenSSL::Digest.resolve(hash).new.digest_length
      out = +"".b
      (1..(length + hlen - 1) / hlen).each do |i|
        u = mac.reset.update(salt).update([i].pack("N")).digest
        t = u
        (iterations - 1).times do
          u = mac.reset.update(u).digest
          t = t.bytes.each_with_index.map { |b, j| b ^ u.getbyte(j) }.pack("C*")
        end
        out << t
      end
      out[0, length]
    end
  end

  # The pre-2.x spelling, still in wide use.
  module PKCS5
    def self.pbkdf2_hmac(pass, salt, iterations, length, hash) =
      KDF.pbkdf2_hmac(pass, salt: salt, iterations: iterations, length: length, hash: hash)

    def self.pbkdf2_hmac_sha1(pass, salt, iterations, length) =
      KDF.pbkdf2_hmac(pass, salt: salt, iterations: iterations, length: length, hash: "SHA1")
  end

  module Random
    def self.random_bytes(n) = SecureRandom.bytes(n)
    def self.bytes(n) = SecureRandom.bytes(n)
    def self.pseudo_bytes(n) = SecureRandom.bytes(n)
    def self.seed(_str) = nil
    def self.status? = true
  end

  # OpenSSL::SSL holds only what non-TLS code names: the error classes (HTTP
  # clients list OpenSSL::SSL::SSLError in rescue clauses that run on every
  # failure, faraday's net_http adapter among them) and the VERIFY_* values.
  # SSLContext / SSLSocket stay absent, so TLS still fails where it is asked for.
  module SSL
    class SSLError < OpenSSLError; end
    class SSLErrorWaitReadable < SSLError; include IO::WaitReadable; end
    class SSLErrorWaitWritable < SSLError; include IO::WaitWritable; end
    VERIFY_NONE = 0
    VERIFY_PEER = 1
    VERIFY_FAIL_IF_NO_PEER_CERT = 2
    VERIFY_CLIENT_ONCE = 4

    def self.const_missing(name)
      raise NameError.new("uninitialized constant OpenSSL::SSL::#{name} " \
                          "(koruby's openssl is a pure-Ruby digest/HMAC subset: no libcrypto)",
                          name)
    end
  end

  # Everything else (Cipher, PKey, X509, SSL contexts, ...) is absent, and stays absent:
  # `defined?` answers nil and the constant raises NameError, the same shapes a
  # feature probe expects.  Only the message says why.
  def self.const_missing(name)
    raise NameError.new("uninitialized constant OpenSSL::#{name} " \
                        "(koruby's openssl is a pure-Ruby digest/HMAC subset: no libcrypto)",
                        name)
  end
end
