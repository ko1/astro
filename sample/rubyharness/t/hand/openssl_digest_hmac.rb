# koruby's openssl is a pure-Ruby digest/HMAC subset; these are the parts that
# must answer exactly what the real extension answers.
require "openssl"

MSG = "The quick brown fox jumps over the lazy dog"

%w[MD5 SHA1 SHA256 SHA384 SHA512].each do |alg|
  p [alg, OpenSSL::Digest.hexdigest(alg, MSG)]
  p [alg, OpenSSL::HMAC.hexdigest(alg, "key", MSG)]
end

p OpenSSL::Digest::SHA256.hexdigest("abc")
p OpenSSL::Digest.new("SHA256").update("a").update("bc").hexdigest
p OpenSSL::Digest.digest("SHA1", "abc").unpack1("H*")
p OpenSSL::Digest.new("sha256", "abc").hexdigest

# a key longer than the block is hashed first; a short one is zero-padded
p OpenSSL::HMAC.hexdigest("SHA256", "k" * 200, "x")
p OpenSSL::HMAC.hexdigest("SHA256", "", "")
p OpenSSL::HMAC.digest("SHA256", "key", MSG).bytesize

h = OpenSSL::HMAC.new("key", "SHA256")
h << "The quick brown "
h.update("fox jumps over the lazy dog")
p h.hexdigest
p h.hexdigest            # non-destructive
p h.to_s == h.hexdigest
h.reset
h << MSG
p h.hexdigest

p OpenSSL.fixed_length_secure_compare("abcd", "abcd")
p OpenSSL.fixed_length_secure_compare("abcd", "abce")
begin
  OpenSSL.fixed_length_secure_compare("a", "bb")
rescue ArgumentError => e
  p e.class
end
p OpenSSL.secure_compare("abc", "abc")
p OpenSSL.secure_compare("abc", "abcd")
p OpenSSL::Random.random_bytes(16).bytesize

# PBKDF2-HMAC (the one KDF koruby implements)
p OpenSSL::KDF.pbkdf2_hmac("secret", salt: "\x00".b * 16, iterations: 1000, length: 16, hash: "sha1").unpack1("H*")
p OpenSSL::KDF.pbkdf2_hmac("secret", salt: "", iterations: 100, length: 19, hash: "sha256").unpack1("H*")
p OpenSSL::KDF.pbkdf2_hmac("", salt: "salt", iterations: 10, length: 0, hash: "sha512")
p OpenSSL::PKCS5.pbkdf2_hmac_sha1("secret", "salt", 100, 16).unpack1("H*")
begin
  OpenSSL::KDF.pbkdf2_hmac(Object.new, salt: "s", iterations: 1, length: 1, hash: "sha1")
rescue TypeError => e
  p e.message
end
begin
  OpenSSL::KDF.pbkdf2_hmac("x", salt: "s", iterations: Object.new, length: 1, hash: "sha1")
rescue TypeError => e
  p e.message
end
