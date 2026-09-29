# Random::Formatter is a core module (rand / random_number over #bytes) that Random
# includes and extends; lib/random/formatter.rb reopens it (ruby-bench activerecord
# calls Random.alphanumeric).  Run under CRuby and koruby and diff.
p Random::Formatter.instance_methods(false).sort
p Random.ancestors.include?(Random::Formatter), Random.singleton_class.include?(Random::Formatter)
srand(5); p Random.rand, Random.rand(100), Random.new(7).rand(1.0), Random.random_number(10)
require "random/formatter"
p Random.alphanumeric(8).size, Random.new(1).alphanumeric(5).size, Random.hex(4).size, Random.uuid.size
require "securerandom"
p SecureRandom.alphanumeric(6).size, SecureRandom.rand(10).class, SecureRandom.random_number.class
p SecureRandom.random_number(1..3).between?(1, 3), SecureRandom.rand(2.5).class, SecureRandom.random_number(-1).class
