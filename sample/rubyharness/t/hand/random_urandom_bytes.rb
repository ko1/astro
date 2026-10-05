# Random.urandom / SecureRandom (OS entropy through getrandom): sizes, encodings, formats, and that bytes vary
require "securerandom"
p Random.urandom(0), Random.urandom(16).bytesize, Random.urandom(16).encoding, Random.urandom(1000).bytesize
p SecureRandom.hex(16).size, SecureRandom.hex(16).match?(/\A\h{32}\z/), SecureRandom.uuid.match?(/\A\h{8}-\h{4}-4\h{3}-[89ab]\h{3}-\h{12}\z/)
p SecureRandom.random_bytes(10).bytesize, SecureRandom.base64(12).size, SecureRandom.urlsafe_base64(9).match?(/\A[\w-]+\z/)
p Array.new(20) { Random.urandom(8) }.uniq.size, Array.new(20) { SecureRandom.uuid }.uniq.size
p Random.new_seed.is_a?(Integer), (0...1).include?(SecureRandom.random_number), SecureRandom.random_number(10).between?(0, 9)
begin
  Random.urandom(-1)
rescue ArgumentError => e
  p e.class
end
