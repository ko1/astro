# String.new(str, **kw) applies the keywords (encoding, capacity) to the copy
p String.new('<msg>不在</msg>', encoding: 'ASCII-8BIT').encoding
p String.new('abc', encoding: Encoding::BINARY).encoding, String.new('x', capacity: 10, encoding: 'BINARY').encoding
p String.new("abc".encode("UTF-16LE")).encoding, String.new("x", capacity: 100).encoding, String.new("é", encoding: "BINARY").bytesize
o = Object.new; def o.to_str = "zz"; p String.new(o, encoding: "BINARY").encoding

# OpenSSL::SSL names its error classes and VERIFY_* values (rescue clauses of HTTP clients)
require 'openssl'
p OpenSSL::SSL::SSLError.ancestors.take(3), OpenSSL::SSL::VERIFY_PEER, OpenSSL::SSL::VERIFY_NONE
p OpenSSL::SSL::SSLErrorWaitReadable.ancestors.include?(IO::WaitReadable)
begin; raise IOError, "x"; rescue OpenSSL::SSL::SSLError, IOError => e; p e.class; end
